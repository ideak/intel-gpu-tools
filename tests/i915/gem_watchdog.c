/*
 * Copyright © 2021 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "config.h"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <unistd.h>
#include <sched.h>

#include "i915/gem.h"
#include "i915/gem_create.h"
#include "i915/gem_vm.h"
#include "igt.h"
#include "igt_params.h"
#include "sw_sync.h"

#define EWATCHDOG EINTR

static unsigned int default_timeout_wait_s;
static const unsigned int watchdog_us = 500 * 1000;

static unsigned int
wait_timeout(int i915, igt_spin_t **spin, unsigned int num_engines,
	     unsigned int wait_us, unsigned int expect)
{
	unsigned int count_idle = 0, count_fence = 0, count_started = 0, i;
	bool started[num_engines];

	memset(started, 0, sizeof(started));

	while (count_started < num_engines) {
		for (i = 0; i < num_engines; i++) {
			if (started[i])
				continue;

			if (igt_spin_has_started(spin[i])) {
				started[i] = true;
				count_started++;
			}
		}
	}

	igt_until_timeout(DIV_ROUND_UP(wait_us, USEC_PER_SEC)) {
		usleep(watchdog_us / 2);

		for (i = 0, count_idle = 0; i < num_engines; i++) {
			if (!gem_bo_busy(i915, spin[i]->handle))
				count_idle++;
		}

		for (i = 0, count_fence = 0; i < num_engines; i++) {
			if (sync_fence_status(spin[i]->out_fence))
				count_fence++;
		}

		if (count_idle == num_engines)
			break;
	}

	if (count_idle < expect) {
		for (i = 0; i < num_engines; i++) {
			if (gem_bo_busy(i915, spin[i]->handle))
				igt_warn("Request %u/%u not cancelled!\n",
					 i + 1, num_engines);
		}
	}

	if (count_fence < expect) {
		for (i = 0; i < num_engines; i++) {
			if (!sync_fence_status(spin[i]->out_fence))
				igt_warn("Fence %u/%u not timed out!\n",
					 i + 1, num_engines);
		}
	}

	igt_assert_eq(count_idle, count_fence);

	return count_fence;
}

static unsigned int spin_flags(void)
{
	return IGT_SPIN_POLL_RUN | IGT_SPIN_FENCE_OUT;
}

static void physical(int i915, const intel_ctx_t *ctx)
{
	const unsigned int wait_us = default_timeout_wait_s * USEC_PER_SEC;
	unsigned int num_engines, i, count;
	const struct intel_execution_engine2 *e;
	igt_spin_t *spin[GEM_MAX_ENGINES];

	i = 0;
	for_each_ctx_engine(i915, ctx, e) {
		spin[i] = igt_spin_new(i915, .ctx = ctx,
				       .engine = e->flags,
				       .flags = spin_flags());
		i++;
	}
	num_engines = i;

	count = wait_timeout(i915, spin, num_engines, wait_us, num_engines);

	for (i = 0; i < num_engines; i++)
		igt_spin_free(i915, spin[i]);

	igt_assert_eq(count, num_engines);
}

static struct i915_engine_class_instance *
list_engines(const intel_ctx_cfg_t *cfg,
	     unsigned int class, unsigned int *out)
{
	struct i915_engine_class_instance *ci;
	unsigned int count = 0, i;

	ci = malloc(cfg->num_engines * sizeof(*ci));
	igt_assert(ci);

	for (i = 0; i < cfg->num_engines; i++) {
		if (class == cfg->engines[i].engine_class)
			ci[count++] = cfg->engines[i];
	}

	if (!count) {
		free(ci);
		ci = NULL;
	}

	*out = count;
	return ci;
}

static size_t sizeof_load_balance(int count)
{
	return offsetof(struct i915_context_engines_load_balance,
			engines[count]);
}

static size_t sizeof_param_engines(int count)
{
	return offsetof(struct i915_context_param_engines,
			engines[count]);
}

#define alloca0(sz) ({ size_t sz__ = (sz); memset(alloca(sz__), 0, sz__); })

static int __set_load_balancer(int i915, uint32_t ctx,
			       const struct i915_engine_class_instance *ci,
			       unsigned int count,
			       void *ext)
{
	struct i915_context_engines_load_balance *balancer =
		alloca0(sizeof_load_balance(count));
	struct i915_context_param_engines *engines =
		alloca0(sizeof_param_engines(count + 1));
	struct drm_i915_gem_context_param p = {
		.ctx_id = ctx,
		.param = I915_CONTEXT_PARAM_ENGINES,
		.size = sizeof_param_engines(count + 1),
		.value = to_user_pointer(engines)
	};

	balancer->base.name = I915_CONTEXT_ENGINES_EXT_LOAD_BALANCE;
	balancer->base.next_extension = to_user_pointer(ext);

	igt_assert(count);
	balancer->num_siblings = count;
	memcpy(balancer->engines, ci, count * sizeof(*ci));

	engines->extensions = to_user_pointer(balancer);
	engines->engines[0].engine_class =
		I915_ENGINE_CLASS_INVALID;
	engines->engines[0].engine_instance =
		I915_ENGINE_CLASS_INVALID_NONE;
	memcpy(engines->engines + 1, ci, count * sizeof(*ci));

	return __gem_context_set_param(i915, &p);
}

static void set_load_balancer(int i915, uint32_t ctx,
			      const struct i915_engine_class_instance *ci,
			      unsigned int count,
			      void *ext)
{
	igt_assert_eq(__set_load_balancer(i915, ctx, ci, count, ext), 0);
}

static void virtual(int i915, const intel_ctx_cfg_t *base_cfg)
{
	const unsigned int wait_us = default_timeout_wait_s * USEC_PER_SEC;
	unsigned int num_engines = base_cfg->num_engines, i, count;
	igt_spin_t *spin[num_engines];
	unsigned int expect = num_engines;
	intel_ctx_cfg_t cfg = {};
	const intel_ctx_t *ctx[num_engines];

	igt_require(gem_has_execlists(i915));

	igt_debug("%u virtual engines\n", num_engines);
	igt_require(num_engines);

	cfg.vm = gem_vm_create(i915);

	i = 0;
	for (int class = 0; class < 32; class++) {
		struct i915_engine_class_instance *ci;

		ci = list_engines(base_cfg, class, &count);
		if (!ci)
			continue;

		for (int pass = 0; pass < count; pass++) {
			igt_assert(sizeof(*ci) == sizeof(int));
			igt_permute_array(ci, count, igt_exchange_int);

			igt_assert(i < num_engines);

			ctx[i] = intel_ctx_create(i915, &cfg);

			set_load_balancer(i915, ctx[i]->id, ci, count, NULL);

			spin[i] = igt_spin_new(i915,
					       .ctx = ctx[i],
					       .flags = spin_flags());
			i++;
		}

		free(ci);
	}

	count = wait_timeout(i915, spin, num_engines, wait_us, expect);

	for (i = 0; i < num_engines && spin[i]; i++) {
		igt_spin_free(i915, spin[i]);
		intel_ctx_destroy(i915, ctx[i]);
	}

	igt_assert_eq(count, expect);
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

static unsigned int offset_in_page(void *addr)
{
	return (uintptr_t)addr & 4095;
}

static uint64_t div64_u64_round_up(uint64_t x, uint64_t y)
{
	return (x + y - 1) / y;
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

static uint64_t ns_to_ticks(int i915, uint64_t ns)
{
	return div64_u64_round_up(ns * read_timestamp_frequency(i915),
				  NSEC_PER_SEC);
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

static void delay(int i915,
		  const struct intel_execution_engine2 *e,
		  uint32_t handle,
		  uint64_t addr,
		  uint64_t ns)
{
	const int use_64b = intel_gen(intel_get_drm_devid(i915)) >= 8;
	const uint32_t base = gem_engine_mmio_base(i915, e->name);
#define CS_GPR(x) (base + 0x600 + 8 * (x))
#define RUNTIME (base + 0x3a8)
	enum { START_TS, NOW_TS };
	uint32_t *map, *cs, *jmp;

	igt_require(base);

	/* Loop until CTX_TIMESTAMP - initial > @ns */

	cs = map = gem_mmap__device_coherent(i915, handle, 0, 4096, PROT_WRITE);

	*cs++ = MI_LOAD_REGISTER_IMM;
	*cs++ = CS_GPR(START_TS) + 4;
	*cs++ = 0;
	*cs++ = MI_LOAD_REGISTER_REG;
	*cs++ = RUNTIME;
	*cs++ = CS_GPR(START_TS);

	while (offset_in_page(cs) & 63)
		*cs++ = 0;
	jmp = cs;

	*cs++ = 0x5 << 23; /* MI_ARB_CHECK */

	*cs++ = MI_LOAD_REGISTER_IMM;
	*cs++ = CS_GPR(NOW_TS) + 4;
	*cs++ = 0;
	*cs++ = MI_LOAD_REGISTER_REG;
	*cs++ = RUNTIME;
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

	/* Break if delta > ns */
	*cs++ = MI_COND_BATCH_BUFFER_END | MI_DO_COMPARE | (1 + use_64b);
	*cs++ = ~ns_to_ticks(i915, ns);
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

