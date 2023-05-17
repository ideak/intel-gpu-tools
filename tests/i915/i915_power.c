// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023 Intel Corporation
 */

#include "igt.h"
#include "i915/gem.h"
#include "igt_power.h"
#include "igt_sysfs.h"
/**
 * TEST: i915 power
 * Description: i915 power measurement tests
 * Feature: hwmon
 * Run type: FULL
 * Test category: GEM_Legacy
 *
 * SUBTEST: sanity
 * Description: Sanity check gpu power measurement
 */

IGT_TEST_DESCRIPTION("i915 power measurement tests");

static double measure_power(struct igt_power *pwr, uint32_t duration_sec)
{
	struct power_sample sample[2];

	igt_power_get_energy(pwr, &sample[0]);
	usleep(duration_sec * USEC_PER_SEC);
	igt_power_get_energy(pwr, &sample[1]);

	return igt_power_get_mW(pwr, &sample[0], &sample[1]);
}

static void sanity(int i915)
{
	const intel_ctx_t *ctx;
	struct igt_power pwr;
	double idle, busy;
	igt_spin_t *spin;
	uint64_t ahnd;
	int dir, gt, req, act;

#define DURATION_SEC 2

	/* Idle power */
	igt_require(!igt_power_open(i915, &pwr, "gpu"));
	gem_quiescent_gpu(i915);
	idle = measure_power(&pwr, DURATION_SEC);
	igt_info("Measured idle power: %g mW\n", idle);

	/* Busy power */
	ctx = intel_ctx_create_all_physical(i915);
	ahnd = get_reloc_ahnd(i915, ctx->id);
	spin = igt_spin_new(i915, .ahnd = ahnd, .ctx = ctx,
			    .engine = ALL_ENGINES,
			    .flags = IGT_SPIN_POLL_RUN);
	/* Wait till at least one spinner starts */
	igt_spin_busywait_until_started(spin);
	busy = measure_power(&pwr, DURATION_SEC);
	i915_for_each_gt(i915, dir, gt) {
		req = igt_sysfs_get_u32(dir, "rps_cur_freq_mhz");
		act = igt_sysfs_get_u32(dir, "rps_act_freq_mhz");
		igt_info("gt %d: req MHz: %d, act MHz: %d\n", gt, req, act);
	}
	igt_free_spins(i915);
	put_ahnd(ahnd);
	intel_ctx_destroy(i915, ctx);
	igt_power_close(&pwr);

	igt_info("Measured busy power: %g mW\n", busy);
	igt_assert(idle >= 0 && busy > 0 && busy > idle);
}

igt_main
{
	int i915;

	igt_fixture {
		igt_skip_on_simulation();
		i915 = drm_open_driver_master(DRIVER_INTEL);
	}

	igt_describe("Sanity check gpu power measurement");
	igt_subtest("sanity") {
		sanity(i915);
	}

	igt_fixture {
		close(i915);
	}
}
