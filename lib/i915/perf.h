/*
 * Copyright (C) 2015-2016 Intel Corporation
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

#ifndef PERF_METRICS_H
#define PERF_METRICS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#include "igt_list.h"

struct intel_device_info;

struct intel_perf_devinfo {
	char devname[20];
	char prettyname[100];

	/*
	 * Always false for gputop, we don't have the additional
	 * snapshots of register values, only the OA reports.
	 */
	bool query_mode;

	bool has_dynamic_configs;

	/* The following fields are prepared for equations from the XML files.
	 * Their values are build up from the topology fields.
	 */
	uint32_t devid;
	uint32_t gen;
	uint32_t revision;
	uint64_t timestamp_frequency;
	uint64_t gt_min_freq;
	uint64_t gt_max_freq;

	uint64_t n_eus;
	uint64_t n_eu_slices;
	uint64_t n_eu_sub_slices;
	uint64_t subslice_mask;
	uint64_t slice_mask;
	uint64_t eu_threads_count;
};

typedef enum {
	INTEL_PERF_LOGICAL_COUNTER_STORAGE_UINT64,
	INTEL_PERF_LOGICAL_COUNTER_STORAGE_UINT32,
	INTEL_PERF_LOGICAL_COUNTER_STORAGE_DOUBLE,
	INTEL_PERF_LOGICAL_COUNTER_STORAGE_FLOAT,
	INTEL_PERF_LOGICAL_COUNTER_STORAGE_BOOL32,
} intel_perf_logical_counter_storage_t;

typedef enum {
	INTEL_PERF_LOGICAL_COUNTER_TYPE_RAW,
	INTEL_PERF_LOGICAL_COUNTER_TYPE_DURATION_RAW,
	INTEL_PERF_LOGICAL_COUNTER_TYPE_DURATION_NORM,
	INTEL_PERF_LOGICAL_COUNTER_TYPE_EVENT,
	INTEL_PERF_LOGICAL_COUNTER_TYPE_THROUGHPUT,
	INTEL_PERF_LOGICAL_COUNTER_TYPE_TIMESTAMP,
} intel_perf_logical_counter_type_t;

typedef enum {
	/* size */
	INTEL_PERF_LOGICAL_COUNTER_UNIT_BYTES,

	/* frequency */
	INTEL_PERF_LOGICAL_COUNTER_UNIT_HZ,

	/* time */
	INTEL_PERF_LOGICAL_COUNTER_UNIT_NS,
	INTEL_PERF_LOGICAL_COUNTER_UNIT_US,

	/**/
	INTEL_PERF_LOGICAL_COUNTER_UNIT_PIXELS,
	INTEL_PERF_LOGICAL_COUNTER_UNIT_TEXELS,
	INTEL_PERF_LOGICAL_COUNTER_UNIT_THREADS,
	INTEL_PERF_LOGICAL_COUNTER_UNIT_PERCENT,

	/* events */
	INTEL_PERF_LOGICAL_COUNTER_UNIT_MESSAGES,
	INTEL_PERF_LOGICAL_COUNTER_UNIT_NUMBER,
	INTEL_PERF_LOGICAL_COUNTER_UNIT_CYCLES,
	INTEL_PERF_LOGICAL_COUNTER_UNIT_EVENTS,
	INTEL_PERF_LOGICAL_COUNTER_UNIT_UTILIZATION,

	/**/
	INTEL_PERF_LOGICAL_COUNTER_UNIT_EU_SENDS_TO_L3_CACHE_LINES,
	INTEL_PERF_LOGICAL_COUNTER_UNIT_EU_ATOMIC_REQUESTS_TO_L3_CACHE_LINES,
	INTEL_PERF_LOGICAL_COUNTER_UNIT_EU_REQUESTS_TO_L3_CACHE_LINES,
	INTEL_PERF_LOGICAL_COUNTER_UNIT_EU_BYTES_PER_L3_CACHE_LINE,

	INTEL_PERF_LOGICAL_COUNTER_UNIT_MAX
} intel_perf_logical_counter_unit_t;


struct intel_perf;
struct intel_perf_metric_set;
struct intel_perf_logical_counter {
	const struct intel_perf_metric_set *metric_set;
	const char *name;
	const char *symbol_name;
	const char *desc;
	intel_perf_logical_counter_storage_t storage;
	intel_perf_logical_counter_type_t type;
	intel_perf_logical_counter_unit_t unit;
	union {
		uint64_t (*max_uint64)(const struct intel_perf *perf,
				       const struct intel_perf_metric_set *metric_set,
				       uint64_t *deltas);
		double (*max_float)(const struct intel_perf *perf,
				    const struct intel_perf_metric_set *metric_set,
				    uint64_t *deltas);
	};

	union {
		uint64_t (*read_uint64)(const struct intel_perf *perf,
					const struct intel_perf_metric_set *metric_set,
					uint64_t *deltas);
		double (*read_float)(const struct intel_perf *perf,
				     const struct intel_perf_metric_set *metric_set,
				     uint64_t *deltas);
	};

	struct igt_list_head link; /* list from intel_perf_logical_counter_group.counters */
};

struct intel_perf_register_prog {
	uint32_t reg;
	uint32_t val;
};

struct intel_perf_metric_set {
	const char *name;
	const char *symbol_name;
	const char *hw_config_guid;

	struct intel_perf_logical_counter *counters;
	int n_counters;

	uint64_t perf_oa_metrics_set;
	int perf_oa_format;
	int perf_raw_size;

	/* For indexing into accumulator->deltas[] ... */
	int gpu_time_offset;
	int gpu_clock_offset;
	int a_offset;
	int b_offset;
	int c_offset;

	struct intel_perf_register_prog *b_counter_regs;
	uint32_t n_b_counter_regs;

	struct intel_perf_register_prog *mux_regs;
	uint32_t n_mux_regs;

	struct intel_perf_register_prog *flex_regs;
	uint32_t n_flex_regs;

	struct igt_list_head link;
};

/* A tree structure with group having subgroups and counters. */
struct intel_perf_logical_counter_group {
	char *name;

	struct igt_list_head counters;
	struct igt_list_head groups;

	struct igt_list_head link;  /* link for intel_perf_logical_counter_group.groups */
};

struct intel_perf {
	const char *name;

	struct intel_perf_logical_counter_group *root_group;

	struct igt_list_head metric_sets;

	struct intel_perf_devinfo devinfo;
};

struct drm_i915_query_topology_info;

struct intel_perf *intel_perf_for_fd(int drm_fd);
struct intel_perf *intel_perf_for_devinfo(uint32_t device_id,
					  uint32_t revision,
					  uint64_t timestamp_frequency,
					  uint64_t gt_min_freq,
					  uint64_t gt_max_freq,
					  const struct drm_i915_query_topology_info *topology);
void intel_perf_free(struct intel_perf *perf);

void intel_perf_add_logical_counter(struct intel_perf *perf,
				    struct intel_perf_logical_counter *counter,
				    const char *group);

void intel_perf_add_metric_set(struct intel_perf *perf,
			       struct intel_perf_metric_set *metric_set);

#ifdef __cplusplus
};
#endif

#endif /* PERF_METRICS_H */
