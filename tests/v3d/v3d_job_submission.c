// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Igalia S.L.
 */

#include "igt.h"
#include "igt_v3d.h"
#include "igt_syncobj.h"

static int fd;

IGT_TEST_DESCRIPTION("Tests that combines Command List (CL) and Compute Shader Dispatch (CSD) jobs.");

/* Number of Command List (CL) jobs to be submitted. */
#define NUM_CL_JOBS  1000

/* Number of Compute Shader Dispatch (CSD) jobs to be submitted. */
#define NUM_CSD_JOBS 250

static int syncobj_wait_array(uint32_t *handles, uint32_t count)
{
	int i, ret = 0;

	for (i = 0; i < count; i++) {
		ret = syncobj_wait_err(fd, &handles[i], 1, INT64_MAX, 0);
		if (ret)
			return ret;
	}

	return ret;
}

static void *create_cl_jobs(void *args)
{
	struct v3d_cl_job **jobs = args;
	int i;

	for (i = 0; i < NUM_CL_JOBS; i++) {
		do_ioctl(fd, DRM_IOCTL_V3D_SUBMIT_CL, jobs[i]->submit);
		igt_assert(syncobj_wait(fd, &jobs[i]->submit->out_sync, 1,
					INT64_MAX, 0, NULL));
	}

	return NULL;
}

static void *create_csd_jobs(void *args)
{
	struct v3d_csd_job **jobs = args;
	int i;

	for (i = 0; i < NUM_CSD_JOBS; i++) {
		do_ioctl(fd, DRM_IOCTL_V3D_SUBMIT_CSD, jobs[i]->submit);
		igt_assert(syncobj_wait(fd, &jobs[i]->submit->out_sync, 1,
					INT64_MAX, 0, NULL));
	}

	return NULL;
}

