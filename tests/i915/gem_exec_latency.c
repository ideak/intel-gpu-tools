/*
 * Copyright © 2016 Intel Corporation
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
 *
 */

#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <inttypes.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/signal.h>
#include <time.h>
#include <sched.h>

#include "drm.h"

#include "igt.h"
#include "igt_device.h"
#include "igt_sysfs.h"
#include "igt_vgem.h"
#include "igt_dummyload.h"
#include "igt_stats.h"

#include "i915/gem.h"

#define ENGINE_FLAGS  (I915_EXEC_RING_MASK | I915_EXEC_BSD_MASK)

#define LIVE 0x1
#define CORK 0x2
#define PREEMPT 0x4

static unsigned int ring_size;
static double rcs_clock;
static struct intel_mmio_data mmio_data;

static void poll_ring(int fd, const struct intel_execution_engine2 *e)
{
	const struct igt_spin_factory opts = {
		.engine = e->flags,
		.flags = IGT_SPIN_POLL_RUN | IGT_SPIN_FAST,
	};
	struct timespec tv = {};
	unsigned long cycles;
	igt_spin_t *spin[2];
	uint64_t elapsed;

	spin[0] = __igt_spin_factory(fd, &opts);
	igt_assert(igt_spin_has_poll(spin[0]));

	spin[1] = __igt_spin_factory(fd, &opts);
	igt_assert(igt_spin_has_poll(spin[1]));

	igt_spin_end(spin[0]);
	igt_spin_busywait_until_started(spin[1]);

	igt_assert(!gem_bo_busy(fd, spin[0]->handle));

	cycles = 0;
	while ((elapsed = igt_nsec_elapsed(&tv)) < 2ull << 30) {
		const unsigned int idx = cycles++ & 1;

		igt_spin_reset(spin[idx]);

		gem_execbuf(fd, &spin[idx]->execbuf);

		igt_spin_end(spin[!idx]);
		igt_spin_busywait_until_started(spin[idx]);
	}

	igt_info("%s completed %ld cycles: %.3f us\n",
		 e->name, cycles, elapsed*1e-3/cycles);

	igt_spin_free(fd, spin[1]);
	igt_spin_free(fd, spin[0]);
}

