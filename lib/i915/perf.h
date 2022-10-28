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

#define _DIV_ROUND_UP(a, b)  (((a) + (b) - 1) / (b))

#define INTEL_DEVICE_MAX_SLICES           (6)  /* Maximum on gfx10 */
#define INTEL_DEVICE_MAX_SUBSLICES        (8)  /* Maximum on gfx11 */
#define INTEL_DEVICE_MAX_EUS_PER_SUBSLICE (16) /* Maximum on gfx12 */

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
	uint32_t graphics_ver;
	uint32_t revision;
	/**
	 * Bit shifting required to put OA report timestamps into
	 * timestamp_frequency (some HW generations can shift
	 * timestamp values to the right by a number of bits).
	 */
	int32_t  oa_timestamp_shift;
	/**
	 * On some platforms only part of the timestamp bits are valid
	 * (on previous platforms we would get full 32bits, newer
	 * platforms can have fewer). It's important to know when
	 * correlating the full 36bits timestamps to the OA report
	 * timestamps.
	 */
	uint64_t  oa_timestamp_mask;
	/* Frequency of the timestamps in Hz */
	uint64_t timestamp_frequency;
	uint64_t gt_min_freq;
	uint64_t gt_max_freq;

	/* Total number of EUs */
	uint64_t n_eus;
	/* Total number of EUs in a slice */
	uint64_t n_eu_slices;
	/* Total number of subslices/dualsubslices */
	uint64_t n_eu_sub_slices;
	/* Number of subslices/dualsubslices in the first half of the
	 * slices.
	 */
	uint64_t n_eu_sub_slices_half_slices;
	/* Mask of available subslices/dualsubslices */
	uint64_t subslice_mask;
	/* Mask of available slices */
	uint64_t slice_mask;
	/* Number of threads in one EU */
	uint64_t eu_threads_count;

	/**
	 * Maximu number of slices present on this device (can be more than
	 * num_slices if some slices are fused).
	 */
	uint16_t max_slices;

	/**
	 * Maximu number of subslices per slice present on this device (can be more
	 * than the maximum value in the num_subslices[] array if some subslices are
	 * fused).
	 */
	uint16_t max_subslices_per_slice;

	/**
	 * Stride to access subslice_masks[].
	 */
	uint16_t subslice_slice_stride;

	/**
	 * Maximum number of EUs per subslice (can be more than
	 * num_eu_per_subslice if some EUs are fused off).
	 */
	uint16_t max_eu_per_subslice;

	/**
	 * Strides to access eu_masks[].
	 */
	uint16_t eu_slice_stride;
	uint16_t eu_subslice_stride;

	/**
	 * A bit mask of the slices available.
	 */
	uint8_t slice_masks[_DIV_ROUND_UP(INTEL_DEVICE_MAX_SLICES, 8)];

	/**
	 * An array of bit mask of the subslices available, use subslice_slice_stride
	 * to access this array.
	 */
	uint8_t subslice_masks[INTEL_DEVICE_MAX_SLICES *
			       _DIV_ROUND_UP(INTEL_DEVICE_MAX_SUBSLICES, 8)];

	/**
	 * An array of bit mask of EUs available, use eu_slice_stride &
	 * eu_subslice_stride to access this array.
	 */
	uint8_t eu_masks[INTEL_DEVICE_MAX_SLICES *
			 INTEL_DEVICE_MAX_SUBSLICES *
			 _DIV_ROUND_UP(INTEL_DEVICE_MAX_EUS_PER_SUBSLICE, 8)];
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
	INTEL_PERF_LOGICAL_COUNTER_UNIT_GBPS,

	INTEL_PERF_LOGICAL_COUNTER_UNIT_MAX
} intel_perf_logical_counter_unit_t;

/* Hold deltas of raw performance counters. */
struct intel_perf_accumulator {
#define INTEL_PERF_MAX_RAW_OA_COUNTERS 64
	uint64_t deltas[INTEL_PERF_MAX_RAW_OA_COUNTERS];
};

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
	int perfcnt_offset;

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

struct drm_i915_perf_record_header;
struct drm_i915_query_topology_info;

static inline bool
intel_perf_devinfo_slice_available(const struct intel_perf_devinfo *devinfo,
				   int slice)
{
	return (devinfo->slice_masks[slice / 8] & (1U << (slice % 8))) != 0;
}

static inline bool
intel_perf_devinfo_subslice_available(const struct intel_perf_devinfo *devinfo,
				      int slice, int subslice)
{
	return (devinfo->subslice_masks[slice * devinfo->subslice_slice_stride +
					subslice / 8] & (1U << (subslice % 8))) != 0;
}

static inline bool
intel_perf_devinfo_eu_available(const struct intel_perf_devinfo *devinfo,
				int slice, int subslice, int eu)
{
	unsigned subslice_offset = slice * devinfo->eu_slice_stride +
		subslice * devinfo->eu_subslice_stride;

	return (devinfo->eu_masks[subslice_offset + eu / 8] & (1U << eu % 8)) != 0;
}

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

void intel_perf_load_perf_configs(struct intel_perf *perf, int drm_fd);

void intel_perf_accumulate_reports(struct intel_perf_accumulator *acc,
				   const struct intel_perf *perf,
				   const struct intel_perf_metric_set *metric_set,
				   const struct drm_i915_perf_record_header *record0,
				   const struct drm_i915_perf_record_header *record1);

uint64_t intel_perf_read_record_timestamp(const struct intel_perf *perf,
					  const struct intel_perf_metric_set *metric_set,
					  const struct drm_i915_perf_record_header *record);

uint64_t intel_perf_read_record_timestamp_raw(const struct intel_perf *perf,
					      const struct intel_perf_metric_set *metric_set,
					      const struct drm_i915_perf_record_header *record);

const char *intel_perf_read_report_reason(const struct intel_perf *perf,
					  const struct drm_i915_perf_record_header *record);

#ifdef __cplusplus
};
#endif

#endif /* PERF_METRICS_H */
