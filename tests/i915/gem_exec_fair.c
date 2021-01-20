/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2016 Intel Corporation
 */

#include "config.h"

#include <pthread.h>
#include <sys/poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sched.h>
#include <signal.h>
#include <unistd.h>

#include "sync_file.h"

#include "i915/gem.h"
#include "igt.h"
#include "igt_rand.h"
#include "igt_rapl.h"
#include "igt_sysfs.h"
#include "igt_syncobj.h"
#include "igt_vgem.h"
#include "ioctl_wrappers.h"
#include "sw_sync.h"

IGT_TEST_DESCRIPTION("Check that GPU time and execution order is fairly distributed across clients");

#define NSEC64 ((uint64_t)NSEC_PER_SEC)

static int has_secure_batches(int i915)
{
	int v = -1;
	drm_i915_getparam_t gp = {
		.param = I915_PARAM_HAS_SECURE_BATCHES,
		.value = &v,
	};

	drmIoctl(i915, DRM_IOCTL_I915_GETPARAM, &gp);

	return v > 0;
}

static bool has_mi_math(int i915, const struct intel_execution_engine2 *e)
{
	uint32_t devid = intel_get_drm_devid(i915);

	if (intel_gen(devid) >= 8)
		return true;

	if (!IS_HASWELL(devid))
		return false;

	if (!has_secure_batches(i915))
		return false;

	return e == NULL || e->class == I915_ENGINE_CLASS_RENDER;
}

static unsigned int offset_in_page(void *addr)
{
	return (uintptr_t)addr & 4095;
}

static uint32_t __batch_create(int i915, uint32_t offset)
{
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	uint32_t handle;

	handle = gem_create(i915, ALIGN(offset + 4, 4096));
	gem_write(i915, handle, offset, &bbe, sizeof(bbe));

	return handle;
}

static uint32_t batch_create(int i915)
{
	return __batch_create(i915, 0);
}

static int read_timestamp_frequency(int i915)
{
	int value = 0;
	drm_i915_getparam_t gp = {
		.value = &value,
		.param = I915_PARAM_CS_TIMESTAMP_FREQUENCY,
	};
	ioctl(i915, DRM_IOCTL_I915_GETPARAM, &gp);
	return value;
}

static uint64_t div64_u64_round_up(uint64_t x, uint64_t y)
{
	return (x + y - 1) / y;
}

static bool is_icelake(int i915)
{
	return intel_get_device_info(intel_get_drm_devid(i915))->is_icelake;
}

static uint64_t ns_to_ctx_ticks(int i915, uint64_t ns)
{
	int f = read_timestamp_frequency(i915);
	if (is_icelake(i915))
		f = 12500000; /* icl!!! are you feeling alright? CTX vs CS */
	return div64_u64_round_up(ns * f, NSEC64);
}

static uint64_t ticks_to_ns(int i915, uint64_t ticks)
{
	return div64_u64_round_up(ticks * NSEC64,
				  read_timestamp_frequency(i915));
}

#define MI_INSTR(opcode, flags) (((opcode) << 23) | (flags))

#define MI_MATH(x)                      MI_INSTR(0x1a, (x) - 1)
#define MI_MATH_INSTR(opcode, op1, op2) ((opcode) << 20 | (op1) << 10 | (op2))
/* Opcodes for MI_MATH_INSTR */
#define   MI_MATH_NOOP                  MI_MATH_INSTR(0x000, 0x0, 0x0)
#define   MI_MATH_LOAD(op1, op2)        MI_MATH_INSTR(0x080, op1, op2)
#define   MI_MATH_LOADINV(op1, op2)     MI_MATH_INSTR(0x480, op1, op2)
#define   MI_MATH_LOAD0(op1)            MI_MATH_INSTR(0x081, op1)
#define   MI_MATH_LOAD1(op1)            MI_MATH_INSTR(0x481, op1)
#define   MI_MATH_ADD                   MI_MATH_INSTR(0x100, 0x0, 0x0)
#define   MI_MATH_SUB                   MI_MATH_INSTR(0x101, 0x0, 0x0)
#define   MI_MATH_AND                   MI_MATH_INSTR(0x102, 0x0, 0x0)
#define   MI_MATH_OR                    MI_MATH_INSTR(0x103, 0x0, 0x0)
#define   MI_MATH_XOR                   MI_MATH_INSTR(0x104, 0x0, 0x0)
#define   MI_MATH_STORE(op1, op2)       MI_MATH_INSTR(0x180, op1, op2)
#define   MI_MATH_STOREINV(op1, op2)    MI_MATH_INSTR(0x580, op1, op2)
/* Registers used as operands in MI_MATH_INSTR */
#define   MI_MATH_REG(x)                (x)
#define   MI_MATH_REG_SRCA              0x20
#define   MI_MATH_REG_SRCB              0x21
#define   MI_MATH_REG_ACCU              0x31
#define   MI_MATH_REG_ZF                0x32
#define   MI_MATH_REG_CF                0x33

#define MI_LOAD_REGISTER_REG    MI_INSTR(0x2A, 1)