#define TIMESTAMP (0x358)
static void latency_on_ring(int fd,
			    const struct intel_execution_engine2 *e,
			    unsigned flags)
{
	const unsigned int gen = intel_gen(intel_get_drm_devid(fd));
	const int has_64bit_reloc = gen >= 8;
	struct drm_i915_gem_exec_object2 obj[3];
	struct drm_i915_gem_relocation_entry reloc;
	struct drm_i915_gem_execbuffer2 execbuf;
	const uint32_t mmio_base = gem_engine_mmio_base(fd, e->name);
	igt_spin_t *spin = NULL;
	IGT_CORK_HANDLE(c);
	volatile uint32_t *reg;
	unsigned repeats = ring_size;
	uint32_t start, end, *map, *results;
	uint64_t offset;
	double gpu_latency;
	int i, j;

	igt_require(mmio_base);
	reg = (volatile uint32_t *)((volatile char *)igt_global_mmio + mmio_base + TIMESTAMP);

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(&obj[1]);
	execbuf.buffer_count = 2;
	execbuf.flags = e->flags;
	execbuf.flags |= I915_EXEC_NO_RELOC | I915_EXEC_HANDLE_LUT;

	memset(obj, 0, sizeof(obj));
	obj[1].handle = gem_create(fd, 4096);
	obj[1].flags = EXEC_OBJECT_WRITE;
	results = gem_mmap__wc(fd, obj[1].handle, 0, 4096, PROT_READ);

	obj[2].handle = gem_create(fd, 64*1024);
	map = gem_mmap__wc(fd, obj[2].handle, 0, 64*1024, PROT_WRITE);
	gem_set_domain(fd, obj[2].handle,
		       I915_GEM_DOMAIN_GTT,
		       I915_GEM_DOMAIN_GTT);
	map[0] = MI_BATCH_BUFFER_END;
	gem_execbuf(fd, &execbuf);

	memset(&reloc,0, sizeof(reloc));
	obj[2].relocation_count = 1;
	obj[2].relocs_ptr = to_user_pointer(&reloc);

	gem_set_domain(fd, obj[2].handle,
		       I915_GEM_DOMAIN_GTT,
		       I915_GEM_DOMAIN_GTT);

	reloc.target_handle = flags & CORK ? 1 : 0;
	reloc.read_domains = I915_GEM_DOMAIN_INSTRUCTION;
	reloc.write_domain = I915_GEM_DOMAIN_INSTRUCTION;
	reloc.presumed_offset = obj[1].offset;

	for (j = 0; j < repeats; j++) {
		execbuf.batch_start_offset = 64 * j;
		reloc.offset =
			execbuf.batch_start_offset + sizeof(uint32_t);
		reloc.delta = sizeof(uint32_t) * j;

		offset = reloc.presumed_offset;
		offset += reloc.delta;

		i = 16 * j;
		/* MI_STORE_REG_MEM */
		map[i++] = 0x24 << 23 | 1;
		if (has_64bit_reloc)
			map[i-1]++;
		map[i++] = mmio_base + TIMESTAMP;
		map[i++] = offset;
		if (has_64bit_reloc)
			map[i++] = offset >> 32;
		map[i++] = MI_BATCH_BUFFER_END;
	}

	if (flags & CORK) {
		obj[0].handle = igt_cork_plug(&c, fd);
		execbuf.buffers_ptr = to_user_pointer(&obj[0]);
		execbuf.buffer_count = 3;
	}

	if (flags & LIVE)
		spin = igt_spin_new(fd, .engine = e->flags);

	start = *reg;
	for (j = 0; j < repeats; j++) {
		uint64_t presumed_offset = reloc.presumed_offset;

		execbuf.batch_start_offset = 64 * j;
		reloc.offset =
			execbuf.batch_start_offset + sizeof(uint32_t);
		reloc.delta = sizeof(uint32_t) * j;

		gem_execbuf(fd, &execbuf);
		igt_assert(reloc.presumed_offset == presumed_offset);
	}
	end = *reg;
	igt_assert(reloc.presumed_offset == obj[1].offset);

	igt_spin_free(fd, spin);
	if (flags & CORK)
		igt_cork_unplug(&c);

	gem_set_domain(fd, obj[1].handle, I915_GEM_DOMAIN_GTT, 0);
	gpu_latency = (results[repeats-1] - results[0]) / (double)(repeats-1);

	gem_set_domain(fd, obj[2].handle,
		       I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);

	execbuf.batch_start_offset = 0;
	for (j = 0; j < repeats - 1; j++) {
		offset = obj[2].offset;
		offset += 64 * (j + 1);

		i = 16 * j + (has_64bit_reloc ? 4 : 3);
		map[i] = MI_BATCH_BUFFER_START;
		if (gen >= 8) {
			map[i] |= 1 << 8 | 1;
			map[i + 1] = offset;
			map[i + 2] = offset >> 32;
		} else if (gen >= 6) {
			map[i] |= 1 << 8;
			map[i + 1] = offset;
		} else {
			map[i] |= 2 << 6;
			map[i + 1] = offset;
			if (gen < 4)
				map[i] |= 1;
		}
	}
	offset = obj[2].offset;
	gem_execbuf(fd, &execbuf);
	igt_assert(offset == obj[2].offset);

	gem_set_domain(fd, obj[1].handle, I915_GEM_DOMAIN_GTT, 0);
	igt_info("%s: dispatch latency: %.1fns, execution latency: %.1fns (target %.1fns)\n",
		 e->name,
		 (end - start) / (double)repeats * rcs_clock,
		 gpu_latency * rcs_clock,
		 (results[repeats - 1] - results[0]) / (double)(repeats - 1) * rcs_clock);

	munmap(map, 64*1024);
	munmap(results, 4096);
	if (flags & CORK)
		gem_close(fd, obj[0].handle);
	gem_close(fd, obj[1].handle);
	gem_close(fd, obj[2].handle);
}

