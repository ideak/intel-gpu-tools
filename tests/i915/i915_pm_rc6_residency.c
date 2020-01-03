/*
 * Copyright © 2012 Intel Corporation
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
 * Authors:
 *    Ben Widawsky <ben@bwidawsk.net>
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

#include "igt.h"
#include "igt_perf.h"
#include "igt_sysfs.h"

#define SLEEP_DURATION 3 /* in seconds */

#define RC6_ENABLED	1
#define RC6P_ENABLED	2
#define RC6PP_ENABLED	4

static int sysfs;

struct residencies {
	int rc6;
	int media_rc6;
	int rc6p;
	int rc6pp;
	int duration;
};

static unsigned long get_rc6_enabled_mask(void)
{
	unsigned long enabled;

	enabled = 0;
	igt_sysfs_scanf(sysfs, "power/rc6_enable", "%lu", &enabled);
	return enabled;
}

static bool has_rc6_residency(const char *name)
{
	unsigned long residency;
	char path[128];

	sprintf(path, "power/%s_residency_ms", name);
	return igt_sysfs_scanf(sysfs, path, "%lu", &residency) == 1;
}

static unsigned long read_rc6_residency(const char *name)
{
	unsigned long residency;
	char path[128];

	residency = 0;
	sprintf(path, "power/%s_residency_ms", name);
	igt_assert(igt_sysfs_scanf(sysfs, path, "%lu", &residency) == 1);
	return residency;
}

static void residency_accuracy(unsigned int diff,
			       unsigned int duration,
			       const char *name_of_rc6_residency)
{
	double ratio;

	ratio = (double)diff / duration;

	igt_info("Residency in %s or deeper state: %u ms (sleep duration %u ms) (%.1f%% of expected duration)\n",
		 name_of_rc6_residency, diff, duration, 100*ratio);
	igt_assert_f(ratio > 0.9 && ratio < 1.05,
		     "Sysfs RC6 residency counter is inaccurate.\n");
}

static unsigned long gettime_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);

	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void read_residencies(int devid, unsigned int mask,
			     struct residencies *res)
{
	res->duration = gettime_ms();

	if (mask & RC6_ENABLED)
		res->rc6 = read_rc6_residency("rc6");

	if ((mask & RC6_ENABLED) &&
	    (IS_VALLEYVIEW(devid) || IS_CHERRYVIEW(devid)))
		res->media_rc6 = read_rc6_residency("media_rc6");

	if (mask & RC6P_ENABLED)
		res->rc6p = read_rc6_residency("rc6p");

	if (mask & RC6PP_ENABLED)
		res->rc6pp = read_rc6_residency("rc6pp");

	res->duration += (gettime_ms() - res->duration) / 2;
}

static void measure_residencies(int devid, unsigned int mask,
				struct residencies *res)
{
	struct residencies start = { };
	struct residencies end = { };
	int retry;

	/*
	 * Retry in case of counter wrap-around. We simply re-run the
	 * measurement, since the valid counter range is different on
	 * different platforms and so fixing it up would be non-trivial.
	 */
	read_residencies(devid, mask, &end);
	igt_debug("time=%d: rc6=(%d, %d), rc6p=%d, rc6pp=%d\n",
		  end.duration, end.rc6, end.media_rc6, end.rc6p, end.rc6pp);
	for (retry = 0; retry < 2; retry++) {
		start = end;
		sleep(SLEEP_DURATION);
		read_residencies(devid, mask, &end);

		igt_debug("time=%d: rc6=(%d, %d), rc6p=%d, rc6pp=%d\n",
			  end.duration,
			  end.rc6, end.media_rc6, end.rc6p, end.rc6pp);

		if (end.rc6 >= start.rc6 &&
		    end.media_rc6 >= start.media_rc6 &&
		    end.rc6p >= start.rc6p &&
		    end.rc6pp >= start.rc6pp)
			break;
	}
	igt_assert_f(retry < 2, "residency values are not consistent\n");

	res->rc6 = end.rc6 - start.rc6;
	res->rc6p = end.rc6p - start.rc6p;
	res->rc6pp = end.rc6pp - start.rc6pp;
	res->media_rc6 = end.media_rc6 - start.media_rc6;
	res->duration = end.duration - start.duration;

	/*
	 * For the purposes of this test case we want a given residency value
	 * to include the time spent in the corresponding RC state _and_ also
	 * the time spent in any enabled deeper states. So for example if any
	 * of RC6P or RC6PP is enabled we want the time spent in these states
	 * to be also included in the RC6 residency value. The kernel reported
	 * residency values are exclusive, so add up things here.
	 */
	res->rc6p += res->rc6pp;
	res->rc6 += res->rc6p;
}

static bool wait_for_rc6(void)
{
	struct timespec tv = {};
	unsigned long start, now;

	/* First wait for roughly an RC6 Evaluation Interval */
	usleep(160 * 1000);

	/* Then poll for RC6 to start ticking */
	now = read_rc6_residency("rc6");
	do {
		start = now;
		usleep(5000);
		now = read_rc6_residency("rc6");
		if (now - start > 1)
			return true;
	} while (!igt_seconds_elapsed(&tv));

	return false;
}

static uint64_t __pmu_read_single(int fd, uint64_t *ts)
{
	uint64_t data[2];

	igt_assert_eq(read(fd, data, sizeof(data)), sizeof(data));

	if (ts)
		*ts = data[1];

	return data[0];
}