static void delay(int i915,
		  const struct intel_execution_engine2 *e,
		  uint32_t handle,
		  uint64_t addr,
		  uint64_t ns)
{
	const int use_64b = intel_gen(intel_get_drm_devid(i915)) >= 8;
	const uint32_t base = gem_engine_mmio_base(i915, e->name);
	const uint32_t runtime = base + (use_64b ? 0x3a8 : 0x358);
#define CS_GPR(x) (base + 0x600 + 8 * (x))
	enum { START_TS, NOW_TS };
	uint32_t *map, *cs, *jmp;

	igt_require(base);
	igt_assert(use_64b || (addr >> 32) == 0);

	/* Loop until CTX_TIMESTAMP - initial > @ns */

	cs = map = gem_mmap__device_coherent(i915, handle, 0, 4096, PROT_WRITE);

	*cs++ = MI_LOAD_REGISTER_IMM;
	*cs++ = CS_GPR(START_TS) + 4;
	*cs++ = 0;
	*cs++ = MI_LOAD_REGISTER_REG;
	*cs++ = runtime;
	*cs++ = CS_GPR(START_TS);

	while (offset_in_page(cs) & 63)
		*cs++ = 0;
	jmp = cs;

	*cs++ = 0x5 << 23; /* MI_ARB_CHECK */

	*cs++ = MI_LOAD_REGISTER_IMM;
	*cs++ = CS_GPR(NOW_TS) + 4;
	*cs++ = 0;
	*cs++ = MI_LOAD_REGISTER_REG;
	*cs++ = runtime;
	*cs++ = CS_GPR(NOW_TS);

	/* delta = now - start; inverted to match COND_BBE */
	*cs++ = MI_MATH(4);
	*cs++ = MI_MATH_LOAD(MI_MATH_REG_SRCA, MI_MATH_REG(NOW_TS));
	*cs++ = MI_MATH_LOAD(MI_MATH_REG_SRCB, MI_MATH_REG(START_TS));
	*cs++ = MI_MATH_SUB;
	*cs++ = MI_MATH_STOREINV(MI_MATH_REG(NOW_TS), MI_MATH_REG_ACCU);

	/* Save delta for reading by COND_BBE */
	*cs++ = 0x24 << 23 | (1 + use_64b); /* SRM */
	*cs++ = CS_GPR(NOW_TS);
	*cs++ = addr + 4000;
	*cs++ = addr >> 32;

	/* Delay between SRM and COND_BBE to post the writes */
	for (int n = 0; n < 8; n++) {
		*cs++ = MI_STORE_DWORD_IMM;
		if (use_64b) {
			*cs++ = addr + 4064;
			*cs++ = addr >> 32;
		} else {
			*cs++ = 0;
			*cs++ = addr + 4064;
		}
		*cs++ = 0;
	}

	/* Break if delta [time elapsed] > ns */
	*cs++ = MI_COND_BATCH_BUFFER_END | MI_DO_COMPARE | (1 + use_64b);
	*cs++ = ~ns_to_ctx_ticks(i915, ns);
	*cs++ = addr + 4000;
	*cs++ = addr >> 32;

	/* Otherwise back to recalculating delta */
	*cs++ = MI_BATCH_BUFFER_START | 1 << 8 | use_64b;
	*cs++ = addr + offset_in_page(jmp);
	*cs++ = addr >> 32;

	munmap(map, 4096);
}

static struct drm_i915_gem_exec_object2
delay_create(int i915, uint32_t ctx,
	     const struct intel_execution_engine2 *e,
	     uint64_t target_ns)
{
	struct drm_i915_gem_exec_object2 obj = {
		.handle = batch_create(i915),
		.flags = EXEC_OBJECT_SUPPORTS_48B_ADDRESS,
	};
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(&obj),
		.buffer_count = 1,
		.rsvd1 = ctx,
		.flags = e->flags,
	};

	obj.offset = obj.handle << 12;
	gem_execbuf(i915, &execbuf);
	gem_sync(i915, obj.handle);

	delay(i915, e, obj.handle, obj.offset, target_ns);

	obj.flags |= EXEC_OBJECT_PINNED;
	return obj;
}

static void tslog(int i915,
		  const struct intel_execution_engine2 *e,
		  uint32_t handle,
		  uint64_t addr)
{
	const int use_64b = intel_gen(intel_get_drm_devid(i915)) >= 8;
	const uint32_t base = gem_engine_mmio_base(i915, e->name);
#define CS_GPR(x) (base + 0x600 + 8 * (x))
#define CS_TIMESTAMP (base + 0x358)
	enum { INC, MASK, ADDR };
	uint32_t *timestamp_lo, *addr_lo;
	uint32_t *map, *cs;

	igt_require(base);
	igt_assert(use_64b || (addr >> 32) == 0);

	map = gem_mmap__device_coherent(i915, handle, 0, 4096, PROT_WRITE);
	cs = map + 512;

	/* Record the current CS_TIMESTAMP into a journal [a 512 slot ring]. */
	*cs++ = 0x24 << 23 | (1 + use_64b); /* SRM */
	*cs++ = CS_TIMESTAMP;
	timestamp_lo = cs;
	*cs++ = addr;
	*cs++ = addr >> 32;

	/* Load the address + inc & mask variables */
	*cs++ = MI_LOAD_REGISTER_IMM;
	*cs++ = CS_GPR(ADDR);
	addr_lo = cs;
	*cs++ = addr;
	*cs++ = MI_LOAD_REGISTER_IMM;
	*cs++ = CS_GPR(ADDR) + 4;
	*cs++ = addr >> 32;

	*cs++ = MI_LOAD_REGISTER_IMM;
	*cs++ = CS_GPR(INC);
	*cs++ = 4;
	*cs++ = MI_LOAD_REGISTER_IMM;
	*cs++ = CS_GPR(INC) + 4;
	*cs++ = 0;

	*cs++ = MI_LOAD_REGISTER_IMM;
	*cs++ = CS_GPR(MASK);
	*cs++ = 0xfffff7ff;
	*cs++ = MI_LOAD_REGISTER_IMM;
	*cs++ = CS_GPR(MASK) + 4;
	*cs++ = 0xffffffff;

	/* Increment the [ring] address for saving CS_TIMESTAMP */
	*cs++ = MI_MATH(8);
	*cs++ = MI_MATH_LOAD(MI_MATH_REG_SRCA, MI_MATH_REG(INC));
	*cs++ = MI_MATH_LOAD(MI_MATH_REG_SRCB, MI_MATH_REG(ADDR));
	*cs++ = MI_MATH_ADD;
	*cs++ = MI_MATH_STORE(MI_MATH_REG(ADDR), MI_MATH_REG_ACCU);
	*cs++ = MI_MATH_LOAD(MI_MATH_REG_SRCA, MI_MATH_REG(ADDR));
	*cs++ = MI_MATH_LOAD(MI_MATH_REG_SRCB, MI_MATH_REG(MASK));
	*cs++ = MI_MATH_AND;
	*cs++ = MI_MATH_STORE(MI_MATH_REG(ADDR), MI_MATH_REG_ACCU);

	/* Rewrite the batch buffer for the next execution */
	*cs++ = 0x24 << 23 | (1 + use_64b); /* SRM */
	*cs++ = CS_GPR(ADDR);
	*cs++ = addr + offset_in_page(timestamp_lo);
	*cs++ = addr >> 32;
	*cs++ = 0x24 << 23 | (1 + use_64b); /* SRM */
	*cs++ = CS_GPR(ADDR);
	*cs++ = addr + offset_in_page(addr_lo);
	*cs++ = addr >> 32;

	*cs++ = MI_BATCH_BUFFER_END;

	munmap(map, 4096);
}