static void latency_from_ring(int fd,
			      const struct intel_execution_engine2 *e,
			      unsigned flags)
{
	const unsigned int gen = intel_gen(intel_get_drm_devid(fd));
	const int has_64bit_reloc = gen >= 8;
	struct drm_i915_gem_exec_object2 obj[3];
	struct drm_i915_gem_relocation_entry reloc;
	struct drm_i915_gem_execbuffer2 execbuf;
	const uint32_t mmio_base = gem_engine_mmio_base(fd, e->name);
	const unsigned int repeats = ring_size / 2;
	const struct intel_execution_engine2 *other;
	uint32_t *map, *results;
	uint32_t ctx[2] = {};
	int i, j;

	igt_require(mmio_base);

	if (flags & PREEMPT) {
		ctx[0] = gem_context_clone_with_engines(fd, 0);
		gem_context_set_priority(fd, ctx[0], -1023);

		ctx[1] = gem_context_clone_with_engines(fd, 0);
		gem_context_set_priority(fd, ctx[1], 1023);
	}

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(&obj[1]);
	execbuf.buffer_count = 2;
	execbuf.flags = e->flags;
	execbuf.flags |= I915_EXEC_NO_RELOC | I915_EXEC_HANDLE_LUT;
	execbuf.rsvd1 = ctx[1];

	memset(obj, 0, sizeof(obj));
	obj[1].handle = gem_create(fd, 4096);
	obj[1].flags = EXEC_OBJECT_WRITE;
	results = gem_mmap__wc(fd, obj[1].handle, 0, 4096, PROT_READ);

	obj[2].handle = gem_create(fd, 64*1024);
	map = gem_mmap__wc(fd, obj[2].handle, 0, 64*1024, PROT_WRITE);
	gem_set_domain(fd, obj[2].handle,
		       I915_GEM_DOMAIN_GTT,
		       I915_GEM_DOMAIN_GTT);
	map[0] = MI_BATCH_BUFFER_END;
	gem_execbuf(fd, &execbuf);

	memset(&reloc,0, sizeof(reloc));
	obj[2].relocation_count = 1;
	obj[2].relocs_ptr = to_user_pointer(&reloc);

	gem_set_domain(fd, obj[2].handle,
		       I915_GEM_DOMAIN_GTT,
		       I915_GEM_DOMAIN_GTT);

	reloc.read_domains = I915_GEM_DOMAIN_INSTRUCTION;
	reloc.write_domain = I915_GEM_DOMAIN_INSTRUCTION;
	reloc.presumed_offset = obj[1].offset;
	reloc.target_handle = flags & CORK ? 1 : 0;

	__for_each_physical_engine(fd, other) {
		igt_spin_t *spin = NULL;
		IGT_CORK_HANDLE(c);

		gem_set_domain(fd, obj[2].handle,
			       I915_GEM_DOMAIN_GTT,
			       I915_GEM_DOMAIN_GTT);

		if (flags & PREEMPT)
			spin = __igt_spin_new(fd,
					      .ctx = ctx[0],
					      .engine = e->flags);

		if (flags & CORK) {
			obj[0].handle = igt_cork_plug(&c, fd);
			execbuf.buffers_ptr = to_user_pointer(&obj[0]);
			execbuf.buffer_count = 3;
		}

		for (j = 0; j < repeats; j++) {
			uint64_t offset;

			execbuf.flags &= ~ENGINE_FLAGS;
			execbuf.flags |= e->flags;

			execbuf.batch_start_offset = 64 * j;
			reloc.offset =
				execbuf.batch_start_offset + sizeof(uint32_t);
			reloc.delta = sizeof(uint32_t) * j;

			reloc.presumed_offset = obj[1].offset;
			offset = reloc.presumed_offset;
			offset += reloc.delta;

			i = 16 * j;
			/* MI_STORE_REG_MEM */
			map[i++] = 0x24 << 23 | 1;
			if (has_64bit_reloc)
				map[i-1]++;
			map[i++] = mmio_base + TIMESTAMP;
			map[i++] = offset;
			if (has_64bit_reloc)
				map[i++] = offset >> 32;
			map[i++] = MI_BATCH_BUFFER_END;

			gem_execbuf(fd, &execbuf);

			execbuf.flags &= ~ENGINE_FLAGS;
			execbuf.flags |= other->flags;

			execbuf.batch_start_offset = 64 * (j + repeats);
			reloc.offset =
				execbuf.batch_start_offset + sizeof(uint32_t);
			reloc.delta = sizeof(uint32_t) * (j + repeats);

			reloc.presumed_offset = obj[1].offset;
			offset = reloc.presumed_offset;
			offset += reloc.delta;

			i = 16 * (j + repeats);
			/* MI_STORE_REG_MEM */
			map[i++] = 0x24 << 23 | 1;
			if (has_64bit_reloc)
				map[i-1]++;
			map[i++] = mmio_base + TIMESTAMP;
			map[i++] = offset;
			if (has_64bit_reloc)
				map[i++] = offset >> 32;
			map[i++] = MI_BATCH_BUFFER_END;

			gem_execbuf(fd, &execbuf);
		}

		if (flags & CORK)
			igt_cork_unplug(&c);
		gem_set_domain(fd, obj[1].handle,
			       I915_GEM_DOMAIN_GTT,
			       I915_GEM_DOMAIN_GTT);
		igt_spin_free(fd, spin);

		igt_info("%s-%s delay: %.2fns\n",
			 e->name, other->name,
			 (results[2*repeats-1] - results[0]) / (double)repeats * rcs_clock);
	}

	munmap(map, 64*1024);
	munmap(results, 4096);

	if (flags & CORK)
		gem_close(fd, obj[0].handle);
	gem_close(fd, obj[1].handle);
	gem_close(fd, obj[2].handle);

	if (flags & PREEMPT) {
		gem_context_destroy(fd, ctx[1]);
		gem_context_destroy(fd, ctx[0]);
	}
}

