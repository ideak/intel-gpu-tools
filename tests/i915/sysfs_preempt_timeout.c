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
#include <sched.h>
#include <sys/types.h>
#include <unistd.h>

#include "drmtest.h" /* gem_quiescent_gpu()! */
#include "i915/gem_engine_topology.h"
#include "igt_dummyload.h"
#include "igt_sysfs.h"
#include "ioctl_wrappers.h" /* igt_require_gem()! */
#include "sw_sync.h"

#include "igt_debugfs.h"

#define ATTR "preempt_timeout_ms"
#define RESET_TIMEOUT 50 /* milliseconds, at least one jiffie for kworker */

static bool __enable_hangcheck(int dir, bool state)
{
	return igt_sysfs_set(dir, "enable_hangcheck", state ? "1" : "0");
}

static bool enable_hangcheck(int i915, bool state)
{
	bool success;
	int dir;

	dir = igt_sysfs_open_parameters(i915);
	if (dir < 0) /* no parameters, must be default! */
		return false;

	success = __enable_hangcheck(dir, state);
	close(dir);

	return success;
}

static void set_preempt_timeout(int engine, unsigned int value)
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
	unsigned int delays[] = { 0, 1, 1000, 1234, 654321 };
	unsigned int saved;

	/* Quick test that store/show reports the same values */

	igt_assert(igt_sysfs_scanf(engine, ATTR, "%u", &saved) == 1);
	igt_debug("Initial %s:%u\n", ATTR, saved);

	for (int i = 0; i < ARRAY_SIZE(delays); i++)
		set_preempt_timeout(engine, delays[i]);

	set_preempt_timeout(engine, saved);
}

