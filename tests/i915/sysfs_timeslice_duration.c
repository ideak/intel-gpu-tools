/*
 * Copyright © 2019 Intel Corporation
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

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "igt_params.h"
#include "drmtest.h"
#include "i915/gem.h"
#include "i915/gem_create.h"
#include "i915/gem_engine_topology.h"
#include "i915/gem_mman.h"
#include "igt_dummyload.h"
#include "igt_sysfs.h"
#include "ioctl_wrappers.h"
#include "intel_chipset.h"
#include "intel_reg.h"
#include "sw_sync.h"

#define ATTR "timeslice_duration_ms"
#define RESET_TIMEOUT 50 /* milliseconds, at least one jiffie for kworker */

#define MI_SEMAPHORE_WAIT		(0x1c << 23)
#define   MI_SEMAPHORE_POLL             (1 << 15)
#define   MI_SEMAPHORE_SAD_GT_SDD       (0 << 12)
#define   MI_SEMAPHORE_SAD_GTE_SDD      (1 << 12)
#define   MI_SEMAPHORE_SAD_LT_SDD       (2 << 12)
#define   MI_SEMAPHORE_SAD_LTE_SDD      (3 << 12)
#define   MI_SEMAPHORE_SAD_EQ_SDD       (4 << 12)
#define   MI_SEMAPHORE_SAD_NEQ_SDD      (5 << 12)

static bool __enable_hangcheck(int dir, bool state)
{
	return igt_sysfs_set(dir, "enable_hangcheck", state ? "1" : "0");
}

static bool enable_hangcheck(int i915, bool state)
{
	bool success;
	int dir;

	dir = igt_params_open(i915);
	if (dir < 0) /* no parameters, must be default! */
		return false;

	success = __enable_hangcheck(dir, state);
	close(dir);

	return success;
}

static void set_timeslice(int engine, unsigned int value)
{
	unsigned int delay;

	igt_sysfs_printf(engine, ATTR, "%u", value);
	igt_sysfs_scanf(engine, ATTR, "%u", &delay);
	igt_assert_eq(delay, value);
}

static int wait_for_reset(int fence)
{
	/* Do a double wait to paper over scheduler fluctuations */
	sync_fence_wait(fence, RESET_TIMEOUT);
	return sync_fence_wait(fence, RESET_TIMEOUT);
}

static void test_idempotent(int i915, int engine)
{
	const unsigned int delays[] = { 0, 1, 1234, 654321 };
	unsigned int saved;

	/* Quick test to verify the kernel reports the same values as we write */

	igt_assert(igt_sysfs_scanf(engine, ATTR, "%u", &saved) == 1);
	igt_debug("Initial %s:%u\n", ATTR, saved);

	for (int i = 0; i < ARRAY_SIZE(delays); i++)
		set_timeslice(engine, delays[i]);

	set_timeslice(engine, saved);
}

static void test_invalid(int i915, int engine)
{
	unsigned int saved, delay;

	/* Quick test that non-representable delays are rejected */

	igt_assert(igt_sysfs_scanf(engine, ATTR, "%u", &saved) == 1);
	igt_debug("Initial %s:%u\n", ATTR, saved);

	igt_sysfs_printf(engine, ATTR, "%llu", -1ull);
	igt_sysfs_scanf(engine, ATTR, "%u", &delay);
	igt_assert_eq(delay, saved);

	igt_sysfs_printf(engine, ATTR, "%d", -1);
	igt_sysfs_scanf(engine, ATTR, "%u", &delay);
	igt_assert_eq(delay, saved);

	igt_sysfs_printf(engine, ATTR, "%llu", 123ull << 32);
	igt_sysfs_scanf(engine, ATTR, "%u", &delay);
	igt_assert_eq(delay, saved);
}

static void set_unbannable(int i915, uint32_t ctx)
{
	struct drm_i915_gem_context_param p = {
		.ctx_id = ctx,
		.param = I915_CONTEXT_PARAM_BANNABLE,
	};

	gem_context_set_param(i915, &p);
}

static const intel_ctx_t *
create_ctx(int i915, unsigned int class, unsigned int inst, int prio)
{
	const intel_ctx_t *ctx = intel_ctx_create_for_engine(i915, class, inst);
	set_unbannable(i915, ctx->id);
	gem_context_set_priority(i915, ctx->id, prio);

	return ctx;
}

static int cmp_u32(const void *_a, const void *_b)
{
	const uint32_t *a = _a, *b = _b;

	return *a - *b;
}

static double clockrate(int i915)
{
	int freq;
	drm_i915_getparam_t gp = {
		.value = &freq,
		.param = I915_PARAM_CS_TIMESTAMP_FREQUENCY,
	};

	igt_require(igt_ioctl(i915, DRM_IOCTL_I915_GETPARAM, &gp) == 0);
	return 1e9 / freq;
}