static void execution_latency(int i915, const struct intel_execution_engine2 *e)
{
	struct drm_i915_gem_exec_object2 obj = {
		.handle = gem_create(i915, 4095),
	};
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(&obj),
		.buffer_count = 1,
		.flags = e->flags | I915_EXEC_NO_RELOC | I915_EXEC_HANDLE_LUT,
	};
	const uint32_t mmio_base = gem_engine_mmio_base(i915, e->name);
	const unsigned int cs_timestamp = mmio_base + 0x358;
	volatile uint32_t *timestamp;
	uint32_t *cs, *result;

	igt_require(mmio_base);
	timestamp =
		(volatile uint32_t *)((volatile char *)igt_global_mmio + cs_timestamp);

	obj.handle = gem_create(i915, 4096);
	obj.flags = EXEC_OBJECT_PINNED;
	result = gem_mmap__wc(i915, obj.handle, 0, 4096, PROT_WRITE);

	for (int i = 0; i < 16; i++) {
		cs = result + 16 * i;
		*cs++ = 0x24 << 23 | 2; /* SRM */
		*cs++ = cs_timestamp;
		*cs++ = 4096 - 16 * 4 + i * 4;
		*cs++ = 0;
		*cs++ = 0xa << 23;
	}

	cs = result + 1024 - 16;

	for (int length = 2; length <= 16; length <<= 1) {
		struct igt_mean submit, batch, total;
		int last = length - 1;

		igt_mean_init(&submit);
		igt_mean_init(&batch);
		igt_mean_init(&total);

		igt_until_timeout(2) {
			uint32_t now, end;

			cs[last] = 0;

			now = *timestamp;
			for (int i = 0; i < length; i++) {
				execbuf.batch_start_offset = 64 * i;
				gem_execbuf(i915, &execbuf);
			}
			while (!((volatile uint32_t *)cs)[last])
				;
			end = *timestamp;

			igt_mean_add(&submit, (cs[0] - now) * rcs_clock);
			igt_mean_add(&batch, (cs[last] - cs[0]) * rcs_clock / last);
			igt_mean_add(&total, (end - now) * rcs_clock);
		}

		igt_info("%sx%d Submission latency: %.2f±%.2fus\n",
			 e->name, length,
			 1e-3 * igt_mean_get(&submit),
			 1e-3 * sqrt(igt_mean_get_variance(&submit)));

		igt_info("%sx%d Inter-batch latency: %.2f±%.2fus\n",
			 e->name, length,
			 1e-3 * igt_mean_get(&batch),
			 1e-3 * sqrt(igt_mean_get_variance(&batch)));

		igt_info("%sx%d End-to-end latency: %.2f±%.2fus\n",
			 e->name, length,
			 1e-3 * igt_mean_get(&total),
			 1e-3 * sqrt(igt_mean_get_variance(&total)));
	}

	munmap(result, 4096);
	gem_close(i915, obj.handle);
}