static void test_invalid(int i915, int engine)
{
	unsigned int saved, delay;

	/* Quick test that values that are not representable are rejected */

	igt_assert(igt_sysfs_scanf(engine, ATTR, "%u", &saved) == 1);
	igt_debug("Initial %s:%u\n", ATTR, saved);

	igt_sysfs_printf(engine, ATTR, PRIu64, -1);
	igt_sysfs_scanf(engine, ATTR, "%u", &delay);
	igt_assert_eq(delay, saved);

	igt_sysfs_printf(engine, ATTR, "%d", -1);
	igt_sysfs_scanf(engine, ATTR, "%u", &delay);
	igt_assert_eq(delay, saved);

	igt_sysfs_printf(engine, ATTR, PRIu64, 40ull << 32);
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

static uint32_t create_context(int i915, unsigned int class, unsigned int inst, int prio)
{
	uint32_t ctx;

	ctx = gem_context_create_for_engine(i915, class, inst);
	set_unbannable(i915, ctx);
	gem_context_set_priority(i915, ctx, prio);

	return ctx;
}

static uint64_t __test_timeout(int i915, int engine, unsigned int timeout)
{
	unsigned int class, inst;
	struct timespec ts = {};
	igt_spin_t *spin[2];
	uint64_t elapsed;
	uint32_t ctx[2];

	igt_assert(igt_sysfs_scanf(engine, "class", "%u", &class) == 1);
	igt_assert(igt_sysfs_scanf(engine, "instance", "%u", &inst) == 1);

	set_preempt_timeout(engine, timeout);

	ctx[0] = create_context(i915, class, inst, -1023);
	spin[0] = igt_spin_new(i915, ctx[0],
			       .flags = (IGT_SPIN_NO_PREEMPTION |
					 IGT_SPIN_POLL_RUN |
					 IGT_SPIN_FENCE_OUT));
	igt_spin_busywait_until_started(spin[0]);

	ctx[1] = create_context(i915, class, inst, 1023);
	igt_nsec_elapsed(&ts);
	spin[1] = igt_spin_new(i915, ctx[1], .flags = IGT_SPIN_POLL_RUN);
	igt_spin_busywait_until_started(spin[1]);
	elapsed = igt_nsec_elapsed(&ts);

	igt_spin_free(i915, spin[1]);

	igt_assert_eq(wait_for_reset(spin[0]->out_fence), 0);
	igt_assert_eq(sync_fence_status(spin[0]->out_fence), -EIO);

	igt_spin_free(i915, spin[0]);

	gem_context_destroy(i915, ctx[1]);
	gem_context_destroy(i915, ctx[0]);
	gem_quiescent_gpu(i915);

	return elapsed;
}

static void test_timeout(int i915, int engine)
{
	int delays[] = { 1, 50, 100, 500 };
	unsigned int saved;

	/*
	 * Send down some non-preemptable workloads and then request a
	 * switch to a higher priority context. The HW will not be able to
	 * respond, so the kernel will be forced to reset the hog. This
	 * timeout should match our specification, and so we can measure
	 * the delay from requesting the preemption to its completion.
	 */

	igt_assert(igt_sysfs_scanf(engine, ATTR, "%u", &saved) == 1);
	igt_debug("Initial %s:%u\n", ATTR, saved);

	gem_quiescent_gpu(i915);
	igt_require(enable_hangcheck(i915, false));

	for (int i = 0; i < ARRAY_SIZE(delays); i++) {
		uint64_t elapsed;

		elapsed = __test_timeout(i915, engine, delays[i]);
		igt_info("%s:%d, elapsed=%.3fms\n",
			 ATTR, delays[i], elapsed * 1e-6);

		/*
		 * We need to give a couple of jiffies slack for the scheduler timeouts
		 * and then a little more slack fr the overhead in submitting and
		 * measuring. 50ms should cover all of our sins and be useful
		 * tolerance.
		 */
		igt_assert_f(elapsed / 1000 / 1000 < delays[i] + 50,
			     "Forced preemption timeout exceeded request!\n");
	}

	igt_assert(enable_hangcheck(i915, true));
	gem_quiescent_gpu(i915);
	set_preempt_timeout(engine, saved);
}

static void test_off(int i915, int engine)
{
	unsigned int class, inst;
	igt_spin_t *spin[2];
	unsigned int saved;
	uint32_t ctx[2];

	/*
	 * We support setting the timeout to 0 to disable the reset on
	 * preemption failure. Having established that we can do forced
	 * preemption on demand, we use the same setup (non-preeemptable hog
	 * followed by a high priority context) and verify that the hog is
	 * never reset. Never is a long time, so we settle for 150s.
	 */

	igt_assert(igt_sysfs_scanf(engine, ATTR, "%u", &saved) == 1);
	igt_debug("Initial %s:%u\n", ATTR, saved);

	gem_quiescent_gpu(i915);
	igt_require(enable_hangcheck(i915, false));

	igt_assert(igt_sysfs_scanf(engine, "class", "%u", &class) == 1);
	igt_assert(igt_sysfs_scanf(engine, "instance", "%u", &inst) == 1);

	set_preempt_timeout(engine, 0);

	ctx[0] = create_context(i915, class, inst, -1023);
	spin[0] = igt_spin_new(i915, ctx[0],
			       .flags = (IGT_SPIN_NO_PREEMPTION |
					 IGT_SPIN_POLL_RUN |
					 IGT_SPIN_FENCE_OUT));
	igt_spin_busywait_until_started(spin[0]);

	ctx[1] = create_context(i915, class, inst, 1023);
	spin[1] = igt_spin_new(i915, ctx[1], .flags = IGT_SPIN_POLL_RUN);

	for (int i = 0; i < 150; i++) {
		igt_assert_eq(sync_fence_status(spin[0]->out_fence), 0);
		sleep(1);
	}

	set_preempt_timeout(engine, 1);

	igt_spin_busywait_until_started(spin[1]);
	igt_spin_free(i915, spin[1]);

	igt_assert_eq(wait_for_reset(spin[0]->out_fence), 0);
	igt_assert_eq(sync_fence_status(spin[0]->out_fence), -EIO);

	igt_spin_free(i915, spin[0]);

	gem_context_destroy(i915, ctx[1]);
	gem_context_destroy(i915, ctx[0]);

	igt_assert(enable_hangcheck(i915, true));
	gem_quiescent_gpu(i915);

	set_preempt_timeout(engine, saved);
}

igt_main
{
	static const struct {
		const char *name;
		void (*fn)(int, int);
	} tests[] = {
		{ "idempotent", test_idempotent },
		{ "invalid", test_invalid },
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
