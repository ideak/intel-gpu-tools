/*
 * Copyright (C) 2020 Intel Corporation
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
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <i915_drm.h>

#include "igt_core.h"
#include "intel_chipset.h"
#include "i915/perf.h"
#include "i915/perf_data_reader.h"

#define MAX(a,b) ((a) > (b) ? (a) : (b))
#define MIN(a,b) ((a) > (b) ? (b) : (a))

static void
usage(void)
{
	printf("Usage: i915-perf-reader [options] file\n"
	       "Reads the content of an i915-perf recording.\n"
	       "\n"
	       "     --help,    -h             Print this screen\n"
	       "     --counters, -c c1,c2,...  List of counters to display values for.\n"
	       "                               Use 'all' to display all counters.\n"
	       "                               Use 'list' to list available counters.\n");
}

static struct intel_perf_logical_counter *
find_counter(struct intel_perf_metric_set *metric_set,
	     const char *name)
{
	for (uint32_t i = 0; i < metric_set->n_counters; i++) {
		if (!strcmp(name, metric_set->counters[i].symbol_name)) {
			return &metric_set->counters[i];
		}
	}

	return NULL;
}

static void
append_counter(struct intel_perf_logical_counter ***counters,
	       int32_t *n_counters,
	       uint32_t *n_allocated_counters,
	       struct intel_perf_logical_counter *counter)
{
	if (*n_counters < *n_allocated_counters) {
		(*counters)[(*n_counters)++] = counter;
		return;
	}

	*n_allocated_counters = MAX(2, *n_allocated_counters * 2);
	*counters = realloc(*counters,
			    sizeof(struct intel_perf_logical_counter *) *
			    (*n_allocated_counters));
	(*counters)[(*n_counters)++] = counter;
}

static struct intel_perf_logical_counter **
get_logical_counters(struct intel_perf_metric_set *metric_set,
		     const char *counter_list,
		     int32_t *out_n_counters)
{
	struct intel_perf_logical_counter **counters = NULL, *counter;
	uint32_t n_allocated_counters = 0;
	const char *current, *next;
	char counter_name[100];

	if (!counter_list) {
		*out_n_counters = 0;
		return NULL;
	}

	if (!strcmp(counter_list, "list")) {
		uint32_t longest_name = 0;

		*out_n_counters = -1;
		for (uint32_t i = 0; i < metric_set->n_counters; i++) {
			longest_name = MAX(longest_name,
					   strlen(metric_set->counters[i].symbol_name));
		}

		fprintf(stdout, "Available counters:\n");
		for (uint32_t i = 0; i < metric_set->n_counters; i++) {
			fprintf(stdout, "%s:%*s%s\n",
				metric_set->counters[i].symbol_name,
				(int)(longest_name -
				      strlen(metric_set->counters[i].symbol_name) + 1), " ",
				metric_set->counters[i].name);
		}
		return NULL;
	}

	if (!strcmp(counter_list, "all")) {
		counters = malloc(sizeof(*counters) * metric_set->n_counters);
		*out_n_counters = metric_set->n_counters;
		for (uint32_t i = 0; i < metric_set->n_counters; i++)
			counters[i] = &metric_set->counters[i];
		return counters;
	}

	*out_n_counters = 0;
	current = counter_list;
	while ((next = strstr(current, ","))) {
		snprintf(counter_name,
			 MIN((uint32_t)(next - current) + 1, sizeof(counter_name)),
			 "%s", current);

		counter = find_counter(metric_set, counter_name);
		if (!counter) {
			fprintf(stderr, "Unknown counter '%s'.\n", counter_name);
			free(counters);
			*out_n_counters = -1;
			return NULL;
		}

		append_counter(&counters, out_n_counters, &n_allocated_counters, counter);

		current = next + 1;
	}

	if (strlen(current) > 0) {
		counter = find_counter(metric_set, current);
		if (!counter) {
			fprintf(stderr, "Unknown counter '%s'.\n", current);
			free(counters);
			*out_n_counters = -1;
			return NULL;
		}

		append_counter(&counters, out_n_counters, &n_allocated_counters, counter);
	}

	return counters;
}

int
main(int argc, char *argv[])
{
	const struct option long_options[] = {
		{"help",             no_argument, 0, 'h'},
		{"counters",   required_argument, 0, 'c'},
		{0, 0, 0, 0}
	};
	struct intel_perf_data_reader reader;
	struct intel_perf_logical_counter **counters;
	const struct intel_device_info *devinfo;
	const char *counter_names = NULL;
	int32_t n_counters;
	int fd, opt;

	while ((opt = getopt_long(argc, argv, "hc:", long_options, NULL)) != -1) {
		switch (opt) {
		case 'h':
			usage();
			return EXIT_SUCCESS;
		case 'c':
			counter_names = optarg;
			break;
		default:
			fprintf(stderr, "Internal error: "
				"unexpected getopt value: %d\n", opt);
			usage();
			return EXIT_FAILURE;
		}
	}

	if (optind >= argc) {
		fprintf(stderr, "No recording file specified.\n");
		return EXIT_FAILURE;
	}

	fd = open(argv[optind], 0, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "Cannot open '%s': %s.\n",
			argv[optind], strerror(errno));
		return EXIT_FAILURE;
	}

	if (!intel_perf_data_reader_init(&reader, fd)) {
		fprintf(stderr, "Unable to parse '%s': %s.\n",
			argv[optind], reader.error_msg);
		return EXIT_FAILURE;
	}

	counters = get_logical_counters(reader.metric_set, counter_names, &n_counters);
	if (n_counters < 0)
		goto exit;

	devinfo = intel_get_device_info(reader.devinfo.devid);

	fprintf(stdout, "Recorded on device=0x%x(%s) graphics_ver=%i\n",
		reader.devinfo.devid, devinfo->codename,
		reader.devinfo.graphics_ver);
	fprintf(stdout, "Metric used : %s (%s) uuid=%s\n",
		reader.metric_set->symbol_name, reader.metric_set->name,
		reader.metric_set->hw_config_guid);
	fprintf(stdout, "Reports: %u\n", reader.n_records);
	fprintf(stdout, "Context switches: %u\n", reader.n_timelines);
	fprintf(stdout, "Timestamp correlation points: %u\n", reader.n_correlations);

	if (strcmp(reader.metric_set_uuid, reader.metric_set->hw_config_guid)) {
		fprintf(stdout,
			"WARNING: Recording used a different HW configuration.\n"
			"WARNING: This could lead to inconsistent counter values.\n");
	}

	for (uint32_t i = 0; i < reader.n_timelines; i++) {
		const struct intel_perf_timeline_item *item = &reader.timelines[i];
		const struct drm_i915_perf_record_header *i915_report0 =
			reader.records[item->record_start];
		const struct drm_i915_perf_record_header *i915_report1 =
			reader.records[item->record_end];
		struct intel_perf_accumulator accu;

		fprintf(stdout, "Time: CPU=0x%016" PRIx64 "-0x%016" PRIx64
			" GPU=0x%016" PRIx64 "-0x%016" PRIx64"\n",
			item->cpu_ts_start, item->cpu_ts_end,
			item->ts_start, item->ts_end);
		fprintf(stdout, "hw_id=0x%x %s\n",
			item->hw_id, item->hw_id == 0xffffffff ? "(idle)" : "");

		intel_perf_accumulate_reports(&accu, reader.metric_set->perf_oa_format,
					      i915_report0, i915_report1);

		for (uint32_t c = 0; c < n_counters; c++) {
			struct intel_perf_logical_counter *counter = counters[c];

			switch (counter->storage) {
			case INTEL_PERF_LOGICAL_COUNTER_STORAGE_UINT64:
			case INTEL_PERF_LOGICAL_COUNTER_STORAGE_UINT32:
			case INTEL_PERF_LOGICAL_COUNTER_STORAGE_BOOL32:
				fprintf(stdout, "   %s: %" PRIu64 "\n",
					counter->symbol_name, counter->read_uint64(reader.perf,
										   reader.metric_set,
										   accu.deltas));
				break;
			case INTEL_PERF_LOGICAL_COUNTER_STORAGE_DOUBLE:
			case INTEL_PERF_LOGICAL_COUNTER_STORAGE_FLOAT:
				fprintf(stdout, "   %s: %f\n",
					counter->symbol_name, counter->read_float(reader.perf,
										  reader.metric_set,
										  accu.deltas));
				break;
			}
		}
	}

 exit:
	intel_perf_data_reader_fini(&reader);
	close(fd);

	return EXIT_SUCCESS;
}