static void wakeup_latency(int i915, const struct intel_execution_engine2 *e)
{
	struct drm_i915_gem_exec_object2 obj = {
		.handle = gem_create(i915, 4095),
	};
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(&obj),
		.buffer_count = 1,
		.flags = e->flags | I915_EXEC_NO_RELOC | I915_EXEC_HANDLE_LUT,
	};
	const uint32_t mmio_base = gem_engine_mmio_base(i915, e->name);
	const unsigned int cs_timestamp = mmio_base + 0x358;
	volatile uint32_t *timestamp;
	struct igt_mean wakeup;
	uint32_t *cs, *result;

	igt_require(gem_class_has_mutable_submission(i915, e->class));

	timestamp =
		(volatile uint32_t *)((volatile char *)igt_global_mmio + cs_timestamp);

	obj.handle = gem_create(i915, 4096);
	obj.flags = EXEC_OBJECT_PINNED;
	result = gem_mmap__wc(i915, obj.handle, 0, 4096, PROT_WRITE);

	cs = result;

	*cs++ = 0x24 << 23 | 2; /* SRM */
	*cs++ = cs_timestamp;
	*cs++ = 4096 - 16 * 4;
	*cs++ = 0;

	*cs++ = MI_BATCH_BUFFER_START | 1;
	*cs++ = 0;
	*cs++ = 0;

	*cs++ = 0x24 << 23 | 2; /* SRM */
	*cs++ = cs_timestamp;
	*cs++ = 4096 - 16 * 4 + 4;
	*cs++ = 0;
	*cs++ = 0xa << 23;

	cs = result + 1024 - 16;

	{
		struct sched_param p = { .sched_priority = 99 };
		sched_setscheduler(0, SCHED_FIFO | SCHED_RESET_ON_FORK, &p);
	}

	igt_mean_init(&wakeup);
	igt_until_timeout(2) {
		uint32_t end;

		igt_fork(child, 1) {
			result[4] = MI_BATCH_BUFFER_START | 1;
			cs[0] = 0;

			gem_execbuf(i915, &execbuf);

			while (!READ_ONCE(cs[0]))
				;
			result[4] = 0;
			__sync_synchronize();
		}
		gem_sync(i915, obj.handle);
		end = *timestamp;

		igt_mean_add(&wakeup, (end - cs[1]) * rcs_clock);
		igt_waitchildren();
	}
	igt_info("%s Wakeup latency: %.2f±%.2fms [%.2f, %.2f]\n", e->name,
		 1e-6 * igt_mean_get(&wakeup),
		 1e-6 * sqrt(igt_mean_get_variance(&wakeup)),
		 1e-6 * wakeup.min, 1e-6 * wakeup.max);

	munmap(result, 4096);
	gem_close(i915, obj.handle);
}

static void
__submit_spin(int fd, igt_spin_t *spin, unsigned int flags)
{
	struct drm_i915_gem_execbuffer2 eb = spin->execbuf;

	eb.flags &= ~(0x3f | I915_EXEC_BSD_MASK);
	eb.flags |= flags | I915_EXEC_NO_RELOC;

	gem_execbuf(fd, &eb);
}

struct rt_pkt {
	struct igt_mean mean;
	double min, max;
};

static bool __spin_wait(int fd, igt_spin_t *spin)
{
	while (!igt_spin_has_started(spin)) {
		if (!gem_bo_busy(fd, spin->handle))
			return false;
	}

	return true;
}

/*
 * Test whether RT thread which hogs the CPU a lot can submit work with
 * reasonable latency.
 */
