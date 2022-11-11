// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Igalia S.L.
 */

#include "igt.h"
#include "igt_vc4.h"

IGT_TEST_DESCRIPTION("Tests for the VC4's performance monitors");

igt_main
{
	int fd;

	igt_fixture {
		fd = drm_open_driver(DRIVER_VC4);
		igt_require(igt_vc4_is_v3d(fd));
	}

	igt_describe("Make sure a perfmon cannot be created with zero counters.");
	igt_subtest("create-perfmon-0") {
		struct drm_vc4_perfmon_create create = {
			.ncounters = 0,
		};
		do_ioctl_err(fd, DRM_IOCTL_VC4_PERFMON_CREATE, &create, EINVAL);
	}

	igt_describe("Make sure a perfmon cannot be created with more counters than the maximum allowed.");
	igt_subtest("create-perfmon-exceed") {
		struct drm_vc4_perfmon_create create = {
			.ncounters = DRM_VC4_MAX_PERF_COUNTERS + 1,
		};
		do_ioctl_err(fd, DRM_IOCTL_VC4_PERFMON_CREATE, &create, EINVAL);
	}

	igt_describe("Make sure a perfmon cannot be created with invalid events identifiers.");
	igt_subtest("create-perfmon-invalid-events") {
		struct drm_vc4_perfmon_create create = {
			.ncounters = 1,
			.events = { VC4_PERFCNT_NUM_EVENTS },
		};
		do_ioctl_err(fd, DRM_IOCTL_VC4_PERFMON_CREATE, &create, EINVAL);
	}

	igt_describe("Make sure a perfmon with 1 counter can be created.");
	igt_subtest("create-single-perfmon") {
		uint8_t events[] = { VC4_PERFCNT_FEP_VALID_PRIMS_NO_RENDER };
		uint32_t id = igt_vc4_perfmon_create(fd, 1, events);

		igt_vc4_perfmon_destroy(fd, id);
	}

	igt_describe("Make sure that two perfmons can be created simultaneously.");
	igt_subtest("create-two-perfmon") {
		uint8_t events_perfmon1[] = { VC4_PERFCNT_FEP_VALID_QUADS };
		uint8_t events_perfmon2[] = { VC4_PERFCNT_L2C_TOTAL_L2_CACHE_HIT, VC4_PERFCNT_QPU_TOTAL_UNIFORM_CACHE_MISS };

		/* Create two different performance monitors */
		uint32_t id1 = igt_vc4_perfmon_create(fd, 1, events_perfmon1);
		uint32_t id2 = igt_vc4_perfmon_create(fd, 2, events_perfmon2);

		/* Make sure that the id's of the performance monitors are different */
		igt_assert_neq(id1, id2);

		igt_vc4_perfmon_destroy(fd, id1);

		/* Make sure that the second perfmon it is still acessible */
		igt_vc4_perfmon_get_values(fd, id2);

		igt_vc4_perfmon_destroy(fd, id2);
	}

	igt_describe("Make sure that getting the values from perfmon fails for invalid identifier.");
	igt_subtest("get-values-invalid-perfmon") {
		struct drm_vc4_perfmon_get_values get = {
			.id = 1,
		};
		do_ioctl_err(fd, DRM_IOCTL_VC4_PERFMON_GET_VALUES, &get, EINVAL);
	}

	igt_describe("Make sure that getting the values from perfmon fails for invalid memory pointer.");
	igt_subtest("get-values-invalid-pointer") {
		uint8_t counters[] = { VC4_PERFCNT_TLB_QUADS_ZERO_COVERAGE,
				       VC4_PERFCNT_PLB_PRIMS_OUTSIDE_VIEWPORT,
				       VC4_PERFCNT_QPU_TOTAL_INST_CACHE_HIT };
		uint32_t id = igt_vc4_perfmon_create(fd, 3, counters);

		struct drm_vc4_perfmon_get_values get = {
			.id = id,
			.values_ptr = 0ULL
		};

		do_ioctl_err(fd, DRM_IOCTL_VC4_PERFMON_GET_VALUES, &get, EFAULT);

		igt_vc4_perfmon_destroy(fd, id);
	}

	igt_describe("Sanity check for getting the values from a valid perfmon.");
	igt_subtest("get-values-valid-perfmon") {
		uint8_t events[] = { VC4_PERFCNT_VPM_TOTAL_CLK_CYCLES_VDW_STALLED,
				       VC4_PERFCNT_PSE_PRIMS_REVERSED,
				       VC4_PERFCNT_QPU_TOTAL_INST_CACHE_HIT };
		uint32_t id = igt_vc4_perfmon_create(fd, 3, events);

		igt_vc4_perfmon_get_values(fd, id);
		igt_vc4_perfmon_destroy(fd, id);
	}

	igt_describe("Make sure that destroying a non-existent perfmon fails.");
	igt_subtest("destroy-invalid-perfmon") {
		struct drm_vc4_perfmon_destroy destroy = {
			.id = 1,
		};
		do_ioctl_err(fd, DRM_IOCTL_VC4_PERFMON_DESTROY, &destroy, EINVAL);
	}

	igt_describe("Make sure that a perfmon is not accessible after being destroyed.");
	igt_subtest("destroy-valid-perfmon") {
		uint8_t events[] = { VC4_PERFCNT_QPU_TOTAL_CLK_CYCLES_EXEC_VALID_INST,
				       VC4_PERFCNT_FEP_VALID_QUADS,
				       VC4_PERFCNT_TMU_TOTAL_TEXT_CACHE_MISS,
				       VC4_PERFCNT_L2C_TOTAL_L2_CACHE_MISS };
		uint32_t id = igt_vc4_perfmon_create(fd, 4, events);
		struct drm_vc4_perfmon_get_values get = {
			.id = id,
		};

		igt_vc4_perfmon_get_values(fd, id);

		igt_vc4_perfmon_destroy(fd, id);

		/* Make sure that the id is no longer allocate */
		do_ioctl_err(fd, DRM_IOCTL_VC4_PERFMON_GET_VALUES, &get, EINVAL);
	}

	igt_fixture
		close(fd);
}
