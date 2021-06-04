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

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <i915_drm.h>

#include "intel_chipset.h"
#include "perf.h"
#include "perf_data_reader.h"

#define MAX(a,b) ((a) > (b) ? (a) : (b))
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

static inline bool
oa_report_ctx_is_valid(const struct intel_perf_devinfo *devinfo,
		       const uint8_t *_report)
{
	const uint32_t *report = (const uint32_t *) _report;

	if (devinfo->graphics_ver < 8) {
		return false; /* TODO */
	} else if (devinfo->graphics_ver == 8) {
		return report[0] & (1ul << 25);
	} else if (devinfo->graphics_ver > 8) {
		return report[0] & (1ul << 16);
	}

	return false;
}

static uint32_t
oa_report_ctx_id(const struct intel_perf_devinfo *devinfo, const uint8_t *report)
{
	if (!oa_report_ctx_is_valid(devinfo, report))
		return 0xffffffff;
	return ((const uint32_t *) report)[2];
}

static inline uint64_t
oa_report_timestamp(const uint8_t *report)
{
	return ((const uint32_t *)report)[1];
}

static void
append_record(struct intel_perf_data_reader *reader,
	      const struct drm_i915_perf_record_header *header)
{
	if (reader->n_records >= reader->n_allocated_records) {
		reader->n_allocated_records = MAX(100, 2 * reader->n_allocated_records);
		reader->records =
			(const struct drm_i915_perf_record_header **)
			realloc((void *) reader->records,
				reader->n_allocated_records *
				sizeof(struct drm_i915_perf_record_header *));
		assert(reader->records);
	}

	reader->records[reader->n_records++] = header;
}

static void
append_timestamp_correlation(struct intel_perf_data_reader *reader,
			     const struct intel_perf_record_timestamp_correlation *corr)
{
	if (reader->n_correlations >= reader->n_allocated_correlations) {
		reader->n_allocated_correlations = MAX(100, 2 * reader->n_allocated_correlations);
		reader->correlations =
			(const struct intel_perf_record_timestamp_correlation **)
			realloc((void *) reader->correlations,
				reader->n_allocated_correlations *
				sizeof(*reader->correlations));
		assert(reader->correlations);
	}

	reader->correlations[reader->n_correlations++] = corr;
}

static struct intel_perf_metric_set *
find_metric_set(struct intel_perf *perf, const char *symbol_name)
{
	struct intel_perf_metric_set *metric_set;

	igt_list_for_each_entry(metric_set, &perf->metric_sets, link) {
		if (!strcmp(symbol_name, metric_set->symbol_name))
			return metric_set;
	}

	return NULL;
}

static bool
parse_data(struct intel_perf_data_reader *reader)
{
	const struct intel_perf_record_device_info *record_info;
	const struct intel_perf_record_device_topology *record_topology;
	const uint8_t *end = reader->mmap_data + reader->mmap_size;
	const uint8_t *iter = reader->mmap_data;

	while (iter < end) {
		const struct drm_i915_perf_record_header *header =
			(const struct drm_i915_perf_record_header *) iter;

		switch (header->type) {
		case DRM_I915_PERF_RECORD_SAMPLE:
			append_record(reader, header);
			break;

		case DRM_I915_PERF_RECORD_OA_REPORT_LOST:
		case DRM_I915_PERF_RECORD_OA_BUFFER_LOST:
			assert(header->size == sizeof(*header));
			break;

		case INTEL_PERF_RECORD_TYPE_VERSION: {
			struct intel_perf_record_version *version =
				(struct intel_perf_record_version*) (header + 1);
			if (version->version != INTEL_PERF_RECORD_VERSION) {
				snprintf(reader->error_msg, sizeof(reader->error_msg),
					 "Unsupported recording version (%u, expected %u)",
					 version->version, INTEL_PERF_RECORD_VERSION);
				return false;
			}
			break;
		}

		case INTEL_PERF_RECORD_TYPE_DEVICE_INFO: {
			reader->record_info = header + 1;
			assert(header->size == (sizeof(struct intel_perf_record_device_info) +
						sizeof(*header)));
			break;
		}

		case INTEL_PERF_RECORD_TYPE_DEVICE_TOPOLOGY: {
			reader->record_topology = header + 1;
			break;
		}

		case INTEL_PERF_RECORD_TYPE_TIMESTAMP_CORRELATION: {
			append_timestamp_correlation(reader,
						     (const struct intel_perf_record_timestamp_correlation *) (header + 1));
			break;
		}
		}

		iter += header->size;
	}

	if (!reader->record_info ||
	    !reader->record_topology) {
		snprintf(reader->error_msg, sizeof(reader->error_msg),
			 "Invalid file, missing device or topology info");
		return false;
	}

	record_info = reader->record_info;
	record_topology = reader->record_topology;

	reader->perf = intel_perf_for_devinfo(record_info->device_id,
					      record_info->device_revision,
					      record_info->timestamp_frequency,
					      record_info->gt_min_frequency,
					      record_info->gt_max_frequency,
					      &record_topology->topology);
	if (!reader->perf) {
		snprintf(reader->error_msg, sizeof(reader->error_msg),
			 "Recording occured on unsupported device (0x%x)",
			 record_info->device_id);
		return false;
	}

	reader->devinfo = reader->perf->devinfo;

	reader->metric_set_name = record_info->metric_set_name;
	reader->metric_set_uuid = record_info->metric_set_uuid;
	reader->metric_set = find_metric_set(reader->perf, record_info->metric_set_name);

	return true;
}

