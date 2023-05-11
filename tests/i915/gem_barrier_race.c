// SPDX-License-Identifier: MIT
/*
 * Copyright(c) 2023 Intel Corporation. All rights reserved.
 */

#include <stdint.h>

#include "drmtest.h"
#include "igt_aux.h"
#include "igt_core.h"
#include "igt_gt.h"
#include "intel_chipset.h"
#include "intel_reg.h"
#include "ioctl_wrappers.h"

#include "i915/gem.h"
#include "i915/gem_create.h"
#include "i915/gem_engine_topology.h"
#include "i915/perf.h"

IGT_TEST_DESCRIPTION("Exercise engine barriers and their interaction with other subsystems");

static void remote_request_workload(int fd, int *done)
{
	/*
	 * Use DRM_IOCTL_I915_PERF_OPEN / close as
	 * intel_context_prepare_remote_request() workload
	 *
	 * Based on code patterns found in tests/i915/perf.c
	 */
	struct intel_perf_metric_set *metric_set = NULL, *metric_set_iter;
	struct intel_perf *intel_perf = intel_perf_for_fd(fd, 0);
	uint64_t properties[] = {
		DRM_I915_PERF_PROP_SAMPLE_OA, true,
		DRM_I915_PERF_PROP_OA_METRICS_SET, 0,
		DRM_I915_PERF_PROP_OA_FORMAT, 0,
		DRM_I915_PERF_PROP_OA_EXPONENT, 5,
	};
	struct drm_i915_perf_open_param param = {
		.flags = I915_PERF_FLAG_FD_CLOEXEC | I915_PERF_FLAG_DISABLED,
		.num_properties = sizeof(properties) / 16,
		.properties_ptr = to_user_pointer(properties),
	};
	uint32_t devid = intel_get_drm_devid(fd);

	igt_require(intel_perf);
	intel_perf_load_perf_configs(intel_perf, fd);

	igt_require(devid);
	igt_list_for_each_entry(metric_set_iter, &intel_perf->metric_sets, link) {
		if (!strcmp(metric_set_iter->symbol_name,
			    IS_HASWELL(devid) ? "RenderBasic" : "TestOa")) {
			metric_set = metric_set_iter;
			break;
		}
	}
	igt_require(metric_set);
	igt_require(metric_set->perf_oa_metrics_set);
	properties[3] = metric_set->perf_oa_metrics_set;
	properties[5] = metric_set->perf_oa_format;

	intel_perf_free(intel_perf);

	igt_fork(child, 1) {
		do {
			int stream = igt_ioctl(fd, DRM_IOCTL_I915_PERF_OPEN, &param);

			igt_assert_fd(stream);
			close(stream);

		} while (!READ_ONCE(*done));
	}
}

/* Copied from tests/i915/gem_ctx_exec.c */
static int exec(int fd, uint32_t handle, int ring, int ctx_id)
{
	struct drm_i915_gem_exec_object2 obj = { .handle = handle };
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(&obj),
		.buffer_count = 1,
		.flags = ring,
	};

	i915_execbuffer2_set_context_id(execbuf, ctx_id);

	return __gem_execbuf(fd, &execbuf);
}

static void intel_context_first_pin_last_unpin_loop(int fd, uint64_t engine, int *done)
{
	/*
	 * Use gem_create -> gem_write -> gem_execbuf -> gem_sync -> gem_close
	 * as intel context first pin / last unpin intensive workload
	 */
	const uint32_t batch[2] = { 0, MI_BATCH_BUFFER_END };

	fd = drm_reopen_driver(fd);

	do {
		uint32_t handle = gem_create(fd, 4096);

		gem_write(fd, handle, 0, batch, sizeof(batch));
		igt_assert_eq(exec(fd, handle, engine, 0), 0);

		gem_sync(fd, handle);
		gem_close(fd, handle);

	} while (!READ_ONCE(*done));

	close(fd);
}

static void test_remote_request(int fd, uint64_t engine, unsigned int timeout)
{
	int *done = mmap(0, 4096, PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);

	igt_assert(done != MAP_FAILED);

	remote_request_workload(fd, done);

	igt_fork(child, sysconf(_SC_NPROCESSORS_ONLN))
		intel_context_first_pin_last_unpin_loop(fd, engine, done);

	sleep(timeout);
	*done = 1;
	igt_waitchildren();
	munmap(done, 4096);
}

igt_main
{
	int fd;

	igt_fixture {
		fd = drm_open_driver_render(DRIVER_INTEL);
		igt_require_gem(fd);
	}

	igt_describe("Race intel_context_prepare_remote_request against intel_context_active_acquire/release");
	igt_subtest_with_dynamic("remote-request") {
		struct intel_execution_engine2 *e;

		for_each_physical_engine(fd, e) {
			if (e->class != I915_ENGINE_CLASS_RENDER &&
			    e->class != I915_ENGINE_CLASS_COMPUTE)
				continue;

			igt_dynamic(e->name)
				test_remote_request(fd, e->flags, 5);

			/* We assume no need for all physical engines to be tested */
			break;
		}
	}
}
