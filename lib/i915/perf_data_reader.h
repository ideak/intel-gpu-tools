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

#ifndef PERF_DATA_READER_H
#define PERF_DATA_READER_H

#ifdef __cplusplus
extern "C" {
#endif

/* Helper to read a i915-perf recording. */

#include <stdbool.h>
#include <stdint.h>

#include "perf.h"
#include "perf_data.h"

struct intel_perf_timeline_item {
	uint64_t ts_start;
	uint64_t ts_end;
	uint64_t cpu_ts_start;
	uint64_t cpu_ts_end;

	/* Offsets into intel_perf_data_reader.records */
	uint32_t record_start;
	uint32_t record_end;

	uint32_t hw_id;

	/* User associated data with a given item on the i915 perf
	 * timeline.
	 */
	void *user_data;
};

struct intel_perf_data_reader {
	/* Array of pointers into the mmapped i915 perf file. */
	const struct drm_i915_perf_record_header **records;
	uint32_t n_records;
	uint32_t n_allocated_records;

	/**/
	struct intel_perf_timeline_item *timelines;
	uint32_t n_timelines;
	uint32_t n_allocated_timelines;

	/**/
	const struct intel_perf_record_timestamp_correlation **correlations;
	uint32_t n_correlations;
	uint32_t n_allocated_correlations;

	struct {
		uint64_t gpu_ts_begin;
		uint64_t gpu_ts_end;
		uint32_t idx;
	} correlation_chunks[4];
	uint32_t n_correlation_chunks;

	const char *metric_set_uuid;
	const char *metric_set_name;

	struct intel_perf_devinfo devinfo;

	struct intel_perf *perf;
	struct intel_perf_metric_set *metric_set;

	char error_msg[256];

	/**/
	const void *record_info;
	const void *record_topology;

	const uint8_t *mmap_data;
	size_t mmap_size;
};

bool intel_perf_data_reader_init(struct intel_perf_data_reader *reader,
				 int perf_file_fd);
void intel_perf_data_reader_fini(struct intel_perf_data_reader *reader);

#ifdef __cplusplus
};
#endif

#endif /* PERF_DATA_READER_H */