static struct drm_i915_gem_exec_object2
tslog_create(int i915, uint32_t ctx, const struct intel_execution_engine2 *e)
{
	struct drm_i915_gem_exec_object2 obj = {
		.handle = batch_create(i915),
		.flags = EXEC_OBJECT_SUPPORTS_48B_ADDRESS,
	};
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(&obj),
		.buffer_count = 1,
		.rsvd1 = ctx,
		.flags = e->flags,
	};

	obj.offset = obj.handle << 12;
	gem_execbuf(i915, &execbuf);
	gem_sync(i915, obj.handle);

	tslog(i915, e, obj.handle, obj.offset);

	obj.flags |= EXEC_OBJECT_PINNED;
	return obj;
}

static int cmp_u32(const void *A, const void *B)
{
	const uint32_t *a = A, *b = B;

	if (*a < *b)
		return -1;
	else if (*a > *b)
		return 1;
	else
		return 0;
}

static uint32_t
read_ctx_timestamp(int i915, const struct intel_execution_engine2 *e)
{
	struct drm_i915_gem_relocation_entry reloc;
	struct drm_i915_gem_exec_object2 obj = {
		.handle = gem_create(i915, 4096),
		.offset = 32 << 20,
		.relocs_ptr = to_user_pointer(&reloc),
		.relocation_count = 1,
	};
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(&obj),
		.buffer_count = 1,
		.flags = e->flags,
	};
	const int use_64b = intel_gen(intel_get_drm_devid(i915)) >= 8;
	const uint32_t base = gem_engine_mmio_base(i915, e->name);
	const uint32_t runtime = base + (use_64b ? 0x3a8 : 0x358);
	uint32_t *map, *cs;
	uint32_t ts;

	cs = map = gem_mmap__device_coherent(i915, obj.handle,
					     0, 4096, PROT_WRITE);

	*cs++ = 0x24 << 23 | (1 + use_64b); /* SRM */
	*cs++ = runtime;

	memset(&reloc, 0, sizeof(reloc));
	reloc.target_handle = obj.handle;
	reloc.presumed_offset = obj.offset;
	reloc.offset = offset_in_page(cs);
	reloc.delta = 4000;
	*cs++ = obj.offset + 4000;
	*cs++ = obj.offset >> 32;

	*cs++ = MI_BATCH_BUFFER_END;

	gem_execbuf(i915, &execbuf);
	gem_sync(i915, obj.handle);
	ts = map[1000];

	if (!ts) {
		/* Twice for good luck (and avoid chance 0) */
		gem_execbuf(i915, &execbuf);
		gem_sync(i915, obj.handle);
		ts = map[1000];
	}

	gem_close(i915, obj.handle);
	munmap(map, 4096);

	return ts;
}

static bool has_ctx_timestamp(int i915, const struct intel_execution_engine2 *e)
{
	const int gen = intel_gen(intel_get_drm_devid(i915));

	if (gen == 8 && e->class == I915_ENGINE_CLASS_VIDEO)
		return false; /* looks fubar */

	return read_ctx_timestamp(i915, e);
}

static struct intel_execution_engine2
pick_random_engine(int i915, const struct intel_execution_engine2 *not)
{
	const struct intel_execution_engine2 *e;
	unsigned int count = 0;

	__for_each_physical_engine(i915, e) {
		if (e->flags == not->flags)
			continue;
		if (!gem_class_has_mutable_submission(i915, e->class))
			continue;
		count++;
	}
	if (!count)
		return *not;

	count = rand() % count;
	__for_each_physical_engine(i915, e) {
		if (e->flags == not->flags)
			continue;
		if (!gem_class_has_mutable_submission(i915, e->class))
			continue;
		if (!count--)
			break;
	}

	return *e;
}

static void fair_child(int i915, uint32_t ctx,
		       const struct intel_execution_engine2 *e,
		       uint64_t frame_ns,
		       int timeline,
		       uint32_t common,
		       unsigned int flags,
		       unsigned long *ctl,
		       unsigned long *median,
		       unsigned long *iqr,
		       int sv, int rv)