static uint64_t
correlate_gpu_timestamp(struct intel_perf_data_reader *reader,
			uint64_t gpu_ts)
{
	/* OA reports only have the lower 32bits of the timestamp
	 * register, while our correlation data has the whole 36bits.
	 * Try to figure what portion of the correlation data the
	 * 32bit timestamp belongs to.
	 */
	uint64_t mask = 0xffffffff;
	int corr_idx = -1;

	for (uint32_t i = 0; i < reader->n_correlation_chunks; i++) {
		if (gpu_ts >= (reader->correlation_chunks[i].gpu_ts_begin & mask) &&
		    gpu_ts <= (reader->correlation_chunks[i].gpu_ts_end & mask)) {
			corr_idx = reader->correlation_chunks[i].idx;
			break;
		}
	}

	/* Not found? Assume prior to the first timestamp correlation.
	 */
	if (corr_idx < 0) {
		return reader->correlations[0]->cpu_timestamp -
			((reader->correlations[0]->gpu_timestamp & mask) - gpu_ts) *
			(reader->correlations[1]->cpu_timestamp - reader->correlations[0]->cpu_timestamp) /
			(reader->correlations[1]->gpu_timestamp - reader->correlations[0]->gpu_timestamp);
	}

	for (uint32_t i = corr_idx; i < (reader->n_correlations - 1); i++) {
		if (gpu_ts >= (reader->correlations[i]->gpu_timestamp & mask) &&
		    gpu_ts < (reader->correlations[i + 1]->gpu_timestamp & mask)) {
			return reader->correlations[i]->cpu_timestamp +
				(gpu_ts - (reader->correlations[i]->gpu_timestamp & mask)) *
				(reader->correlations[i + 1]->cpu_timestamp - reader->correlations[i]->cpu_timestamp) /
				(reader->correlations[i + 1]->gpu_timestamp - reader->correlations[i]->gpu_timestamp);
		}
	}

	/* This is a bit harsh, but the recording tool should ensure we have
	 * sampling points on either side of the bag of OA reports.
	 */
	assert(0);
}

static void
append_timeline_event(struct intel_perf_data_reader *reader,
		      uint64_t ts_start, uint64_t ts_end,
		      uint32_t record_start, uint32_t record_end,
		      uint32_t hw_id)
{
	if (reader->n_timelines >= reader->n_allocated_timelines) {
		reader->n_allocated_timelines = MAX(100, 2 * reader->n_allocated_timelines);
		reader->timelines =
			(struct intel_perf_timeline_item *)
			realloc((void *) reader->timelines,
				reader->n_allocated_timelines *
				sizeof(*reader->timelines));
		assert(reader->timelines);
	}

	reader->timelines[reader->n_timelines].ts_start = ts_start;
	reader->timelines[reader->n_timelines].ts_end = ts_end;
	reader->timelines[reader->n_timelines].cpu_ts_start =
		correlate_gpu_timestamp(reader, ts_start);
	reader->timelines[reader->n_timelines].cpu_ts_end =
		correlate_gpu_timestamp(reader, ts_end);
	reader->timelines[reader->n_timelines].record_start = record_start;
	reader->timelines[reader->n_timelines].record_end = record_end;
	reader->timelines[reader->n_timelines].hw_id = hw_id;
	reader->n_timelines++;
}

