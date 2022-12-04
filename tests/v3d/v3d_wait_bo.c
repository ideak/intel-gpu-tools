// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Igalia S.L.
 */

#include "igt.h"
#include "igt_v3d.h"
#include "v3d/v3d_cl.h"

IGT_TEST_DESCRIPTION("Tests for the V3D's Wait BO IOCTL");

static void test_used_bo(int fd, struct v3d_bo *bo, uint64_t timeout)
{
	struct drm_v3d_wait_bo arg = {
		.timeout_ns = timeout,
		.handle = bo->handle,
	};
	int ret;

	ret = igt_ioctl(fd, DRM_IOCTL_V3D_WAIT_BO, &arg);

	if (ret == -1 && errno == ETIME)
		igt_debug("Timeout triggered\n");
	igt_assert(ret == 0 || (ret == -1 && errno == ETIME));
}

igt_main
{
	int fd;
	struct v3d_bo *bo;

	igt_fixture {
		fd = drm_open_driver(DRIVER_V3D);
		bo = igt_v3d_create_bo(fd, PAGE_SIZE);
	}

	igt_describe("Make sure it cannot wait on an invalid BO.");
	igt_subtest("bad-bo") {
		struct drm_v3d_wait_bo arg = {
			.handle = bo->handle + 1,
			.timeout_ns = 0,
		};
		do_ioctl_err(fd, DRM_IOCTL_V3D_WAIT_BO, &arg, EINVAL);
	}

	igt_describe("Make sure the pad is zero.");
	igt_subtest("bad-pad") {
		struct drm_v3d_wait_bo arg = {
			.pad = 1,
			.handle = bo->handle,
			.timeout_ns = 0,
		};
		do_ioctl_err(fd, DRM_IOCTL_V3D_WAIT_BO, &arg, EINVAL);
	}

	igt_describe("Wait on an unused BO for 0 ns.");
	igt_subtest("unused-bo-0ns")
		igt_v3d_wait_bo(fd, bo, 0);

	igt_describe("Wait on an unused BO for 1 ns.");
	igt_subtest("unused-bo-1ns")
		igt_v3d_wait_bo(fd, bo, 1);

	igt_describe("Wait on a newly mapped BO for 0 ns.");
	igt_subtest("map-bo-0ns") {
		igt_v3d_bo_mmap(fd, bo);
		igt_v3d_wait_bo(fd, bo, 0);
		munmap(bo->map, bo->size);
	}

	igt_describe("Wait on a newly mapped BO for 1 ns.");
	igt_subtest("map-bo-1ns") {
		igt_v3d_bo_mmap(fd, bo);
		igt_v3d_wait_bo(fd, bo, 1);
		munmap(bo->map, bo->size);
	}

	igt_describe("Wait for BOs used for a noop job for 0 ns.");
	igt_subtest("used-bo-0ns") {
		struct v3d_cl_job *job = igt_v3d_noop_job(fd);

		do_ioctl(fd, DRM_IOCTL_V3D_SUBMIT_CL, job->submit);

		test_used_bo(fd, job->tile_alloc, 0);
		test_used_bo(fd, job->tile_state, 0);
		test_used_bo(fd, job->bcl->bo, 0);
		test_used_bo(fd, job->rcl->bo, 0);
		test_used_bo(fd, job->icl->bo, 0);

		igt_v3d_free_cl_job(fd, job);
	}

	igt_describe("Wait for BOs used for a noop job for 1 ns.");
	igt_subtest("used-bo-1ns") {
		struct v3d_cl_job *job = igt_v3d_noop_job(fd);

		do_ioctl(fd, DRM_IOCTL_V3D_SUBMIT_CL, job->submit);

		test_used_bo(fd, job->tile_alloc, 1);
		test_used_bo(fd, job->tile_state, 1);
		test_used_bo(fd, job->bcl->bo, 1);
		test_used_bo(fd, job->rcl->bo, 1);
		test_used_bo(fd, job->icl->bo, 1);

		igt_v3d_free_cl_job(fd, job);
	}

	igt_describe("Wait for BOs used for a noop job for a long amount of time.");
	igt_subtest("used-bo") {
		struct v3d_cl_job *job = igt_v3d_noop_job(fd);

		do_ioctl(fd, DRM_IOCTL_V3D_SUBMIT_CL, job->submit);

		igt_v3d_wait_bo(fd, job->tile_alloc, INT64_MAX);
		igt_v3d_wait_bo(fd, job->tile_state, INT64_MAX);
		igt_v3d_wait_bo(fd, job->bcl->bo, INT64_MAX);
		igt_v3d_wait_bo(fd, job->rcl->bo, INT64_MAX);
		igt_v3d_wait_bo(fd, job->icl->bo, INT64_MAX);

		igt_v3d_free_cl_job(fd, job);
	}

	igt_fixture {
		igt_v3d_free_bo(fd, bo);
		close(fd);
	}
}