static uint64_t __test_duration(int i915, int engine, unsigned int timeout)
{
	struct drm_i915_gem_exec_object2 obj[3] = {
		{
			.handle = gem_create(i915, 4096),
			.offset = 0,
			.flags = EXEC_OBJECT_PINNED,
		},
		{
			.handle = gem_create(i915, 4096),
			.offset = 4096,
			.flags = EXEC_OBJECT_PINNED,
		},
		{ gem_create(i915, 4096) }
	};
	struct drm_i915_gem_execbuffer2 eb = {
		.buffer_count = ARRAY_SIZE(obj),
		.buffers_ptr = to_user_pointer(obj),
	};
	const unsigned int gen = intel_gen(intel_get_drm_devid(i915));
	double duration = clockrate(i915);
	unsigned int class, inst, mmio;
	uint32_t *cs, *map;
	const intel_ctx_t *ctx[2];
	int start;
	int i;

	igt_require(gem_scheduler_has_preemption(i915));
	igt_require(gen >= 8); /* MI_SEMAPHORE_WAIT */

	igt_assert(igt_sysfs_scanf(engine, "class", "%u", &class) == 1);
	igt_assert(igt_sysfs_scanf(engine, "instance", "%u", &inst) == 1);
	igt_require(igt_sysfs_scanf(engine, "mmio_base", "%x", &mmio) == 1);

	set_timeslice(engine, timeout);

	ctx[0] = create_ctx(i915, class, inst, 0);
	ctx[1] = create_ctx(i915, class, inst, 0);

	map = gem_mmap__device_coherent(i915, obj[2].handle,
					0, 4096, PROT_WRITE);
	gem_set_domain(i915, obj[2].handle,
		       I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);

	cs = map;
	for (i = 0; i < 10; i++) {
		*cs++ = MI_SEMAPHORE_WAIT |
			MI_SEMAPHORE_POLL |
			MI_SEMAPHORE_SAD_NEQ_SDD |
			(4 - 2 + (gen >= 12));
		*cs++ = 0;
		*cs++ = obj[0].offset + sizeof(uint32_t) * i;
		*cs++ = 0;
		if (gen >= 12)
			*cs++ = 0;

		*cs++ = 0x24 << 23 | 2; /* SRM */
		*cs++ = mmio + 0x358;
		*cs++ = obj[1].offset + sizeof(uint32_t) * i;
		*cs++ = 0;

		*cs++ = MI_STORE_DWORD_IMM;
		*cs++ = obj[0].offset +
			4096 - sizeof(uint32_t) * i - sizeof(uint32_t);
		*cs++ = 0;
		*cs++ = 1;
	}
	*cs++ = MI_BATCH_BUFFER_END;

	cs += 16 - ((cs - map) & 15);
	start = (cs - map) * sizeof(*cs);
	for (i = 0; i < 10; i++) {
		*cs++ = MI_STORE_DWORD_IMM;
		*cs++ = obj[0].offset + sizeof(uint32_t) * i;
		*cs++ = 0;
		*cs++ = 1;

		*cs++ = MI_SEMAPHORE_WAIT |
			MI_SEMAPHORE_POLL |
			MI_SEMAPHORE_SAD_NEQ_SDD |
			(4 - 2 + (gen >= 12));
		*cs++ = 0;
		*cs++ = obj[0].offset +
			4096 - sizeof(uint32_t) * i - sizeof(uint32_t);
		*cs++ = 0;
		if (gen >= 12)
			*cs++ = 0;
	}
	*cs++ = MI_BATCH_BUFFER_END;
	igt_assert(cs - map < 4096 / sizeof(*cs));
	munmap(map, 4096);

	eb.rsvd1 = ctx[0]->id;
	gem_execbuf(i915, &eb);

	eb.rsvd1 = ctx[1]->id;
	eb.batch_start_offset = start;
	gem_execbuf(i915, &eb);

	gem_sync(i915, obj[2].handle);

	gem_set_domain(i915, obj[1].handle,
		       I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);
	map = gem_mmap__device_coherent(i915, obj[1].handle,
					0, 4096, PROT_WRITE);
	for (i = 0; i < 9; i++)
		map[i] = map[i + 1] - map[i];
	qsort(map, 9, sizeof(*map), cmp_u32);
	duration *= map[4] / 2; /* 2 sema-waits between timestamp updates */
	munmap(map, 4096);

	for (i = 0; i < ARRAY_SIZE(ctx); i++)
		intel_ctx_destroy(i915, ctx[i]);

	for (i = 0; i < ARRAY_SIZE(obj); i++)
		gem_close(i915, obj[i].handle);

	return duration;
}