igt_main
{
	igt_fixture {
		fd = drm_open_driver(DRIVER_V3D);
		igt_require(igt_v3d_get_param(fd, DRM_V3D_PARAM_SUPPORTS_CSD));
		igt_require(igt_v3d_get_param(fd, DRM_V3D_PARAM_SUPPORTS_MULTISYNC_EXT));
	}

	igt_describe("Test if the out-sync of an array of mixed jobs is behaving correctly.");
	igt_subtest("array-job-submission") {
		uint32_t handles[4];
		struct v3d_cl_job *cl_jobs[2];
		struct v3d_csd_job *csd_jobs[2];
		int i;

		for (i = 0; i < ARRAY_SIZE(handles); i++)
			handles[i] = syncobj_create(fd, 0);

		for (i = 0; i < 2; i++) {
			cl_jobs[i] = igt_v3d_noop_job(fd);
			csd_jobs[i] = igt_v3d_empty_shader(fd);
		}

		cl_jobs[0]->submit->out_sync =  handles[0];
		csd_jobs[0]->submit->out_sync =  handles[1];
		cl_jobs[1]->submit->out_sync =  handles[2];
		csd_jobs[1]->submit->out_sync =  handles[3];

		do_ioctl(fd, DRM_IOCTL_V3D_SUBMIT_CL, cl_jobs[0]->submit);
		igt_assert_eq(syncobj_wait_array(handles, ARRAY_SIZE(handles)), -EINVAL);

		do_ioctl(fd, DRM_IOCTL_V3D_SUBMIT_CSD, csd_jobs[0]->submit);
		igt_assert_eq(syncobj_wait_array(handles, ARRAY_SIZE(handles)), -EINVAL);

		do_ioctl(fd, DRM_IOCTL_V3D_SUBMIT_CL, cl_jobs[1]->submit);
		igt_assert_eq(syncobj_wait_array(handles, ARRAY_SIZE(handles)), -EINVAL);

		do_ioctl(fd, DRM_IOCTL_V3D_SUBMIT_CSD, csd_jobs[1]->submit);
		igt_assert_eq(syncobj_wait_array(handles, ARRAY_SIZE(handles)), 0);

		for (i = 0; i < 2; i++) {
			igt_v3d_free_cl_job(fd, cl_jobs[i]);
			igt_v3d_free_csd_job(fd, csd_jobs[i]);
		}
	}

	igt_describe("Test if multiple singlesyncs have the same behaviour as one multisync.");
	igt_subtest("multiple-singlesync-to-multisync") {
		struct drm_v3d_multi_sync ms = { 0 };
		uint32_t handles[4];
		struct v3d_cl_job *cl_jobs[2];
		struct v3d_csd_job *csd_jobs[2];
		struct drm_v3d_sem *in_syncs, *out_syncs;
		int i;

		for (i = 0; i < ARRAY_SIZE(handles); i++)
			handles[i] = syncobj_create(fd, 0);

		for (i = 0; i < 2; i++) {
			cl_jobs[i] = igt_v3d_noop_job(fd);
			csd_jobs[i] = igt_v3d_empty_shader(fd);
		}

		cl_jobs[0]->submit->out_sync =  handles[0];
		csd_jobs[0]->submit->out_sync =  handles[1];
		cl_jobs[1]->submit->out_sync =  handles[2];

		igt_v3d_set_multisync(&ms, V3D_CSD);
		ms.in_sync_count = 3;
		ms.out_sync_count = 1;

		in_syncs = malloc(ms.in_sync_count * sizeof(*in_syncs));
		out_syncs = malloc(ms.out_sync_count * sizeof(*in_syncs));

		for (i = 0; i < ms.in_sync_count; i++)
			in_syncs[i].handle = handles[i];

		out_syncs[0].handle = handles[3];

		ms.in_syncs = to_user_pointer(in_syncs);
		ms.out_syncs = to_user_pointer(out_syncs);

		csd_jobs[1]->submit->flags = DRM_V3D_SUBMIT_EXTENSION;
		csd_jobs[1]->submit->extensions = to_user_pointer(&ms);

		do_ioctl(fd, DRM_IOCTL_V3D_SUBMIT_CL, cl_jobs[0]->submit);

		do_ioctl_err(fd, DRM_IOCTL_V3D_SUBMIT_CSD, csd_jobs[1]->submit, EINVAL);
		igt_assert_eq(syncobj_wait_array(handles, ARRAY_SIZE(handles)), -EINVAL);

		do_ioctl(fd, DRM_IOCTL_V3D_SUBMIT_CSD, csd_jobs[0]->submit);

		do_ioctl_err(fd, DRM_IOCTL_V3D_SUBMIT_CSD, csd_jobs[1]->submit, EINVAL);
		igt_assert_eq(syncobj_wait_array(handles, ARRAY_SIZE(handles)), -EINVAL);

		do_ioctl(fd, DRM_IOCTL_V3D_SUBMIT_CL, cl_jobs[1]->submit);
		igt_assert_eq(syncobj_wait_array(handles, ARRAY_SIZE(handles)), -EINVAL);

		do_ioctl(fd, DRM_IOCTL_V3D_SUBMIT_CSD, csd_jobs[1]->submit);
		igt_assert_eq(syncobj_wait_array(handles, ARRAY_SIZE(handles)), 0);

		for (i = 0; i < 2; i++) {
			igt_v3d_free_cl_job(fd, cl_jobs[i]);
			igt_v3d_free_csd_job(fd, csd_jobs[i]);
		}
	}

	igt_describe("Test if all queues are progressing independently.");
	igt_subtest("threaded-job-submission") {
		struct v3d_cl_job **cl_jobs = NULL;
		struct v3d_csd_job **csd_jobs = NULL;
		pthread_t *threads[2];
		int i, ret;

		cl_jobs = malloc(NUM_CL_JOBS * sizeof(*cl_jobs));
		csd_jobs = malloc(NUM_CSD_JOBS * sizeof(*csd_jobs));

		for (i = 0; i < NUM_CL_JOBS; i++) {
			igt_print_activity();

			cl_jobs[i] = igt_v3d_noop_job(fd);
			cl_jobs[i]->submit->out_sync = syncobj_create(fd,
								      DRM_SYNCOBJ_CREATE_SIGNALED);
		}

		for (i = 0; i < NUM_CSD_JOBS; i++) {
			igt_print_activity();

			csd_jobs[i] = igt_v3d_empty_shader(fd);
			csd_jobs[i]->submit->out_sync = syncobj_create(fd,
								       DRM_SYNCOBJ_CREATE_SIGNALED);
		}

		for (i = 0; i < ARRAY_SIZE(threads); i++) {
			threads[i] = malloc(sizeof(*threads[i]));
			igt_assert(threads[i]);
		}

		ret = pthread_create(threads[0], NULL, &create_cl_jobs, cl_jobs);
		igt_assert_eq(ret, 0);

		ret = pthread_create(threads[1], NULL, &create_csd_jobs, csd_jobs);
		igt_assert_eq(ret, 0);

		for (i = 0; i < ARRAY_SIZE(threads); i++)
			pthread_join(*threads[i], NULL);

		for (i = 0; i < NUM_CL_JOBS; i++)
			igt_v3d_free_cl_job(fd, cl_jobs[i]);

		for (i = 0; i < NUM_CSD_JOBS; i++)
			igt_v3d_free_csd_job(fd, csd_jobs[i]);

		for (i = 0; i < ARRAY_SIZE(threads); i++)
			free(threads[i]);

		free(cl_jobs);
		free(csd_jobs);
	}

	igt_fixture
		close(fd);
}
