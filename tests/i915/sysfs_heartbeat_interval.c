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
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "drmtest.h"
#include "i915/gem.h"
#include "i915/gem_context.h"
#include "i915/gem_engine_topology.h"
#include "igt_debugfs.h"
#include "igt_dummyload.h"
#include "igt_sysfs.h"
#include "sw_sync.h"

#define ATTR "heartbeat_interval_ms"
#define RESET_TIMEOUT 50 /* milliseconds, at least one jiffie for kworker */

static bool __enable_hangcheck(int dir, bool state)
{
	return igt_sysfs_set(dir, "enable_hangcheck", state ? "1" : "0");
}

static void enable_hangcheck(int i915, bool state)
{
	int dir;

	dir = igt_sysfs_open_parameters(i915);
	if (dir < 0) /* no parameters, must be default! */
		return;

	__enable_hangcheck(dir, state);
	close(dir);
}

static void set_attr(int engine, const char *attr, unsigned int value)
{
	unsigned int saved = ~value;

	igt_debug("set %s:%d\n", attr, value);
	igt_require(igt_sysfs_printf(engine, attr, "%u", value) > 0);
	igt_sysfs_scanf(engine, attr, "%u", &saved);
	igt_assert_eq(saved, value);
}

static void set_heartbeat(int engine, unsigned int value)
{
	set_attr(engine, ATTR, value);
}

static void set_preempt_timeout(int engine, unsigned int value)
{
	set_attr(engine, "preempt_timeout_ms", value);
}

static int wait_for_reset(int fence)
{
	/* Do a double wait to paper over scheduler fluctuations */
	sync_fence_wait(fence, RESET_TIMEOUT);
	return sync_fence_wait(fence, RESET_TIMEOUT);
}

static void test_idempotent(int i915, int engine)
{
	unsigned int delays[] = { 1, 1000, 5000, 50000, 123456789 };
	unsigned int saved;

	/* Quick test that the property reports the values we set */

	igt_assert(igt_sysfs_scanf(engine, ATTR, "%u", &saved) == 1);
	igt_debug("Initial %s:%u\n", ATTR, saved);

	for (int i = 0; i < ARRAY_SIZE(delays); i++)
		set_heartbeat(engine, delays[i]);

	set_heartbeat(engine, saved);
}