static int __execbuf(int i915, struct drm_i915_gem_execbuffer2 *execbuf)
{
	int err;

	err = 0;
	if (ioctl(i915, DRM_IOCTL_I915_GEM_EXECBUFFER2, execbuf)) {
		err = -errno;
		igt_assume(err);
	}

	errno = 0;
	return err;
}

static uint32_t
far_delay(int i915, unsigned long delay, unsigned int target,
	  const intel_ctx_t *ctx,
	  const struct intel_execution_engine2 *e, int *fence)
{
	struct drm_i915_gem_exec_object2 obj = delay_create(i915, 0, e, delay);
	struct drm_i915_gem_exec_object2 batch[2] = {
		{
			.handle = batch_create(i915),
			.flags = EXEC_OBJECT_WRITE,
		}
	};
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(batch),
		.buffer_count = 2,
		.flags = e->flags,
	};
	intel_ctx_cfg_t cfg = ctx->cfg;
	uint32_t handle = gem_create(i915, 4096);
	unsigned long count, submit;

	igt_require(intel_gen(intel_get_drm_devid(i915)) >= 8);
	igt_require(gem_class_can_store_dword(i915, e->class));

	fcntl(i915, F_SETFL, fcntl(i915, F_GETFL) | O_NONBLOCK);

	submit = 3 * target;
	submit *= NSEC_PER_SEC;
	submit /= 2 * delay;

	if (gem_has_vm(i915))
		cfg.vm = gem_vm_create(i915);
	cfg.flags |= I915_CONTEXT_CREATE_FLAGS_SINGLE_TIMELINE;

	/*
	 * Submit a few long chains of individually short pieces of work
	 * against a shared object.
	 */
	for (count = 0; count < submit;) {
		const intel_ctx_t *tmp_ctx = intel_ctx_create(i915, &cfg);
		igt_assert(tmp_ctx->id);
		execbuf.rsvd1 = tmp_ctx->id;

		batch[1] = obj;
		while (__execbuf(i915, &execbuf) == 0)
			count++;
		intel_ctx_destroy(i915, tmp_ctx);
	}

	execbuf.flags |= I915_EXEC_FENCE_OUT;
	execbuf.rsvd1 = ctx->id;
	batch[1] = batch[0];
	batch[1].flags &= ~EXEC_OBJECT_WRITE;
	batch[0].handle = handle;
	assert(batch[0].flags & EXEC_OBJECT_WRITE);
	gem_execbuf_wr(i915, &execbuf);

	gem_close(i915, obj.handle);

	/* And pass the resulting end fence out. */
	*fence = execbuf.rsvd2 >> 32;

	return handle;
}