static unsigned int set_heartbeat(int engine, unsigned int value)
{
	const char *attr= "heartbeat_interval_ms";
	unsigned int old = ~value, new;

	igt_debug("set %s:%d\n", attr, value);
	igt_sysfs_scanf(engine, attr, "%u", &old);
	igt_require(igt_sysfs_printf(engine, attr, "%u", value) > 0);
	igt_sysfs_scanf(engine, attr, "%u", &new);
	igt_assert_eq(new, value);

	return old;
}

static void disable_heartbeat(int engine, unsigned int *saved)
{
	*saved = set_heartbeat(engine, 0);
}

static void enable_heartbeat(int engine, unsigned int saved)
{
	set_heartbeat(engine, saved);
}

static void test_duration(int i915, int engine)
{
	int delays[] = { 1, 50, 100, 500 };
	unsigned int saved, heartbeat;
	uint64_t elapsed;
	int epsilon;

	/*
	 * Timeslicing at its very basic level is sharing the GPU by
	 * running one context for interval before running another. After
	 * each interval the running context is swapped for another runnable
	 * context.
	 *
	 * We can measure this directly by watching the xCS_TIMESTAMP and
	 * recording its value every time we switch into the context, using
	 * a couple of semaphores to busyspin for the timeslice.
	 */

	igt_assert(igt_sysfs_scanf(engine, ATTR, "%u", &saved) == 1);
	igt_debug("Initial %s:%u\n", ATTR, saved);

	gem_quiescent_gpu(i915);

	disable_heartbeat(engine, &heartbeat);

	elapsed = __test_duration(i915, engine, 1);
	epsilon = 2 * elapsed / 1000 / 1000;
	if (epsilon < 50)
		epsilon = 50;
	igt_info("Minimum duration measured as %.3fms; setting error threshold to %dms\n",
		 elapsed * 1e-6, epsilon);
	igt_require(epsilon < 1000);

	for (int i = 0; i < ARRAY_SIZE(delays); i++) {
		elapsed = __test_duration(i915, engine, delays[i]);
		igt_info("%s:%d, elapsed=%.3fms\n",
			 ATTR, delays[i], elapsed * 1e-6);

		/*
		 * We need to give a couple of jiffies slack for the scheduler
		 * timeouts and then a little more slack fr the overhead in
		 * submitting and measuring. 50ms should cover all of our sins
		 * and be useful tolerance.
		 */
		igt_assert_f(elapsed / 1000 / 1000 < delays[i] + epsilon,
			     "Timeslice exceeded request!\n");
	}
	enable_heartbeat(engine, heartbeat);

	gem_quiescent_gpu(i915);
	set_timeslice(engine, saved);
}

static uint64_t __test_timeout(int i915, int engine, unsigned int timeout)
{
	unsigned int class, inst;
	struct timespec ts = {};
	igt_spin_t *spin[2];
	uint64_t elapsed;
	const intel_ctx_t *ctx[2];

	igt_assert(igt_sysfs_scanf(engine, "class", "%u", &class) == 1);
	igt_assert(igt_sysfs_scanf(engine, "instance", "%u", &inst) == 1);

	set_timeslice(engine, timeout);

	ctx[0] = create_ctx(i915, class, inst, 0);
	spin[0] = igt_spin_new(i915, .ctx = ctx[0],
			       .flags = (IGT_SPIN_NO_PREEMPTION |
					 IGT_SPIN_POLL_RUN |
					 IGT_SPIN_FENCE_OUT));
	igt_spin_busywait_until_started(spin[0]);

	ctx[1] = create_ctx(i915, class, inst, 0);
	igt_nsec_elapsed(&ts);
	spin[1] = igt_spin_new(i915, .ctx = ctx[1], .flags = IGT_SPIN_POLL_RUN);
	igt_spin_busywait_until_started(spin[1]);
	elapsed = igt_nsec_elapsed(&ts);

	igt_spin_free(i915, spin[1]);

	igt_assert_eq(wait_for_reset(spin[0]->out_fence), 0);
	igt_assert_eq(sync_fence_status(spin[0]->out_fence), -EIO);

	igt_spin_free(i915, spin[0]);

	intel_ctx_destroy(i915, ctx[1]);
	intel_ctx_destroy(i915, ctx[0]);
	gem_quiescent_gpu(i915);

	return elapsed;
}

