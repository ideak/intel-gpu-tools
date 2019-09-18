/*
 * Copyright (C) 2019 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef PERF_DATA_H
#define PERF_DATA_H

#ifdef __cplusplus
extern "C" {
#endif

#include <i915_drm.h>

/* The structures below are embedded in the i915-perf stream so as to
 * provide metadata. The types used in the
 * drm_i915_perf_record_header.type are defined in
 * intel_perf_record_type.
 */

#include <stdint.h>

enum intel_perf_record_type {
	/* Start at 65536, which is pretty safe since after 3years the
	 * kernel hasn't defined more than 3 entries.
	 */

	INTEL_PERF_RECORD_TYPE_VERSION = 1 << 16,

	/* intel_perf_record_device_info */
	INTEL_PERF_RECORD_TYPE_DEVICE_INFO,

	/* intel_perf_record_device_topology */
	INTEL_PERF_RECORD_TYPE_DEVICE_TOPOLOGY,

	/* intel_perf_record_timestamp_correlation */
	INTEL_PERF_RECORD_TYPE_TIMESTAMP_CORRELATION,
};

/* This structure cannot ever change. */
struct intel_perf_record_version {
	/* Version of the i915-perf file recording format (effectively
	 * versioning this file).
	 */
	uint32_t version;

#define INTEL_PERF_RECORD_VERSION (1)

	uint32_t pad;
} __attribute__((packed));

struct intel_perf_record_device_info {
	/* Frequency of the timestamps in the records. */
	uint64_t timestamp_frequency;

	/* PCI ID */
	uint32_t device_id;

	/* Stepping */
	uint32_t device_revision;

	/* GT min/max frequencies */
	uint32_t gt_min_frequency;
	uint32_t gt_max_frequency;

	/* Engine */
	uint32_t engine_class;
	uint32_t engine_instance;

	/* enum drm_i915_oa_format */
	uint32_t oa_format;

	/* Metric set name */
	char metric_set_name[256];

	/* Configuration identifier */
	char metric_set_uuid[40];

	uint32_t pad;
 } __attribute__((packed));

/* Topology as reported by i915 (variable length, aligned by the
 * recorder). */
struct intel_perf_record_device_topology {
	struct drm_i915_query_topology_info topology;
};

/* Timestamp correlation between CPU/GPU. */
struct intel_perf_record_timestamp_correlation {
	/* In CLOCK_MONOTONIC */
	uint64_t cpu_timestamp;

	/* Engine timestamp associated with the OA unit */
	uint64_t gpu_timestamp;
} __attribute__((packed));

#ifdef __cplusplus
};
#endif

#endif /* PERF_DATA_H */