static void test_invalid(int i915, int engine)
{
	unsigned int saved, delay;

	/* Quick test that we reject any unrepresentable intervals */

	igt_assert(igt_sysfs_scanf(engine, ATTR, "%u", &saved) == 1);
	igt_debug("Initial %s:%u\n", ATTR, saved);

	igt_sysfs_printf(engine, ATTR, PRIu64, -1);
	igt_sysfs_scanf(engine, ATTR, "%u", &delay);
	igt_assert_eq(delay, saved);

	igt_sysfs_printf(engine, ATTR, PRIu64, 10ull << 32);
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

	set_heartbeat(engine, timeout);

	ctx[0] = create_context(i915, class, inst, 1023);
	spin[0] = igt_spin_new(i915, ctx[0],
			       .flags = (IGT_SPIN_NO_PREEMPTION |
					 IGT_SPIN_POLL_RUN |
					 IGT_SPIN_FENCE_OUT));
	igt_spin_busywait_until_started(spin[0]);

	ctx[1] = create_context(i915, class, inst, -1023);
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

static void test_precise(int i915, int engine)
{
	int delays[] = { 1, 50, 100, 500 };
	unsigned int saved;

	/*
	 * The heartbeat interval defines how long the kernel waits between
	 * checking on the status of the engines. It first sends down a
	 * heartbeat pulse, waits the interval and sees if the system managed
	 * to complete the pulse. If not, it gives a priority bump to the pulse
	 * and waits again. This is repeated until the priority cannot be bumped
	 * any more, and the system is declared hung.
	 *
	 * If we combine the preemptive pulse with forced preemption, we instead
	 * get a much faster hang detection. Thus in combination we can measure
	 * the system response time to reseting a hog as a measure of the
	 * heartbeat interval, and so confirm it matches our specification.
	 */

	set_preempt_timeout(engine, 1);

	igt_assert(igt_sysfs_scanf(engine, ATTR, "%u", &saved) == 1);
	igt_debug("Initial %s:%u\n", ATTR, saved);
	gem_quiescent_gpu(i915);

	for (int i = 0; i < ARRAY_SIZE(delays); i++) {
		uint64_t elapsed;

		elapsed = __test_timeout(i915, engine, delays[i]);
		igt_info("%s:%d, elapsed=%.3fms[%d]\n", ATTR,
			 delays[i], elapsed * 1e-6,
			 (int)(elapsed / 1000 / 1000));

		/*
		 * It takes a couple of missed heartbeats before we start
		 * terminating hogs, and a little bit of jiffie slack for
		 * scheduling at each step. 150ms should cover all of our
		 * sins and be useful tolerance.
		 */
		igt_assert_f(elapsed / 1000 / 1000 < 3 * delays[i] + 150,
			     "Heartbeat interval (and CPR) exceeded request!\n");
	}

	gem_quiescent_gpu(i915);
	set_heartbeat(engine, saved);
}

static void test_nopreempt(int i915, int engine)
{
	int delays[] = { 1, 50, 100, 500 };
	unsigned int saved;

	/*
	 * The same principle as test_precise(), except that forced preemption
	 * is disabled (or simply not supported by the platform). This time,
	 * it waits until the system misses a few heartbeat before doing a
	 * per-engine/full-gpu reset. As such it is less precise, but we
	 * can still estimate an upper bound for our specified heartbeat
	 * interval and verify the system conforms.
	 */

	/* Test heartbeats with forced preemption  disabled */
	set_preempt_timeout(engine, 0);

	igt_assert(igt_sysfs_scanf(engine, ATTR, "%u", &saved) == 1);
	igt_debug("Initial %s:%u\n", ATTR, saved);
	gem_quiescent_gpu(i915);

	for (int i = 0; i < ARRAY_SIZE(delays); i++) {
		uint64_t elapsed;

		elapsed = __test_timeout(i915, engine, delays[i]);
		igt_info("%s:%d, elapsed=%.3fms[%d]\n", ATTR,
			 delays[i], elapsed * 1e-6,
			 (int)(elapsed / 1000 / 1000));

		/*
		 * It takes a few missed heartbeats before we start
		 * terminating hogs, and a little bit of jiffie slack for
		 * scheduling at each step. 250ms should cover all of our
		 * sins and be useful tolerance.
		 */
		igt_assert_f(elapsed / 1000 / 1000 < 5 * delays[i] + 250,
			     "Heartbeat interval (and CPR) exceeded request!\n");
	}

	gem_quiescent_gpu(i915);
	set_heartbeat(engine, saved);
}

static unsigned int measured_usleep(unsigned int usec)
{
	struct timespec ts = { };
	unsigned int slept;

	slept = igt_nsec_elapsed(&ts);
	igt_assert(slept == 0);
	do {
		usleep(usec - slept);
		slept = igt_nsec_elapsed(&ts) / 1000;
	} while (slept < usec);

	return igt_nsec_elapsed(&ts);
}

static void client(int i915, int engine, int *ctl, int duration, int expect)
{
	unsigned int class, inst;
	unsigned long count = 0;
	uint32_t ctx;

	igt_assert(igt_sysfs_scanf(engine, "class", "%u", &class) == 1);
	igt_assert(igt_sysfs_scanf(engine, "instance", "%u", &inst) == 1);

	ctx = create_context(i915, class, inst, 0);

	while (!READ_ONCE(*ctl)) {
		unsigned int elapsed;
		igt_spin_t *spin;

		spin = igt_spin_new(i915, ctx,
				    .flags = (IGT_SPIN_NO_PREEMPTION |
					      IGT_SPIN_POLL_RUN |
					      IGT_SPIN_FENCE_OUT));

		igt_spin_busywait_until_started(spin);
		igt_assert_eq(sync_fence_status(spin->out_fence), 0);

		elapsed = measured_usleep(duration * 1000);
		igt_spin_end(spin);

		sync_fence_wait(spin->out_fence, -1);
		if (sync_fence_status(spin->out_fence) != expect)
			kill(getppid(), SIGALRM); /* cancel parent's sleep */

		igt_assert_f(sync_fence_status(spin->out_fence) == expect,
			     "%s client: elapsed: %.3fms, expected %d, got %d\n",
			     expect < 0 ? "Bad" : "Good", elapsed * 1e-6,
			     expect, sync_fence_status(spin->out_fence));
		igt_spin_free(i915, spin);
		count++;
	}

	gem_context_destroy(i915, ctx);
	igt_info("%s client completed %lu spins\n",
		 expect < 0 ? "Bad" : "Good", count);
}

static void sighandler(int sig)
{
}

static void __test_mixed(int i915, int engine,
			 int heartbeat,
			 int good,
			 int bad,
			 int duration)
{
	unsigned int saved;
	sighandler_t old;
	int *shared;

	/*
	 * Given two clients of which one is a hog, be sure we cleanly
	 * terminate the hog leaving the good client to run.
	 */

	igt_assert(igt_sysfs_scanf(engine, ATTR, "%u", &saved) == 1);
	igt_debug("Initial %s:%u\n", ATTR, saved);
	gem_quiescent_gpu(i915);

	shared = mmap(NULL, 4096, PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
	igt_assert(shared != MAP_FAILED);

	set_heartbeat(engine, heartbeat);

	igt_fork(child, 1) /* good client */
		client(i915, engine, shared, good, 1);
	igt_fork(child, 1) /* bad client */
		client(i915, engine, shared, bad, -EIO);

	old = signal(SIGALRM, sighandler);
	sleep(duration);
	signal(SIGALRM, old);

	*shared = true;
	igt_waitchildren();
	munmap(shared, 4096);

	gem_quiescent_gpu(i915);
	set_heartbeat(engine, saved);
}

static void test_mixed(int i915, int engine)
{
	/*
	 * Hogs rarely run alone. Our hang detection must carefully wean
	 * out the hogs from the innocent clients. Thus we run a mixed workload
	 * with non-preemptable hogs that exceed the heartbeat, and quicker
	 * innocents. We inspect the fence status of each to verify that
	 * only the hogs are reset.
	 */
	set_preempt_timeout(engine, 25);
	__test_mixed(i915, engine, 25, 10, 250, 5);
}

static void test_long(int i915, int engine)
{
	/*
	 * Some clients relish being hogs, and demand that the system
	 * never do hangchecking. Never is hard to test, so instead we
	 * run over a day and verify that only the super hogs are reset.
	 */
	set_preempt_timeout(engine, 0);
	__test_mixed(i915, engine,
		     60 * 1000, /* 60s */
		     60 * 1000, /* 60s */
		     300 * 1000, /* 5min */
		     24 * 3600 /* 24hours */);
}

static void test_off(int i915, int engine)
{
	unsigned int class, inst;
	unsigned int saved;
	igt_spin_t *spin;
	uint32_t ctx;

	/*
	 * Some other clients request that there is never any interruption
	 * or jitter in their workload and so demand that the kernel never
	 * sends a heartbeat to steal precious cycles from their workload.
	 * Turn off the heartbeat and check that the workload is uninterrupted
	 * for 150s.
	 */

	igt_assert(igt_sysfs_scanf(engine, ATTR, "%u", &saved) == 1);
	igt_debug("Initial %s:%u\n", ATTR, saved);
	gem_quiescent_gpu(i915);

	igt_assert(igt_sysfs_scanf(engine, "class", "%u", &class) == 1);
	igt_assert(igt_sysfs_scanf(engine, "instance", "%u", &inst) == 1);

	set_heartbeat(engine, 0);

	ctx = create_context(i915, class, inst, 0);

	spin = igt_spin_new(i915, ctx,
			    .flags = (IGT_SPIN_POLL_RUN |
				      IGT_SPIN_NO_PREEMPTION |
				      IGT_SPIN_FENCE_OUT));
	igt_spin_busywait_until_started(spin);

	for (int i = 0; i < 150; i++) {
		igt_assert_eq(sync_fence_status(spin->out_fence), 0);
		sleep(1);
	}

	set_heartbeat(engine, 1);

	igt_assert_eq(sync_fence_wait(spin->out_fence, 250), 0);
	igt_assert_eq(sync_fence_status(spin->out_fence), -EIO);

	igt_spin_free(i915, spin);

	gem_quiescent_gpu(i915);
	set_heartbeat(engine, saved);
}

igt_main
{
	static const struct {
		const char *name;
		void (*fn)(int, int);
	} tests[] = {
		{ "idempotent", test_idempotent },
		{ "invalid", test_invalid },
		{ "precise", test_precise },
		{ "nopreempt", test_nopreempt },
		{ "mixed", test_mixed },
		{ "off", test_off },
		{ "long", test_long },
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

		enable_hangcheck(i915, true);
	}

	for (typeof(*tests) *t = tests; t->name; t++)
		igt_subtest_with_dynamic(t->name)
			dyn_sysfs_engines(i915, engines, ATTR, t->fn);

	igt_fixture {
		close(engines);
		close(i915);
	}
}
