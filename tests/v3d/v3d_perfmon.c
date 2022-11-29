// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Igalia S.L.
 */

#include "igt.h"
#include "igt_v3d.h"

IGT_TEST_DESCRIPTION("Tests for the V3D's performance monitors");

igt_main
{
	int fd;

	igt_fixture
		fd = drm_open_driver(DRIVER_V3D);

	igt_describe("Make sure a perfmon cannot be created with zero counters.");
	igt_subtest("create-perfmon-0") {
		struct drm_v3d_perfmon_create create = {
			.ncounters = 0,
		};
		do_ioctl_err(fd, DRM_IOCTL_V3D_PERFMON_CREATE, &create, EINVAL);
	}

	igt_describe("Make sure a perfmon cannot be created with more counters than the maximum allowed.");
	igt_subtest("create-perfmon-exceed") {
		struct drm_v3d_perfmon_create create = {
			.ncounters = DRM_V3D_MAX_PERF_COUNTERS + 1,
		};
		do_ioctl_err(fd, DRM_IOCTL_V3D_PERFMON_CREATE, &create, EINVAL);
	}

	igt_describe("Make sure a perfmon cannot be created with invalid counters identifiers.");
	igt_subtest("create-perfmon-invalid-counters") {
		struct drm_v3d_perfmon_create create = {
			.ncounters = 1,
			.counters = { V3D_PERFCNT_NUM },
		};
		do_ioctl_err(fd, DRM_IOCTL_V3D_PERFMON_CREATE, &create, EINVAL);
	}

	igt_describe("Make sure a perfmon with 1 counter can be created.");
	igt_subtest("create-single-perfmon") {
		uint8_t counters[] = { V3D_PERFCNT_FEP_VALID_PRIMTS_NO_PIXELS };
		uint32_t id = igt_v3d_perfmon_create(fd, 1, counters);

		igt_v3d_perfmon_destroy(fd, id);
	}

	igt_describe("Make sure that two perfmons can be created simultaneously.");
	igt_subtest("create-two-perfmon") {
		uint8_t counters_perfmon1[] = { V3D_PERFCNT_AXI_WRITE_STALLS_WATCH_0 };
		uint8_t counters_perfmon2[] = { V3D_PERFCNT_L2T_TMUCFG_READS, V3D_PERFCNT_CORE_MEM_WRITES };

		/* Create two different performance monitors */
		uint32_t id1 = igt_v3d_perfmon_create(fd, 1, counters_perfmon1);
		uint32_t id2 = igt_v3d_perfmon_create(fd, 2, counters_perfmon2);

		/* Make sure that the id's of the performance monitors are different */
		igt_assert_neq(id1, id2);

		igt_v3d_perfmon_destroy(fd, id1);

		/* Make sure that the second perfmon it is still acessible */
		igt_v3d_perfmon_get_values(fd, id2);

		igt_v3d_perfmon_destroy(fd, id2);
	}

	igt_describe("Make sure that getting the values from perfmon fails for non-zero pad.");
	igt_subtest("get-values-invalid-pad") {
		struct drm_v3d_perfmon_get_values get = {
			.pad = 1,
		};
		do_ioctl_err(fd, DRM_IOCTL_V3D_PERFMON_GET_VALUES, &get, EINVAL);
	}

	igt_describe("Make sure that getting the values from perfmon fails for invalid identifier.");
	igt_subtest("get-values-invalid-perfmon") {
		struct drm_v3d_perfmon_get_values get = {
			.id = 1,
		};
		do_ioctl_err(fd, DRM_IOCTL_V3D_PERFMON_GET_VALUES, &get, EINVAL);
	}

	igt_describe("Make sure that getting the values from perfmon fails for invalid memory pointer.");
	igt_subtest("get-values-invalid-pointer") {
		uint8_t counters[] = { V3D_PERFCNT_TLB_QUADS_STENCIL_FAIL,
				       V3D_PERFCNT_PTB_PRIM_VIEWPOINT_DISCARD,
				       V3D_PERFCNT_QPU_UC_HIT };
		uint32_t id = igt_v3d_perfmon_create(fd, 3, counters);

		struct drm_v3d_perfmon_get_values get = {
			.id = id,
			.values_ptr = 0ULL
		};

		do_ioctl_err(fd, DRM_IOCTL_V3D_PERFMON_GET_VALUES, &get, EFAULT);

		igt_v3d_perfmon_destroy(fd, id);
	}

	igt_describe("Sanity check for getting the values from a valid perfmon.");
	igt_subtest("get-values-valid-perfmon") {
		uint8_t counters[] = { V3D_PERFCNT_COMPUTE_ACTIVE,
				       V3D_PERFCNT_PTB_MEM_READS,
				       V3D_PERFCNT_CLE_ACTIVE };
		uint32_t id = igt_v3d_perfmon_create(fd, 3, counters);

		igt_v3d_perfmon_get_values(fd, id);
		igt_v3d_perfmon_destroy(fd, id);
	}

	igt_describe("Make sure that destroying a non-existent perfmon fails.");
	igt_subtest("destroy-invalid-perfmon") {
		struct drm_v3d_perfmon_destroy destroy = {
			.id = 1,
		};
		do_ioctl_err(fd, DRM_IOCTL_V3D_PERFMON_DESTROY, &destroy, EINVAL);
	}

	igt_describe("Make sure that a perfmon is not accessible after being destroyed.");
	igt_subtest("destroy-valid-perfmon") {
		uint8_t counters[] = { V3D_PERFCNT_AXI_WRITE_STALLS_WATCH_1,
				       V3D_PERFCNT_TMU_CONFIG_ACCESSES,
				       V3D_PERFCNT_TLB_PARTIAL_QUADS,
				       V3D_PERFCNT_L2T_SLC0_READS };
		uint32_t id = igt_v3d_perfmon_create(fd, 4, counters);
		struct drm_v3d_perfmon_get_values get = {
			.id = id,
		};

		igt_v3d_perfmon_get_values(fd, id);

		igt_v3d_perfmon_destroy(fd, id);

		/* Make sure that the id is no longer allocate */
		do_ioctl_err(fd, DRM_IOCTL_V3D_PERFMON_GET_VALUES, &get, EINVAL);
	}

	igt_fixture
		close(fd);
}