#define F_SYNC		(1 << 0)
#define F_PACE		(1 << 1)
#define F_FLOW		(1 << 2)
#define F_HALF		(1 << 3)
#define F_SOLO		(1 << 4)
#define F_SPARE		(1 << 5)
#define F_NEXT		(1 << 6)
#define F_VIP		(1 << 7)
#define F_RRUL		(1 << 8)
#define F_SHARE		(1 << 9)
#define F_PING		(1 << 10)
#define F_THROTTLE	(1 << 11)
#define F_ISOLATE	(1 << 12)
{
	const int batches_per_frame = flags & F_SOLO ? 1 : 3;
	struct drm_i915_gem_exec_object2 obj[4] = {
		{},
		{
			.handle = common ?: gem_create(i915, 4096),
		},
		delay_create(i915, ctx, e, frame_ns / batches_per_frame),
		delay_create(i915, ctx, e, frame_ns / batches_per_frame),
	};
	struct intel_execution_engine2 ping = *e;
	int p_fence = -1, n_fence = -1;
	unsigned long count = 0;
	unsigned int aux_flags;
	int n;

	srandom(getpid());
	if (flags & F_PING)
		ping = pick_random_engine(i915, e);
	obj[0] = tslog_create(i915, ctx, &ping);

	/* Synchronize with other children/parent upon construction */
	if (sv != -1)
		write(sv, &p_fence, sizeof(p_fence));
	if (rv != -1)
		read(rv, &p_fence, sizeof(p_fence));
	igt_assert(p_fence == -1);

	aux_flags = 0;
	if (intel_gen(intel_get_drm_devid(i915)) < 8)
		aux_flags = I915_EXEC_SECURE;
	ping.flags |= aux_flags;
	aux_flags |= e->flags;

	while (!READ_ONCE(*ctl)) {
		struct drm_i915_gem_execbuffer2 execbuf = {
			.buffers_ptr = to_user_pointer(obj),
			.buffer_count = 3,
			.rsvd1 = ctx,
			.rsvd2 = -1,
			.flags = aux_flags,
		};

		if (flags & F_FLOW) {
			unsigned int seq;

			seq = count;
			if (flags & F_NEXT)
				seq++;

			execbuf.rsvd2 =
				sw_sync_timeline_create_fence(timeline, seq);
			execbuf.flags |= I915_EXEC_FENCE_IN;
		}

		execbuf.flags |= I915_EXEC_FENCE_OUT;
		gem_execbuf_wr(i915, &execbuf);
		n_fence = execbuf.rsvd2 >> 32;
		execbuf.flags &= ~(I915_EXEC_FENCE_OUT | I915_EXEC_FENCE_IN);
		for (n = 1; n < batches_per_frame; n++)
			gem_execbuf(i915, &execbuf);
		close(execbuf.rsvd2);

		execbuf.buffer_count = 1;
		execbuf.batch_start_offset = 2048;
		execbuf.flags = ping.flags | I915_EXEC_FENCE_IN;
		execbuf.rsvd2 = n_fence;
		gem_execbuf(i915, &execbuf);

		if (flags & F_PACE && p_fence != -1) {
			struct pollfd pfd = {
				.fd = p_fence,
				.events = POLLIN,
			};
			poll(&pfd, 1, -1);
		}
		close(p_fence);

		if (flags & F_SYNC) {
			struct pollfd pfd = {
				.fd = n_fence,
				.events = POLLIN,
			};
			poll(&pfd, 1, -1);
		}

		if (flags & F_THROTTLE)
			igt_ioctl(i915, DRM_IOCTL_I915_GEM_THROTTLE, 0);

		igt_swap(obj[2], obj[3]);
		igt_swap(p_fence, n_fence);
		count++;
	}
	close(p_fence);

	gem_close(i915, obj[3].handle);
	gem_close(i915, obj[2].handle);
	if (obj[1].handle != common)
		gem_close(i915, obj[1].handle);

	gem_sync(i915, obj[0].handle);
	if (median) {
		uint32_t *map;

		/*
		 * We recorded the CS_TIMESTAMP of each frame, and if
		 * the GPU is being shared completely fairly, we expect
		 * each frame to be at the same interval from the last.
		 *
		 * Compute the interval between frames and report back
		 * both the median interval and the range for this client.
		 */

		map = gem_mmap__device_coherent(i915, obj[0].handle,
						0, 4096, PROT_WRITE);
		igt_assert(map[0]);
		for (n = 1; n < min(count, 512); n++) {
			igt_assert(map[n]);
			map[n - 1] = map[n] - map[n - 1];
		}
		qsort(map, --n, sizeof(*map), cmp_u32);
		*iqr = ticks_to_ns(i915, map[(3 * n + 3) / 4] - map[n / 4]);
		*median = ticks_to_ns(i915, map[n / 2]);
		munmap(map, 4096);
	}
	gem_close(i915, obj[0].handle);
}

static int cmp_ul(const void *A, const void *B)
{
	const unsigned long *a = A, *b = B;

	if (*a < *b)
		return -1;
	else if (*a > *b)
		return 1;
	else
		return 0;
}

static uint64_t d_cpu_time(const struct rusage *a, const struct rusage *b)
{
	uint64_t cpu_time = 0;

	cpu_time += (a->ru_utime.tv_sec - b->ru_utime.tv_sec) * NSEC64;
	cpu_time += (a->ru_utime.tv_usec - b->ru_utime.tv_usec) * 1000;

	cpu_time += (a->ru_stime.tv_sec - b->ru_stime.tv_sec) * NSEC64;
	cpu_time += (a->ru_stime.tv_usec - b->ru_stime.tv_usec) * 1000;

	return cpu_time;
}

static void timeline_advance(int timeline, int delay_ns)
{
	struct timespec tv = { .tv_nsec = delay_ns };
	nanosleep(&tv, NULL);
	sw_sync_timeline_inc(timeline, 1);
}

