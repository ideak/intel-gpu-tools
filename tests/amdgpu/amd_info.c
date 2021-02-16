/*
 * Copyright 2014 Advanced Micro Devices, Inc.
 * Copyright 2021 Valve Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "config.h"

#include "igt.h"

#include <amdgpu.h>
#include <amdgpu_drm.h>

static amdgpu_device_handle dev;

static void query_firmware_version_test(void)
{
	struct amdgpu_gpu_info gpu_info = {};
	uint32_t version, feature;

	igt_assert_f(amdgpu_query_gpu_info(dev, &gpu_info) == 0,
		     "Failed to query the gpu information\n");

	igt_assert_f(amdgpu_query_firmware_version(dev, AMDGPU_INFO_FW_VCE, 0,
						   0, &version, &feature) == 0,
		     "Failed to query the firmware version\n");
}

static void query_timestamp_test(uint32_t sleep_time, int sample_count)
{
	struct amdgpu_gpu_info gpu_info = {};
	double median, std_err, err_95_conf;
	igt_stats_t stats;
	float ns_per_tick;
	int i;

	igt_stats_init_with_size(&stats, sample_count);

	/* figure out how many nanoseconds each gpu timestamp tick represents */
	igt_assert_f(amdgpu_query_gpu_info(dev, &gpu_info) == 0,
		     "Failed to query the gpu information\n");
	igt_assert_f(gpu_info.gpu_counter_freq > 0,
		     "The GPU counter frequency cannot be undefined\n");
	ns_per_tick = 1e9 / (gpu_info.gpu_counter_freq * 1000.0);

	/* acquire the data needed for the analysis */
	for (i = 0; i < sample_count; i++) {
		uint64_t ts_start, ts_end, cpu_delta;
		int64_t gpu_delta;
		float corrected_gpu_delta;
		struct timespec ts_cpu;

		igt_assert_f(igt_gettime(&ts_cpu) == 0,
			     "Failed to read the CPU-provided time");

		igt_assert_f(amdgpu_query_info(dev, AMDGPU_INFO_TIMESTAMP, 8,
					       &ts_start) == 0,
			     "Failed to query the GPU start timestamp\n");

		usleep(sleep_time);

		igt_assert_f(amdgpu_query_info(dev, AMDGPU_INFO_TIMESTAMP, 8,
					       &ts_end) == 0,
			     "Failed to query the GPU end timestamp\n");

		/* get the GPU and CPU deltas */
		cpu_delta = igt_nsec_elapsed(&ts_cpu);
		gpu_delta = ts_end - ts_start;
		corrected_gpu_delta = gpu_delta * ns_per_tick;

		/* make sure the GPU timestamps are ordered */
		igt_assert_f(gpu_delta > 0,
			     "The GPU time is not moving or is ticking in the "
			     "wrong direction (start=%lu, end=%lu, "
			     "end-start=%lu)\n", ts_start, ts_end, gpu_delta);

		igt_stats_push_float(&stats, corrected_gpu_delta / cpu_delta);
	}

	/* generate the statistics */
	median = igt_stats_get_median(&stats);
	std_err = igt_stats_get_std_error(&stats);
	err_95_conf = std_err * 1.96;

	/* check that the median ticking rate is ~1.0, meaning that the
	 * the GPU and CPU timestamps grow at the same rate
	 */
	igt_assert_f(median > 0.99 && median < 1.01,
		     "The GPU time elapses at %.2f%% (+/- %.2f%% at 95%% "
		     "confidence) of the CPU's speed\ngpu_counter_freq=%u kHz, "
		     "should be %.0f kHz (+/- %.1f kHz at 95%% confidence)\n",
		     median * 100.0, err_95_conf * 100.0,
		     gpu_info.gpu_counter_freq,
		     gpu_info.gpu_counter_freq * median,
		     gpu_info.gpu_counter_freq * err_95_conf);

	/* check the jitter in the ticking rate */
	igt_assert_f(err_95_conf < 0.01,
		     "The GPU time ticks with a jitter greater than 1%%, at "
		     "95%% confidence (+/- %.3f%%)\n", err_95_conf * 100.0);

	igt_stats_fini(&stats);
}

IGT_TEST_DESCRIPTION("Test the consistency of the data provided through the "
		     "DRM_AMDGPU_INFO IOCTL");
igt_main
{
	int fd = -1;

	igt_fixture {
		uint32_t major, minor;
		int err;

		fd = drm_open_driver(DRIVER_AMDGPU);

		err = amdgpu_device_initialize(fd, &major, &minor, &dev);
		igt_require(err == 0);

		igt_info("Initialized amdgpu, driver version %d.%d\n",
			 major, minor);
	}

	igt_describe("Make sure we can retrieve the firmware version");
	igt_subtest("query-firmware-version")
		query_firmware_version_test();

	igt_describe("Check that the GPU time ticks constantly, and at the "
		     "same rate as the CPU");
	igt_subtest("query-timestamp")
		query_timestamp_test(10000, 100);

	igt_describe("Check that the GPU time keeps on ticking, even during "
		     "long idle times which could lead to clock/power gating");
	igt_subtest("query-timestamp-while-idle")
		query_timestamp_test(7000000, 1);

	igt_fixture {
		amdgpu_device_deinitialize(dev);
		close(fd);
	}
}