static void
generate_cpu_events(struct intel_perf_data_reader *reader)
{
	uint32_t last_header_idx = 0;
	const struct drm_i915_perf_record_header *last_header = reader->records[0],
		*current_header = reader->records[0];
	const uint8_t *start_report, *end_report;
	uint32_t last_ctx_id, current_ctx_id;
	uint64_t gpu_ts_start, gpu_ts_end;

	for (uint32_t i = 1; i < reader->n_records; i++) {
		current_header = reader->records[i];

		start_report = (const uint8_t *) (last_header + 1);
		end_report = (const uint8_t *) (current_header + 1);

		last_ctx_id = oa_report_ctx_id(&reader->devinfo, start_report);
		current_ctx_id = oa_report_ctx_id(&reader->devinfo, end_report);

		gpu_ts_start = oa_report_timestamp(start_report);
		gpu_ts_end = oa_report_timestamp(end_report);

		if (last_ctx_id == current_ctx_id)
			continue;

		append_timeline_event(reader, gpu_ts_start, gpu_ts_end, last_header_idx, i, last_ctx_id);

		last_header = current_header;
		last_header_idx = i;
	}

	if (last_header != current_header)
		append_timeline_event(reader, gpu_ts_start, gpu_ts_end, last_header_idx, reader->n_records - 1, last_ctx_id);
}

static void
compute_correlation_chunks(struct intel_perf_data_reader *reader)
{
	uint64_t mask = ~(0xffffffff);
	uint32_t last_idx = 0;
	uint64_t last_ts = reader->correlations[last_idx]->gpu_timestamp;

	for (uint32_t i = 0; i < reader->n_correlations; i++) {
		if (!reader->n_correlation_chunks ||
		    (last_ts & mask) != (reader->correlations[i]->gpu_timestamp & mask)) {
			assert(reader->n_correlation_chunks < ARRAY_SIZE(reader->correlation_chunks));
			reader->correlation_chunks[reader->n_correlation_chunks].gpu_ts_begin = last_ts;
			reader->correlation_chunks[reader->n_correlation_chunks].gpu_ts_end = last_ts | ~mask;
			reader->correlation_chunks[reader->n_correlation_chunks].idx = last_idx;
			last_ts = reader->correlation_chunks[reader->n_correlation_chunks].gpu_ts_end + 1;
			last_idx = i;
			reader->n_correlation_chunks++;
		}
	}
}

bool
intel_perf_data_reader_init(struct intel_perf_data_reader *reader,
			    int perf_file_fd)
{
        struct stat st;
        if (fstat(perf_file_fd, &st) != 0) {
		snprintf(reader->error_msg, sizeof(reader->error_msg),
			 "Unable to access file (%s)", strerror(errno));
		return false;
	}

	memset(reader, 0, sizeof(*reader));

	reader->mmap_size = st.st_size;
	reader->mmap_data = (const uint8_t *) mmap(NULL, st.st_size,
						   PROT_READ, MAP_PRIVATE,
						   perf_file_fd, 0);
	if (reader->mmap_data == MAP_FAILED) {
		snprintf(reader->error_msg, sizeof(reader->error_msg),
			 "Unable to access file (%s)", strerror(errno));
		return false;
	}

	if (!parse_data(reader))
		return false;

	compute_correlation_chunks(reader);
	generate_cpu_events(reader);

	return true;
}

void
intel_perf_data_reader_fini(struct intel_perf_data_reader *reader)
{
	intel_perf_free(reader->perf);
	free(reader->records);
	free(reader->timelines);
	free(reader->correlations);
	munmap((void *)reader->mmap_data, reader->mmap_size);
}