static void fairness(int i915,
		     const struct intel_execution_engine2 *e,
		     int duration, unsigned int flags)
{
	const int frame_ns = 16666 * 1000;
	const int fence_ns = flags & F_HALF ? 2 * frame_ns : frame_ns;
	unsigned long *result, *iqr;
	uint32_t common = 0;
	struct {
		int child[2];
		int parent[2];
	} lnk;

	igt_require(has_ctx_timestamp(i915, e));
	igt_require(gem_class_has_mutable_submission(i915, e->class));
	if (flags & (F_ISOLATE | F_PING))
		igt_require(intel_gen(intel_get_drm_devid(i915)) >= 8);

	igt_assert(pipe(lnk.child) == 0);
	igt_assert(pipe(lnk.parent) == 0);

	if (flags & F_SHARE)
		common = gem_create(i915, 4095);

	result = mmap(NULL, 4096, PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
	igt_assert(result != MAP_FAILED);
	iqr = mmap(NULL, 4096, PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
	igt_assert(iqr != MAP_FAILED);

	/*
	 * The combined workload always runs at a 60fps target (unless F_HALF!).
	 * This gives a frame of interval of 16ms that is evenly split across
	 * all the clients, so simulating a system with a bunch of clients that
	 * are perfectly balanced and can sustain 60fps. Our job is to ensure
	 * that each client does run at a smooth 60fps.
	 *
	 * Each client runs a fixed length delay loop (as a single request,
	 * or split into 3) and then records the CS_TIMESTAMP after completing
	 * its delay. Given a fair allotment of GPU time to each client,
	 * that timestamp will [ideally] be at a precise 16ms intervals.
	 * In practice, time is wasted on context switches, so as the number
	 * of clients increases, the proprotion of time spent on context
	 * switches grows. As we get to 64 render clients, we will be spending
	 * as much time in context switches as executing the client workloads.
	 *
	 * Each client frame may be paced by some throttling technique found
	 * in the wild. i.e. each client may wait until a simulated vblank
	 * to indicate the start of a new frame, or it may wait until the
	 * completion of a previous frame. This causes submission from each
	 * client and across the system to be chunky and uneven.
	 *
	 * We look at the variation of frame intervals within each client, and
	 * the variation of the medians across the clients to see if the
	 * distribution (budget) of GPU time was fair enough.
	 *
	 * Alternative (and important) metrics will be more latency centric;
	 * looking at how well we can sustain meeting deadline given competition
	 * by clients for the GPU.
	 */

	for (int n = 2; n <= 256; n <<= 1) { /* 32 == 500us per client */
		int timeline = sw_sync_timeline_create();
		int nfences = duration * NSEC64 / fence_ns + 1;
		int nchild = n - 1; /* odd for easy medians */
		const int child_ns = frame_ns / (nchild + !!(flags & F_SPARE));
		const int lo = nchild / 4;
		const int hi = (3 * nchild + 3) / 4 - 1;
		struct rusage old_usage, usage;
		uint64_t cpu_time, d_time;
		struct timespec tv;
		struct igt_mean m;

		memset(result, 0, (nchild + 1) * sizeof(result[0]));

		if (flags & F_PING) { /* fill the others with light bg load */
			struct intel_execution_engine2 *ping;

			__for_each_physical_engine(i915, ping) {
				if (ping->flags == e->flags)
					continue;

				igt_fork(child, 1) {
					uint32_t ctx = gem_context_clone_with_engines(i915, 0);

					fair_child(i915, ctx, ping,
						   child_ns / 8,
						   -1, common,
						   F_SOLO | F_PACE | F_SHARE,
						   &result[nchild],
						   NULL, NULL, -1, -1);

					gem_context_destroy(i915, ctx);
				}
			}
		}

		getrusage(RUSAGE_CHILDREN, &old_usage);
		igt_nsec_elapsed(memset(&tv, 0, sizeof(tv)));
		igt_fork(child, nchild) {
			uint32_t ctx;

			if (flags & F_ISOLATE) {
				int clone, dmabuf = -1;

				if (common)
					dmabuf = prime_handle_to_fd(i915, common);

				clone = gem_reopen_driver(i915);
				gem_context_copy_engines(i915, 0, clone, 0);
				i915 = clone;

				if (dmabuf != -1)
					common = prime_fd_to_handle(i915, dmabuf);
			}

			ctx = gem_context_clone_with_engines(i915, 0);

			if (flags & F_VIP && child == 0) {
				gem_context_set_priority(i915, ctx, 1023);
				flags |= F_FLOW;
			}
			if (flags & F_RRUL && child == 0)
				flags |= F_SOLO | F_FLOW | F_SYNC;

			fair_child(i915, ctx, e, child_ns,
				   timeline, common, flags,
				   &result[nchild],
				   &result[child], &iqr[child],
				   lnk.child[1], lnk.parent[0]);

			gem_context_destroy(i915, ctx);
		}

		{
			int sync;
			for (int child = 0; child < nchild; child++)
				read(lnk.child[0], &sync, sizeof(sync));
			for (int child = 0; child < nchild; child++)
				write(lnk.parent[1], &sync, sizeof(sync));
		}

		while (nfences--)
			timeline_advance(timeline, fence_ns);

		result[nchild] = 1;
		for (int child = 0; child < nchild; child++) {
			while (!READ_ONCE(result[child]))
				timeline_advance(timeline, fence_ns);
		}

		igt_waitchildren();
		close(timeline);

		/*
		 * Are we running out of CPU time, and fail to submit frames?
		 *
		 * We try to rule out any undue impact on the GPU scheduling
		 * from the CPU scheduler by looking for core saturation. If
		 * we may be in a situation where the clients + kernel are
		 * taking a whole core (think lockdep), then it is increasingly
		 * likely that our measurements include delays from the CPU
		 * scheduler. Err on the side of caution.
		 */
		d_time = igt_nsec_elapsed(&tv);
		getrusage(RUSAGE_CHILDREN, &usage);
		cpu_time = d_cpu_time(&usage, &old_usage);
		igt_debug("CPU usage: %.0f%%\n", 100. * cpu_time / d_time);
		if (4 * cpu_time > 3 * d_time) {
			if (nchild > 7) /* good enough to judge pass/fail */
				break;

			igt_skip_on_f(4 * cpu_time > 3 * d_time,
				      "%.0f%% CPU usage, presuming capacity exceeded\n",
				      100. * cpu_time / d_time);
		}

		/* With no contention, we should match our target frametime */
		if (nchild == 1) {
			igt_info("Interval %.2fms, range %.2fms\n",
				 1e-6 * result[0], 1e-6 * iqr[0]);
			igt_assert(4 * result[0] > 3 * fence_ns &&
				   3 * result[0] < 4 * fence_ns);
			continue;
		}

		/*
		 * The VIP should always be able to hit the target frame rate;
		 * regardless of budget contention from lessor clients.
		 */
		if (flags & (F_VIP | F_RRUL)) {
			const char *who = flags & F_VIP ? "VIP" : "RRUL";
			igt_info("%s interval %.2fms, range %.2fms\n",
				 who, 1e-6 * result[0], 1e-6 * iqr[0]);
			if (flags & F_VIP) {
				igt_assert_f(4 * result[0] > 3 * fence_ns &&
					     3 * result[0] < 4 * fence_ns,
					     "%s expects to run exactly when it wants, expects an interval of %.2fms, was %.2fms\n",
					     who,
					     1e-6 * fence_ns,
					     1e-6 * result[0]);
			}
			igt_assert_f(iqr[0] < result[0],
				     "%s frame IQR %.2fms exceeded median threshold %.2fms\n",
				     who, 1e-6 * iqr[0], 1e-6 * result[0] / 2);
			if (!--nchild)
				continue;

			/* Exclude the VIP result from the plebian statistics */
			memmove(result, result + 1, nchild * sizeof(*result));
			memmove(iqr, iqr + 1, nchild * sizeof(*iqr));
		}

		igt_mean_init(&m);
		for (int child = 0; child < nchild; child++)
			igt_mean_add(&m, result[child]);

		qsort(result, nchild, sizeof(*result), cmp_ul);
		qsort(iqr, nchild, sizeof(*iqr), cmp_ul);

		/*
		 * The target interval for median/mean is 16ms (fence_ns).
		 * However, this work is evenly split across the clients so
		 * the range (and median) of client medians may be much less
		 * than 16ms [16/3N]. We present median of medians to try
		 * and avoid any instability while running in CI; at the cost
		 * of insensitivity!
		 */
		igt_info("%3d clients, range: [%.1f, %.1f], iqr: [%.1f, %.1f], median: %.1f [%.1f, %.1f], mean: %.1f ± %.2f ms, cpu: %.0f%%\n",
			 nchild,
			 1e-6 * result[0],  1e-6 * result[nchild - 1],
			 1e-6 * result[lo], 1e-6 * result[hi],
			 1e-6 * result[nchild / 2],
			 1e-6 * iqr[lo], 1e-6 * iqr[hi],
			 1e-6 * igt_mean_get(&m),
			 1e-6 * sqrt(igt_mean_get_variance(&m)),
			 100. * cpu_time / d_time);

		igt_assert_f(iqr[nchild / 2] < result[nchild / 2],
			     "Child frame IQR %.2fms exceeded median threshold %.2fms\n",
			     1e-6 * iqr[nchild / 2],
			     1e-6 * result[nchild / 2]);

		igt_assert_f(4 * igt_mean_get(&m) > 3 * result[nchild / 2] &&
			     3 * igt_mean_get(&m) < 4 * result[nchild / 2],
			     "Mean of client interval %.2fms differs from median %.2fms, distribution is skewed\n",

			     1e-6 * igt_mean_get(&m), 1e-6 * result[nchild / 2]);

		igt_assert_f(result[nchild / 2] > frame_ns / 2,
			     "Median client interval %.2fms did not match target interval %.2fms\n",
			     1e-6 * result[nchild / 2], 1e-6 * frame_ns);


		igt_assert_f(result[hi] - result[lo] < result[nchild / 2],
			     "Interquartile range of client intervals %.2fms is as large as the median threshold %.2fms, clients are not evenly distributed!\n",
			     1e-6 * (result[hi] - result[lo]),
			     1e-6 * result[nchild / 2]);

		/* May be slowed due to sheer volume of context switches */
		if (result[0] > 2 * fence_ns)
			break;
	}

	munmap(iqr, 4096);
	munmap(result, 4096);
	if (common)
		gem_close(i915, common);

	close(lnk.child[0]);
	close(lnk.child[1]);
	close(lnk.parent[0]);
	close(lnk.parent[1]);
}

static void deadline_child(int i915,
			   uint32_t ctx,
			   const struct intel_execution_engine2 *e,
			   uint32_t handle,
			   int timeline,
			   int frame_ns,
			   int sv, int rv,
			   int *done,
			   unsigned int flags)
#define DL_PRIO (1 << 0)
{
	struct drm_i915_gem_exec_object2 obj[] = {
		{ handle }, delay_create(i915, ctx, e, frame_ns),
	};
	struct drm_i915_gem_exec_fence fence = {
		.flags = I915_EXEC_FENCE_SIGNAL,
	};
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(obj),
		.buffer_count = ARRAY_SIZE(obj),
		.flags = I915_EXEC_FENCE_OUT | e->flags,
		.rsvd1 = ctx,
	};
	unsigned int seq = 1;
	int prev = -1, next = -1;

	if (intel_gen(intel_get_drm_devid(i915)) < 8)
		execbuf.flags |= I915_EXEC_SECURE;

	gem_execbuf_wr(i915, &execbuf);
	execbuf.rsvd2 >>= 32;
	gem_execbuf_wr(i915, &execbuf);
	gem_sync(i915, obj[1].handle);

	execbuf.num_cliprects = 1;
	execbuf.cliprects_ptr = to_user_pointer(&fence);
	execbuf.flags |= I915_EXEC_FENCE_ARRAY;
	if (!(flags & DL_PRIO))
		execbuf.flags |= I915_EXEC_FENCE_IN;

	write(sv, &prev, sizeof(int));
	read(rv, &prev, sizeof(int));
	igt_assert(prev == -1);

	prev = execbuf.rsvd2;
	next = execbuf.rsvd2 >> 32;
	while (!READ_ONCE(*done)) {
		sync_fence_wait(prev, -1);
		igt_assert_eq(sync_fence_status(prev), 1);
		close(prev);

		fence.handle = syncobj_create(i915, 0);
		execbuf.rsvd2 = sw_sync_timeline_create_fence(timeline, seq);
		gem_execbuf_wr(i915, &execbuf);
		close(execbuf.rsvd2);

		write(sv, &fence.handle, sizeof(uint32_t));

		prev = next;
		next = execbuf.rsvd2 >> 32;
		seq++;
	}
	close(next);
	close(prev);
}

static struct intel_execution_engine2 pick_default(int i915)
{
	const struct intel_execution_engine2 *e;

	__for_each_physical_engine(i915, e) {
		if (!e->flags)
			return *e;
	}

	return (struct intel_execution_engine2){};
}

static struct intel_execution_engine2 pick_engine(int i915, const char *name)
{
	const struct intel_execution_engine2 *e;

	__for_each_physical_engine(i915, e) {
		if (!strcmp(e->name, name))
			return *e;
	}

	return (struct intel_execution_engine2){};
}

static bool has_syncobj(int i915)
{
	struct drm_get_cap cap = { .capability = DRM_CAP_SYNCOBJ };
	ioctl(i915, DRM_IOCTL_GET_CAP, &cap);
	return cap.value;
}

static bool has_fence_array(int i915)
{
	int value = 0;
	struct drm_i915_getparam gp = {
		.param = I915_PARAM_HAS_EXEC_FENCE_ARRAY,
		.value = &value,
	};

	ioctl(i915, DRM_IOCTL_I915_GETPARAM, &gp);
	errno = 0;

	return value;
}

static uint64_t time_get_mono_ns(void)
{
	struct timespec tv;

	igt_assert(clock_gettime(CLOCK_MONOTONIC, &tv) == 0);
	return tv.tv_sec * NSEC64 + tv.tv_nsec;
}

static void deadline(int i915, int duration, unsigned int flags)
{
	const int64_t frame_ns = 33670 * 1000; /* 29.7fps */
	const int64_t parent_ns = 400 * 1000;
	const int64_t switch_ns = 50 * 1000;
	const int64_t overhead_ns = /* estimate timeslicing overhead */
		(frame_ns / 1000 / 1000 + 2) * switch_ns + parent_ns;
	struct intel_execution_engine2 pe = pick_default(i915);
	struct intel_execution_engine2 ve = pick_engine(i915, "vcs0");
	struct drm_i915_gem_exec_fence *fences = calloc(sizeof(*fences), 32);
	struct drm_i915_gem_exec_object2 *obj = calloc(sizeof(*obj), 32);
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(obj),
		.cliprects_ptr = to_user_pointer(fences),
		.flags =
			I915_EXEC_BATCH_FIRST |
			I915_EXEC_FENCE_ARRAY |
			I915_EXEC_FENCE_OUT
	};
	int *ctl;

	igt_require(has_syncobj(i915));
	igt_require(has_fence_array(i915));
	igt_require(has_mi_math(i915, &pe));
	igt_require(has_ctx_timestamp(i915, &pe));
	igt_require(has_mi_math(i915, &ve));
	igt_require(has_ctx_timestamp(i915, &ve));
	igt_assert(obj && fences);
	if (flags & DL_PRIO)
		igt_require(gem_scheduler_has_preemption(i915));

	ctl = mmap(NULL, 4096, PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
	igt_assert(ctl != MAP_FAILED);

	obj[0] = delay_create(i915, 0, &pe, parent_ns);
	if (flags & DL_PRIO)
		gem_context_set_priority(i915, 0, 1023);
	if (intel_gen(intel_get_drm_devid(i915)) < 8)
		execbuf.flags |= I915_EXEC_SECURE;
	for (int n = 1; n <= 5; n++) {
		int timeline = sw_sync_timeline_create();
		int nframes = duration * NSEC64 / frame_ns + 1;
		int num_children = (1 << n) - 1;
		int child_ns = (frame_ns - overhead_ns) / num_children - switch_ns;
		struct { int child[2], parent[2]; } *link;
		uint64_t start, over;
		int missed;

		if (child_ns < 0)
			break;

		execbuf.buffer_count = num_children + 1;
		execbuf.num_cliprects = num_children;

		link = malloc(sizeof(*link) * num_children);
		for (int i = 0; i < num_children; i++) {
			obj[i + 1].handle = gem_create(i915, 4096);
			pipe(link[i].child);
			pipe(link[i].parent);
		}

		*ctl = 0;
		igt_fork(child, num_children) {
			uint32_t ctx = gem_context_clone_with_engines(i915, 0);

			deadline_child(i915, ctx, &ve, obj[child + 1].handle,
				       timeline, child_ns,
				       link[child].child[1],
				       link[child].parent[0],
				       ctl, flags);

			gem_context_destroy(i915, ctx);
		}

		for (int i = 0; i < num_children; i++)
			read(link[i].child[0], &over, sizeof(int));
		igt_info("Testing %d children, with %'dns\n", num_children, child_ns);
		for (int i = 0; i < num_children; i++)
			write(link[i].parent[1], &over, sizeof(int));

		over = 0;
		missed = 0;
		start = time_get_mono_ns();
		for (int frame = 1; frame <= nframes; frame++) {
			struct rusage old_usage, usage;
			uint64_t cpu_time, d_time;
			struct timespec tv;
			uint64_t time;
			int fence;

			getrusage(RUSAGE_CHILDREN, &old_usage);
			igt_nsec_elapsed(memset(&tv, 0, sizeof(tv)));

			sw_sync_timeline_inc(timeline, 1);
			for (int i = 0; i < num_children; i++) {
				read(link[i].child[0], &fences[i].handle, sizeof(uint32_t));
				fences[i].flags = I915_EXEC_FENCE_WAIT;
			}

			gem_execbuf_wr(i915, &execbuf);
			for (int i = 0; i < num_children; i++)
				syncobj_destroy(i915, fences[i].handle);

			fence = execbuf.rsvd2 >> 32;
			sync_fence_wait(fence, -1);
			igt_assert_eq(sync_fence_status(fence), 1);
			time = sync_fence_timestamp(fence) - start;
			close(fence);

			d_time = igt_nsec_elapsed(&tv);
			getrusage(RUSAGE_CHILDREN, &usage);
			cpu_time = d_cpu_time(&usage, &old_usage);
			igt_debug("CPU usage: %.0f%%\n",
				  100. * cpu_time / d_time);
			if (4 * cpu_time > 3 * d_time)
				break;

			if (time > frame * frame_ns) {
				igt_warn("Frame %d: over by %'"PRIu64"ns\n",
					 frame, time - frame * frame_ns);
				over += time - frame * frame_ns;
				missed++;
			}
		}
		*ctl = 1;
		sw_sync_timeline_inc(timeline, 3);
		igt_waitchildren();
		close(timeline);

		igt_assert_f(missed == 0,
			     "%d child, missed %d frames, overran by %'"PRIu64"us\n",
			     num_children, missed, over / 1000);

		for (int i = 0; i < num_children; i++) {
			gem_close(i915, obj[i + 1].handle);
			close(link[i].child[0]);
			close(link[i].child[1]);
			close(link[i].parent[0]);
			close(link[i].parent[1]);
		}
		free(link);

		gem_quiescent_gpu(i915);
	}

	gem_close(i915, obj[0].handle);
	free(obj);
	free(fences);
}

static bool set_heartbeat(int i915, const char *name, unsigned int value)
{
	unsigned int x;

	if (gem_engine_property_printf(i915, name,
				       "heartbeat_interval_ms",
				       "%d", value) < 0)
		return false;

	x = ~value;
	gem_engine_property_scanf(i915, name,
				  "heartbeat_interval_ms",
				  "%d", &x);
	igt_assert_eq(x, value);

	return true;
}

igt_main
{
	static const struct {
		const char *name;
		unsigned int flags;
		unsigned int basic;
#define BASIC		(1 << 0)
#define BASIC_ALL	(1 << 1)
	} fair[] = {
		/*
		 * none - maximal greed in each client
		 *
		 * Push as many frames from each client as fast as possible
		 */
		{ "none",       0, BASIC_ALL },
		{ "none-vip",   F_VIP, BASIC }, /* one vip client must meet deadlines */
		{ "none-solo",  F_SOLO, BASIC }, /* 1 batch per frame per client */
		{ "none-share", F_SHARE, BASIC }, /* read from a common buffer */
		{ "none-rrul",  F_RRUL, BASIC }, /* "realtime-response under load" */
		{ "none-ping",  F_PING }, /* measure inter-engine fairness */

		/*
		 * throttle - original per client throttling
		 *
		 * Used for front buffering rendering where there is no
		 * extenal frame marker. Each client tries to only keep
		 * 20ms of work submitted, though that measurement is
		 * flawed...
		 *
		 * This is used by Xorg to try and maintain some resembalance
		 * of input/output consistency when being feed a continuous
		 * stream of X11 draw requests straight into scanout, where
		 * the clients may submit the work faster than can be drawn.
		 *
		 * Throttling tracks requests per-file (and assumes that
		 * all requests are in submission order across the whole file),
		 * so we split each child to its own fd.
		 */
		{ "throttle",       F_THROTTLE | F_ISOLATE, BASIC },
		{ "throttle-vip",   F_THROTTLE | F_ISOLATE | F_VIP },
		{ "throttle-solo",  F_THROTTLE | F_ISOLATE | F_SOLO },
		{ "throttle-share", F_THROTTLE | F_ISOLATE | F_SHARE },
		{ "throttle-rrul",  F_THROTTLE | F_ISOLATE | F_RRUL },

		/*
		 * pace - mesa "submit double buffering"
		 *
		 * Submit a frame, wait for previous frame to start. This
		 * prevents each client from getting too far ahead of its
		 * rendering, maintaining a consistent input/output latency.
		 */
		{ "pace",       F_PACE, BASIC_ALL },
		{ "pace-solo",  F_PACE | F_SOLO, BASIC },
		{ "pace-share", F_PACE | F_SOLO | F_SHARE, BASIC },
		{ "pace-ping",  F_PACE | F_SOLO | F_SHARE | F_PING},

		/* sync - only submit a frame at a time */
		{ "sync",      F_SYNC, BASIC },
		{ "sync-vip",  F_SYNC | F_VIP },
		{ "sync-solo", F_SYNC | F_SOLO },

		/* flow - synchronise execution against the clock (vblank) */
		{ "flow",       F_PACE | F_FLOW, BASIC },
		{ "flow-solo",  F_PACE | F_FLOW | F_SOLO },
		{ "flow-share", F_PACE | F_FLOW | F_SHARE },
		{ "flow-ping",  F_PACE | F_FLOW | F_SHARE | F_PING },

		/* next - submit ahead of the clock (vblank double buffering) */
		{ "next",       F_PACE | F_FLOW | F_NEXT },
		{ "next-solo",  F_PACE | F_FLOW | F_NEXT | F_SOLO },
		{ "next-share", F_PACE | F_FLOW | F_NEXT | F_SHARE },
		{ "next-ping",  F_PACE | F_FLOW | F_NEXT | F_SHARE | F_PING },

		/* spare - underutilise by a single client timeslice */
		{ "spare",      F_PACE | F_FLOW | F_SPARE },
		{ "spare-solo", F_PACE | F_FLOW | F_SPARE | F_SOLO },

		/* half - run at half pace (submit 16ms of work every 32ms) */
		{ "half",       F_PACE | F_FLOW | F_HALF },
		{ "half-solo",  F_PACE | F_FLOW | F_HALF | F_SOLO },

		{}
	};
	const struct intel_execution_engine2 *e;
	int i915 = -1;

	igt_fixture {
		igt_require_sw_sync();

		i915 = drm_open_driver_master(DRIVER_INTEL);
		gem_submission_print_method(i915);
		gem_scheduler_print_capability(i915);

		igt_require_gem(i915);
		gem_require_mmap_wc(i915);
		gem_require_contexts(i915);
		igt_require(gem_scheduler_enabled(i915));
		igt_require(gem_scheduler_has_ctx_priority(i915));

		igt_info("CS timestamp frequency: %d\n",
			 read_timestamp_frequency(i915));
		igt_require(has_mi_math(i915, NULL));

		igt_fork_hang_detector(i915);
	}

	/* First we do a trimmed set of basic tests for faster CI */
	for (typeof(*fair) *f = fair; f->name; f++) {
		if (!f->basic)
			continue;

		igt_subtest_with_dynamic_f("basic-%s", f->name)  {
			__for_each_physical_engine(i915, e) {
				if (!has_mi_math(i915, e))
					continue;

				if (!gem_class_can_store_dword(i915, e->class))
					continue;

				if (e->flags && !(f->basic & BASIC_ALL))
					continue;

				igt_dynamic_f("%s", e->name)
					fairness(i915, e, 1, f->flags);
			}
		}
	}

	igt_subtest("basic-deadline")
		deadline(i915, 2, 0);
	igt_subtest("deadline-prio")
		deadline(i915, 2, DL_PRIO);

	for (typeof(*fair) *f = fair; f->name; f++) {
		igt_subtest_with_dynamic_f("fair-%s", f->name)  {
			__for_each_physical_engine(i915, e) {
				if (!has_mi_math(i915, e))
					continue;

				if (!gem_class_can_store_dword(i915, e->class))
					continue;

				if (!set_heartbeat(i915, e->name, 5000))
					continue;

				igt_dynamic_f("%s", e->name)
					fairness(i915, e, 5, f->flags);
			}
		}
	}

	igt_fixture {
		igt_stop_hang_detector();
		close(i915);
	}
}