static void test_timeout(int i915, int engine)
{
	int delays[] = { 1, 50, 100, 500 };
	unsigned int saved;
	uint64_t elapsed;
	int epsilon;

	/*
	 * Timeslicing requires us to preempt the running context in order to
	 * switch into its contemporary. If we couple a unpreemptable hog
	 * with a fast forced reset, we can measure the timeslice by how long
	 * it takes for the hog to be reset and the high priority context
	 * to complete.
	 */

	igt_require(igt_sysfs_printf(engine, "preempt_timeout_ms", "%u", 1) == 1);
	igt_assert(igt_sysfs_scanf(engine, ATTR, "%u", &saved) == 1);
	igt_debug("Initial %s:%u\n", ATTR, saved);

	gem_quiescent_gpu(i915);
	igt_require(enable_hangcheck(i915, false));

	elapsed = __test_timeout(i915, engine, 1);
	epsilon = 2 * elapsed / 1000 / 1000;
	if (epsilon < 50)
		epsilon = 50;
	igt_info("Minimum timeout measured as %.3fms; setting error threshold to %dms\n",
		 elapsed * 1e-6, epsilon);
	igt_require(epsilon < 1000);

	for (int i = 0; i < ARRAY_SIZE(delays); i++) {
		elapsed = __test_timeout(i915, engine, delays[i]);
		igt_info("%s:%d, elapsed=%.3fms\n",
			 ATTR, delays[i], elapsed * 1e-6);

		/*
		 * We need to give a couple of jiffies slack for the scheduler
		 * timeouts and then a little more slack fr the overhead in
		 * submitting and measuring. 50ms should cover all of our sins
		 * and be useful tolerance.
		 */
		igt_assert_f(elapsed / 1000 / 1000 < delays[i] + epsilon,
			     "Timeslice exceeded request!\n");
	}

	igt_assert(enable_hangcheck(i915, true));
	gem_quiescent_gpu(i915);
	set_timeslice(engine, saved);
}

static void test_off(int i915, int engine)
{
	unsigned int class, inst;
	unsigned int saved;
	igt_spin_t *spin[2];
	const intel_ctx_t *ctx[2];

	/*
	 * As always, there are some who must run uninterrupted and simply do
	 * not want to share the GPU even for a microsecond. Those greedy
	 * clients can disable timeslicing entirely, and so set the timeslice
	 * to 0. We test that a hog is not preempted within the 150s of
	 * our boredom threshold.
	 */

	igt_require(igt_sysfs_printf(engine, "preempt_timeout_ms", "%u", 1) == 1);
	igt_assert(igt_sysfs_scanf(engine, ATTR, "%u", &saved) == 1);
	igt_debug("Initial %s:%u\n", ATTR, saved);

	gem_quiescent_gpu(i915);
	igt_require(enable_hangcheck(i915, false));

	igt_assert(igt_sysfs_scanf(engine, "class", "%u", &class) == 1);
	igt_assert(igt_sysfs_scanf(engine, "instance", "%u", &inst) == 1);

	set_timeslice(engine, 0);

	ctx[0] = create_ctx(i915, class, inst, 0);
	spin[0] = igt_spin_new(i915, .ctx = ctx[0],
			       .flags = (IGT_SPIN_NO_PREEMPTION |
					 IGT_SPIN_POLL_RUN |
					 IGT_SPIN_FENCE_OUT));
	igt_spin_busywait_until_started(spin[0]);

	ctx[1] = create_ctx(i915, class, inst, 0);
	spin[1] = igt_spin_new(i915, .ctx = ctx[1], .flags = IGT_SPIN_POLL_RUN);

	for (int i = 0; i < 150; i++) {
		igt_assert_eq(sync_fence_status(spin[0]->out_fence), 0);
		sleep(1);
	}

	set_timeslice(engine, 1);

	igt_spin_busywait_until_started(spin[1]);
	igt_spin_free(i915, spin[1]);

	igt_assert_eq(wait_for_reset(spin[0]->out_fence), 0);
	igt_assert_eq(sync_fence_status(spin[0]->out_fence), -EIO);

	igt_spin_free(i915, spin[0]);

	intel_ctx_destroy(i915, ctx[1]);
	intel_ctx_destroy(i915, ctx[0]);

	igt_assert(enable_hangcheck(i915, true));
	gem_quiescent_gpu(i915);

	set_timeslice(engine, saved);
}

igt_main
{
	static const struct {
		const char *name;
		void (*fn)(int, int);
	} tests[] = {
		{ "idempotent", test_idempotent },
		{ "invalid", test_invalid },
		{ "duration", test_duration },
		{ "timeout", test_timeout },
		{ "off", test_off },
		{ }
	};
	int i915 = -1, engines = -1;

	igt_fixture {
		int sys;

		i915 = drm_open_driver(DRIVER_INTEL);
		igt_require_gem(i915);
		igt_allow_hang(i915, 0, 0);

		sys = igt_sysfs_open(i915);
		igt_require(sys != -1);

		engines = openat(sys, "engine", O_RDONLY);
		igt_require(engines != -1);

		close(sys);
	}

	for (typeof(*tests) *t = tests; t->name; t++)
		igt_subtest_with_dynamic(t->name)
			dyn_sysfs_engines(i915, engines, ATTR, t->fn);

	igt_fixture {
		close(engines);
		close(i915);
	}
}
