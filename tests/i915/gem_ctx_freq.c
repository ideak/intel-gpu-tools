/*
 * Copyright © 2018 Intel Corporation
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

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>

#include "igt.h"
#include "igt_perf.h"
#include "igt_sysfs.h"
#include "sw_sync.h"

#define SAMPLE_PERIOD (USEC_PER_SEC / 10)
#define PMU_TOLERANCE 100

static int sysfs = -1;

static void kick_rps_worker(void)
{
	sched_yield();
	usleep(SAMPLE_PERIOD);
}

static double measure_frequency(int pmu, int period_us)
{
	uint64_t data[2];
	uint64_t d_t, d_v;

	kick_rps_worker(); /* let the kthreads (intel_rps_work) run */

	igt_assert_eq(read(pmu, data, sizeof(data)), sizeof(data));
	d_v = -data[0];
	d_t = -data[1];

	usleep(period_us);

	igt_assert_eq(read(pmu, data, sizeof(data)), sizeof(data));
	d_v += data[0];
	d_t += data[1];

	return d_v * 1e9 / d_t;
}

static bool __pmu_within_tolerance(double actual, double target)
{
	return (actual > target - PMU_TOLERANCE &&
		actual < target + PMU_TOLERANCE);
}

static void pmu_assert(double actual, double target)
{
	igt_assert_f(__pmu_within_tolerance(actual, target),
		     "Measured frequency %.2fMHz, is beyond target %.0f±%dMhz\n",
		     actual, target, PMU_TOLERANCE);
}

static void busy_wait_until_idle(int i915, igt_spin_t *spin)
{
	igt_spin_end(spin);
	do {
		usleep(10000);
	} while (gem_bo_busy(i915, spin->handle));
}

static void __igt_spin_free_idle(int i915, igt_spin_t *spin)
{
	busy_wait_until_idle(i915, spin);

	igt_spin_free(i915, spin);
}

#define TRIANGLE_SIZE(x) (2 * (x) + 1)
static void triangle_fill(uint32_t *t, unsigned int nstep,
			  uint32_t min, uint32_t max)
{
	for (unsigned int step = 0; step <= 2*nstep; step++) {
		int frac = step > nstep ? 2*nstep - step : step;
		t[step] = min + (max - min) * frac / nstep;
	}
}

static void set_sysfs_freq(uint32_t min, uint32_t max)
{
	igt_sysfs_printf(sysfs, "gt_min_freq_mhz", "%u", min);
	igt_sysfs_printf(sysfs, "gt_max_freq_mhz", "%u", max);
}

static void get_sysfs_freq(uint32_t *min, uint32_t *max)
{
	igt_sysfs_scanf(sysfs, "gt_min_freq_mhz", "%u", min);
	igt_sysfs_scanf(sysfs, "gt_max_freq_mhz", "%u", max);
}

static void sysfs_range(int i915)
{
#define N_STEPS 10
	uint32_t frequencies[TRIANGLE_SIZE(N_STEPS)];
	uint32_t sys_min, sys_max;
	igt_spin_t *spin;
	double measured;
	int pmu;

	/*
	 * The sysfs interface sets the global limits and overrides the
	 * user's request. So we can to check that if the user requests
	 * a range outside of the sysfs, the requests are only run at the
	 * constriained sysfs range.
	 */

	get_sysfs_freq(&sys_min, &sys_max);
	igt_info("System min freq: %dMHz; max freq: %dMHz\n", sys_min, sys_max);

	triangle_fill(frequencies, N_STEPS, sys_min, sys_max);

	pmu = perf_i915_open(I915_PMU_REQUESTED_FREQUENCY);
	igt_require(pmu >= 0);

	for (int outer = 0; outer <= 2*N_STEPS; outer++) {
		uint32_t sys_freq = frequencies[outer];
		uint32_t cur, discard;

		gem_quiescent_gpu(i915);
		spin = igt_spin_new(i915);
		usleep(10000);

		set_sysfs_freq(sys_freq, sys_freq);
		get_sysfs_freq(&cur, &discard);

		measured = measure_frequency(pmu, SAMPLE_PERIOD);
		igt_debugfs_dump(i915, "i915_rps_boost_info");

		set_sysfs_freq(sys_min, sys_max);
		__igt_spin_free_idle(i915, spin);

		igt_info("sysfs: Measured %.1fMHz, expected %dMhz\n",
			 measured, cur);
		pmu_assert(measured, cur);
	}
	gem_quiescent_gpu(i915);

	close(pmu);

#undef N_STEPS
}

static void restore_sysfs_freq(int sig)
{
	char buf[256];

	if (igt_sysfs_read(sysfs, "gt_RPn_freq_mhz", buf, sizeof(buf)) > 0) {
		igt_sysfs_set(sysfs, "gt_idle_freq_mhz", buf);
		igt_sysfs_set(sysfs, "gt_min_freq_mhz", buf);
	}

	if (igt_sysfs_read(sysfs, "gt_RP0_freq_mhz", buf, sizeof(buf)) > 0) {
		igt_sysfs_set(sysfs, "gt_max_freq_mhz", buf);
		igt_sysfs_set(sysfs, "gt_boost_freq_mhz", buf);
	}
}

static void disable_boost(int i915)
{
	char *value;

	value = igt_sysfs_get(i915, "gt_RPn_freq_mhz");
	igt_sysfs_set(i915, "gt_min_freq_mhz", value);
	igt_sysfs_set(i915, "gt_boost_freq_mhz", value);
	free(value);

	value = igt_sysfs_get(i915, "gt_RP0_freq_mhz");
	igt_sysfs_set(i915, "gt_max_freq_mhz", value);
	free(value);
}

igt_main
{
	int i915 = -1;

	igt_fixture {
		i915 = drm_open_driver(DRIVER_INTEL);
		igt_require_gem(i915);

		sysfs = igt_sysfs_open(i915);
		igt_assert(sysfs != -1);
		igt_install_exit_handler(restore_sysfs_freq);

		disable_boost(sysfs);
	}

	igt_subtest_f("sysfs")
		sysfs_range(i915);
}