static uint64_t pmu_read_single(int fd)
{
	return __pmu_read_single(fd, NULL);
}

#define __assert_within_epsilon(x, ref, tol_up, tol_down) \
	igt_assert_f((x) <= (ref) * (1.0 + (tol_up)/100.) && \
		     (x) >= (ref) * (1.0 - (tol_down)/100.), \
		     "'%s' != '%s' (%.3g not within +%d%%/-%d%% tolerance of %.3g)\n",\
		     #x, #ref, (double)(x), (tol_up), (tol_down), (double)(ref))

#define assert_within_epsilon(x, ref, tolerance) \
	__assert_within_epsilon(x, ref, tolerance, tolerance)

static bool __pmu_wait_for_rc6(int fd)
{
	struct timespec tv = {};
	uint64_t start, now;

	/* First wait for roughly an RC6 Evaluation Interval */
	usleep(160 * 1000);

	/* Then poll for RC6 to start ticking */
	now = pmu_read_single(fd);
	do {
		start = now;
		usleep(5000);
		now = pmu_read_single(fd);
		if (now - start > 1e6)
			return true;
	} while (!igt_seconds_elapsed(&tv));

	return false;
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

static uint32_t batch_create(int fd)
{
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	uint32_t handle;

	handle = gem_create(fd, 4096);
	gem_write(fd, handle, 0, &bbe, sizeof(bbe));

	return handle;
}

static int open_pmu(int i915, uint64_t config)
{
	int fd;

	fd = perf_i915_open(i915, config);
	igt_skip_on(fd < 0 && errno == ENODEV);
	igt_assert(fd >= 0);

	return fd;
}

static void rc6_idle(int i915)
{
	const int64_t duration_ns = SLEEP_DURATION * (int64_t)NSEC_PER_SEC;
	unsigned long slept, cycles;
	unsigned long *done;
	uint64_t rc6, ts[2];
	int fd;

	fd = open_pmu(i915, I915_PMU_RC6_RESIDENCY);
	igt_require(__pmu_wait_for_rc6(fd));

	/* While idle check full RC6. */
	rc6 = -__pmu_read_single(fd, &ts[0]);
	slept = measured_usleep(duration_ns / 1000);
	rc6 += __pmu_read_single(fd, &ts[1]);
	igt_debug("slept=%lu perf=%"PRIu64", rc6=%"PRIu64"\n",
		  slept, ts[1] - ts[0], rc6);
	assert_within_epsilon(rc6, ts[1] - ts[0], 5);

	/* Setup up a very light load */
	done = mmap(0, 4096, PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
	igt_fork(child, 1) {
		struct drm_i915_gem_exec_object2 obj = {
			.handle = batch_create(i915),
		};
		struct drm_i915_gem_execbuffer2 execbuf = {
			.buffers_ptr = to_user_pointer(&obj),
			.buffer_count = 1,
		};

		do {
			struct timespec tv = {};

			igt_nsec_elapsed(&tv);

			gem_execbuf(i915, &execbuf);
			while (gem_bo_busy(i915, obj.handle))
				usleep(0);
			done[1]++;

			usleep(igt_nsec_elapsed(&tv) / 10); /* => 1% busy */
		} while (!READ_ONCE(*done));
	}

	/* While very nearly idle (idle to within tolerance), expect full RC6 */
	cycles = -READ_ONCE(done[1]);
	rc6 = -__pmu_read_single(fd, &ts[0]);
	slept = measured_usleep(duration_ns / 1000);
	rc6 += __pmu_read_single(fd, &ts[1]);
	cycles += READ_ONCE(done[1]);
	igt_debug("slept=%lu perf=%"PRIu64", cycles=%lu, rc6=%"PRIu64"\n",
		  slept, ts[1] - ts[0], cycles, rc6);

	*done = 1;
	igt_waitchildren();
	munmap(done, 4096);
	close(fd);

	igt_assert(cycles >= SLEEP_DURATION); /* At least one wakeup/s needed */
	assert_within_epsilon(rc6, ts[1] - ts[0], 5);
}

igt_main
{
	unsigned int rc6_enabled = 0;
	unsigned int devid = 0;
	int i915 = -1;

	/* Use drm_open_driver to verify device existence */
	igt_fixture {
		i915 = drm_open_driver(DRIVER_INTEL);
		devid = intel_get_drm_devid(i915);
		sysfs = igt_sysfs_open(i915);

		igt_require(has_rc6_residency("rc6"));

		/* Make sure rc6 counters are running */
		igt_drop_caches_set(i915, DROP_IDLE);
		igt_require(wait_for_rc6());

		rc6_enabled = get_rc6_enabled_mask();
		igt_require(rc6_enabled & RC6_ENABLED);
	}

	igt_subtest("rc6-idle") {
		igt_require_gem(i915);
		gem_quiescent_gpu(i915);

		rc6_idle(i915);
	}

	igt_subtest("rc6-accuracy") {
		struct residencies res;

		measure_residencies(devid, rc6_enabled, &res);
		residency_accuracy(res.rc6, res.duration, "rc6");
	}

	igt_subtest("media-rc6-accuracy") {
		struct residencies res;

		igt_require(IS_VALLEYVIEW(devid) || IS_CHERRYVIEW(devid));

		measure_residencies(devid, rc6_enabled, &res);
		residency_accuracy(res.media_rc6, res.duration, "media_rc6");
	}

	igt_fixture
		close(i915);

}