static void
far_fence(int i915, int timeout, const intel_ctx_t *ctx,
	  const struct intel_execution_engine2 *e)
{
	int fence = -1;
	uint32_t handle =
		far_delay(i915, NSEC_PER_SEC / 250, timeout, ctx, e, &fence);

	gem_close(i915, handle);

	igt_assert_eq(sync_fence_wait(fence, -1), 0);

	/*
	 * Many short pieces of work simulating independent clients working and
	 * presenting work to a consumer should not be interrupted by the
	 * watchdog.
	 *
	 * TODO/FIXME: Opens:
	 *
	 * 1)
	 *    Missing fence error propagation means consumer may fail to notice
	 *    the work hasn't actually been executed.
	 *
	 *    There is also no clear agreement on whether error propagation is
	 *    desired or not.
	 *
	 * 2)
	 *    This assert could instead check that fence status is in error, if
	 *    it will be accepted this kind of workload should suddenly start
	 *    failing. Depends if the desire is to test watchdog could break
	 *    existing userspace or whether it is acceptable to silently not
	 *    execute workloads.
	 *
	 * 3)
	 *    Implement subtest which actually renders to a shared buffer so
	 *    watchdog effect on rendering result can also be demonstrated.
	 */
	igt_assert_eq(sync_fence_status(fence), 1);

	close(fence);
}

igt_main
{
	const struct intel_execution_engine2 *e;
	const intel_ctx_t *ctx;
	int i915 = -1;

	igt_fixture {
		const unsigned int timeout = 1;
		char *tmp;

		i915 = drm_open_driver_master(DRIVER_INTEL);
		gem_submission_print_method(i915);
		gem_scheduler_print_capability(i915);

		igt_require_gem(i915);

		tmp = __igt_params_get(i915, "request_timeout_ms");
		igt_skip_on_f(!tmp || !atoi(tmp),
			      "Request expiry not supported!\n");
		free(tmp);

		igt_params_save_and_set(i915, "request_timeout_ms", "%u",
					timeout * 1000);
		default_timeout_wait_s = timeout * 5;

		i915 = gem_reopen_driver(i915); /* Apply modparam. */
		ctx = intel_ctx_create_all_physical(i915);
	}

	igt_subtest_group {
		igt_subtest("default-physical")
			physical(i915, ctx);

		igt_subtest("default-virtual")
			virtual(i915, &ctx->cfg);
	}

	igt_subtest_with_dynamic("far-fence") {
		for_each_ctx_engine(i915, ctx, e) {
			igt_dynamic_f("%s", e->name)
				far_fence(i915, default_timeout_wait_s * 3,
					  ctx, e);
		}
	}

	igt_fixture {
		intel_ctx_destroy(i915, ctx);
		close(i915);
	}
}