static void rthog_latency_on_ring(int fd, const struct intel_execution_engine2 *e)
{
	const char *passname[] = {
		"warmup",
		"normal",
		"rt[0]",
		"rt[1]",
		"rt[2]",
		"rt[3]",
		"rt[4]",
		"rt[5]",
		"rt[6]",
	};
#define NPASS ARRAY_SIZE(passname)
#define MMAP_SZ (64 << 10)
	const struct igt_spin_factory opts = {
		.engine = e->flags,
		.flags = IGT_SPIN_POLL_RUN | IGT_SPIN_FAST,
	};
	struct rt_pkt *results;
	int ret;

	igt_assert(NPASS * sizeof(*results) <= MMAP_SZ);
	results = mmap(NULL, MMAP_SZ, PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
	igt_assert(results != MAP_FAILED);

	igt_fork(child, 1) {
		unsigned int pass = 0; /* Three phases: warmup, normal, rt. */

		do {
			struct igt_mean mean;
			double min = HUGE_VAL;
			double max = -HUGE_VAL;
			igt_spin_t *spin;

			igt_mean_init(&mean);

			if (pass == 2) {
				struct sched_param rt =
				{ .sched_priority = 99 };

				ret = sched_setscheduler(0,
							 SCHED_FIFO | SCHED_RESET_ON_FORK,
							 &rt);
				if (ret) {
					igt_warn("Failed to set scheduling policy!\n");
					break;
				}
			}

			usleep(250);

			spin = __igt_spin_factory(fd, &opts);
			if (!spin) {
				igt_warn("Failed to create spinner! (%s)\n",
					 passname[pass]);
				break;
			}
			igt_spin_busywait_until_started(spin);

			igt_until_timeout(pass > 0 ? 5 : 2) {
				struct timespec ts = { };
				double t;

				igt_spin_end(spin);
				gem_sync(fd, spin->handle);

				igt_spin_reset(spin);

				igt_nsec_elapsed(&ts);
				__submit_spin(fd, spin, e->flags);
				if (!__spin_wait(fd, spin)) {
					igt_warn("Wait timeout! (%s)\n",
						 passname[pass]);
					break;
				}

				t = igt_nsec_elapsed(&ts) * 1e-9;
				if (t > max)
					max = t;
				if (t < min)
					min = t;

				igt_mean_add(&mean, t);
			}

			igt_spin_free(fd, spin);

			igt_info("%8s %10s: mean=%.2fus stddev=%.3fus [%.2fus, %.2fus] (n=%lu)\n",
				 e->name,
				 passname[pass],
				 igt_mean_get(&mean) * 1e6,
				 sqrt(igt_mean_get_variance(&mean)) * 1e6,
				 min * 1e6, max * 1e6,
				 mean.count);

			results[pass].mean = mean;
			results[pass].min = min;
			results[pass].max = max;
		} while (++pass < NPASS);
	}
	igt_waitchildren();

	{
		struct rt_pkt normal = results[1];
		igt_stats_t stats;
		double variance;

		igt_stats_init_with_size(&stats, NPASS);

		variance = 0;
		for (unsigned int pass = 2; pass < NPASS; pass++) {
			struct rt_pkt *rt = &results[pass];

			igt_assert(rt->max);

			igt_stats_push_float(&stats, igt_mean_get(&rt->mean));
			variance += igt_mean_get_variance(&rt->mean);
		}
		variance /= NPASS - 2;

		igt_info("%8s: normal latency=%.2f±%.3fus, rt latency=%.2f±%.3fus\n",
			 e->name,
			 igt_mean_get(&normal.mean) * 1e6,
			 sqrt(igt_mean_get_variance(&normal.mean)) * 1e6,
			 igt_stats_get_median(&stats) * 1e6,
			 sqrt(variance) * 1e6);

		igt_assert(igt_stats_get_median(&stats) <
			   igt_mean_get(&normal.mean) * 2);

		/* The system is noisy; be conservative when declaring fail. */
		igt_assert(variance < igt_mean_get_variance(&normal.mean) * 10);
	}

	munmap(results, MMAP_SZ);
}

static void context_switch(int i915,
			   const struct intel_execution_engine2 *e,
			   unsigned int flags)
{
	struct drm_i915_gem_exec_object2 obj[2];
	struct drm_i915_gem_relocation_entry reloc[5];
	struct drm_i915_gem_execbuffer2 eb;
	uint32_t *cs, *bbe, *results, v;
	const uint32_t mmio_base = gem_engine_mmio_base(i915, e->name);
	struct igt_mean mean;
	uint32_t ctx[2];

	igt_require(mmio_base);
	igt_require(gem_class_has_mutable_submission(i915, e->class));

	for (int i = 0; i < ARRAY_SIZE(ctx); i++)
		ctx[i] = gem_context_clone_with_engines(i915, 0);

	if (flags & PREEMPT) {
		gem_context_set_priority(i915, ctx[0], -1023);
		gem_context_set_priority(i915, ctx[1], +1023);
	}

	memset(obj, 0, sizeof(obj));
	obj[0].handle = gem_create(i915, 4096);
	gem_set_caching(i915, obj[0].handle, 1);
	results = gem_mmap__cpu(i915, obj[0].handle, 0, 4096, PROT_READ);
	gem_set_domain(i915, obj[0].handle, I915_GEM_DOMAIN_CPU, 0);

	obj[1].handle = gem_create(i915, 4096);
	memset(reloc,0, sizeof(reloc));
	obj[1].relocation_count = ARRAY_SIZE(reloc);
	obj[1].relocs_ptr = to_user_pointer(reloc);
	bbe = gem_mmap__wc(i915, obj[1].handle, 0, 4096, PROT_WRITE);
	gem_set_domain(i915, obj[1].handle,
		       I915_GEM_DOMAIN_WC, I915_GEM_DOMAIN_WC);

	cs = bbe;
	*cs++ = 0x5 << 23;
	*cs++ = 0x24 << 23 | 2; /* SRM */
	*cs++ = mmio_base + 0x358; /* TIMESTAMP */
	reloc[0].target_handle = obj[0].handle;
	reloc[0].offset = (cs - bbe) * sizeof(*cs);
	*cs++ = 0;
	*cs++ = 0;
	*cs++ = MI_BATCH_BUFFER_START | 1;
	reloc[1].target_handle = obj[1].handle;
	reloc[1].offset = (cs - bbe) * sizeof(*cs);
	*cs++ = 0;
	*cs++ = 0;

	cs = bbe + 64;
	*cs++ = 0x24 << 23 | 2; /* SRM */
	*cs++ = mmio_base + 0x358; /* TIMESTAMP */
	reloc[2].target_handle = obj[0].handle;
	reloc[2].offset = (cs - bbe) * sizeof(*cs);
	*cs++ = reloc[2].delta = 4;
	*cs++ = 0;
	*cs++ = 0x29 << 23 | 2; /* LRM */
	*cs++ = mmio_base + 0x600; /* GPR0 */
	reloc[3].target_handle = obj[0].handle;
	reloc[3].offset = (cs - bbe) * sizeof(*cs);
	*cs++ = 0;
	*cs++ = 0;
	*cs++ = 0x24 << 23 | 2; /* SRM */
	*cs++ = mmio_base + 0x600; /* GPR0 */
	reloc[4].target_handle = obj[0].handle;
	reloc[4].offset = (cs - bbe) * sizeof(*cs);
	*cs++ = reloc[4].delta = 8;
	*cs++ = 0;
	*cs++ = 0xa << 23;

	memset(&eb, 0, sizeof(eb));
	eb.buffers_ptr = to_user_pointer(obj);
	eb.buffer_count = ARRAY_SIZE(obj);
	eb.flags = e->flags | I915_EXEC_NO_RELOC;

	v = 0;
	igt_mean_init(&mean);
	igt_until_timeout(5) {
		eb.rsvd1 = ctx[0];
		eb.batch_start_offset = 0;
		gem_execbuf(i915, &eb);

		while (results[0] == v)
			igt_assert(gem_bo_busy(i915, obj[1].handle));

		eb.rsvd1 = ctx[1];
		eb.batch_start_offset = 64 * sizeof(*cs);
		gem_execbuf(i915, &eb);

		*bbe = 0xa << 23;
		gem_sync(i915, obj[1].handle);
		*bbe = 0x5 << 23;

		v = results[0];
		igt_mean_add(&mean, (results[1] - results[2]) * rcs_clock);
	}
	igt_info("%s context switch latency%s: %.2f±%.2fus\n",
		 e->name, flags & PREEMPT ? " (preempt)" : "",
		 1e-3 * igt_mean_get(&mean),
		 1e-3 * sqrt(igt_mean_get_variance(&mean)));
	munmap(results, 4096);
	munmap(bbe, 4096);

	for (int i = 0; i < ARRAY_SIZE(obj); i++)
		gem_close(i915, obj[i].handle);

	for (int i = 0; i < ARRAY_SIZE(ctx); i++)
		gem_context_destroy(i915, ctx[i]);
}

static double clockrate(int i915, int reg)
{
	volatile uint32_t *mmio;
	uint32_t r_start, r_end;
	struct timespec tv;
	uint64_t t_start, t_end;
	uint64_t elapsed;
	int cs_timestamp_freq;
	drm_i915_getparam_t gp = {
		.value = &cs_timestamp_freq,
		.param = I915_PARAM_CS_TIMESTAMP_FREQUENCY,
	};

	if (igt_ioctl(i915, DRM_IOCTL_I915_GETPARAM, &gp) == 0)
		return cs_timestamp_freq;

	mmio = (volatile uint32_t *)((volatile char *)igt_global_mmio + reg);

	t_start = igt_nsec_elapsed(&tv);
	r_start = *mmio;
	elapsed = igt_nsec_elapsed(&tv) - t_start;

	usleep(1000);

	t_end = igt_nsec_elapsed(&tv);
	r_end = *mmio;
	elapsed += igt_nsec_elapsed(&tv) - t_end;

	elapsed = (t_end - t_start) + elapsed / 2;
	return (r_end - r_start) * 1e9 / elapsed;
}

#define test_each_engine(T, i915, e) \
	igt_subtest_with_dynamic(T) __for_each_physical_engine(i915, e) \
		for_each_if(gem_class_can_store_dword(i915, (e)->class)) \
			igt_dynamic_f("%s", (e)->name)

igt_main
{
	const struct intel_execution_engine2 *e;
	int device = -1;

	igt_fixture {
		device = drm_open_driver(DRIVER_INTEL);
		igt_require_gem(device);
		gem_require_mmap_wc(device);

		gem_submission_print_method(device);

		ring_size = gem_submission_measure(device, ALL_ENGINES);
		igt_info("Ring size: %d batches\n", ring_size);
		igt_require(ring_size > 8);
		ring_size -= 8; /* leave some spare */
		if (ring_size > 1024)
			ring_size = 1024;

		intel_register_access_init(&mmio_data, igt_device_get_pci_device(device), false, device);
		rcs_clock = clockrate(device, 0x2000 + TIMESTAMP);
		igt_info("RCS timestamp clock: %.0fKHz, %.1fns\n",
			 rcs_clock / 1e3, 1e9 / rcs_clock);
		rcs_clock = 1e9 / rcs_clock;
	}

	igt_subtest_group {
		igt_fixture
			igt_require(intel_gen(intel_get_drm_devid(device)) >= 7);

		test_each_engine("rthog-submit", device, e)
			rthog_latency_on_ring(device, e);

		test_each_engine("dispatch", device, e)
			latency_on_ring(device, e, 0);
		test_each_engine("dispatch-queued", device, e)
			latency_on_ring(device, e, CORK);

		test_each_engine("live-dispatch", device, e)
			latency_on_ring(device, e, LIVE);
		test_each_engine("live-dispatch-queued", device, e)
			latency_on_ring(device, e, LIVE | CORK);

		test_each_engine("poll", device, e)
			poll_ring(device, e);

		test_each_engine("synchronisation", device, e)
			latency_from_ring(device, e, 0);
		test_each_engine("synchronisation-queued", device, e)
			latency_from_ring(device, e, CORK);

		test_each_engine("execution-latency", device, e)
			execution_latency(device, e);
		test_each_engine("wakeup-latency", device, e)
			wakeup_latency(device, e);

		igt_subtest_group {
			igt_fixture {
				gem_require_contexts(device);
				igt_require(gem_scheduler_has_preemption(device));
			}

			test_each_engine("preemption", device, e)
				latency_from_ring(device, e, PREEMPT);
			test_each_engine("context-switch", device, e)
				context_switch(device, e, 0);
			test_each_engine("context-preempt", device, e)
				context_switch(device, e, PREEMPT);
		}
	}

	igt_fixture {
		intel_register_access_fini(&mmio_data);
		close(device);
	}
}
