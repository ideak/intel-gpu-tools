/*
 * Copyright (C) 2018 Intel Corporation
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
#include <dirent.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <unistd.h>

#include <i915_drm.h>

#include "intel_chipset.h"
#include "perf.h"

#include "i915_perf_metrics_hsw.h"
#include "i915_perf_metrics_bdw.h"
#include "i915_perf_metrics_chv.h"
#include "i915_perf_metrics_sklgt2.h"
#include "i915_perf_metrics_sklgt3.h"
#include "i915_perf_metrics_sklgt4.h"
#include "i915_perf_metrics_kblgt2.h"
#include "i915_perf_metrics_kblgt3.h"
#include "i915_perf_metrics_cflgt2.h"
#include "i915_perf_metrics_cflgt3.h"
#include "i915_perf_metrics_bxt.h"
#include "i915_perf_metrics_glk.h"
#include "i915_perf_metrics_cnl.h"
#include "i915_perf_metrics_icl.h"
#include "i915_perf_metrics_ehl.h"
#include "i915_perf_metrics_tglgt1.h"
#include "i915_perf_metrics_tglgt2.h"
#include "i915_perf_metrics_rkl.h"
#include "i915_perf_metrics_dg1.h"

static int
perf_ioctl(int fd, unsigned long request, void *arg)
{
	int ret;

	do {
		ret = ioctl(fd, request, arg);
	} while (ret == -1 && (errno == EINTR || errno == EAGAIN));
	return ret;
}

static struct intel_perf_logical_counter_group *
intel_perf_logical_counter_group_new(struct intel_perf *perf,
				     struct intel_perf_logical_counter_group *parent,
				     const char *name)
{
	struct intel_perf_logical_counter_group *group = calloc(1, sizeof(*group));

	group->name = strdup(name);

	IGT_INIT_LIST_HEAD(&group->counters);
	IGT_INIT_LIST_HEAD(&group->groups);

	if (parent)
		igt_list_add_tail(&group->link, &parent->groups);
	else
		IGT_INIT_LIST_HEAD(&group->link);

	return group;
}

static void
intel_perf_logical_counter_group_free(struct intel_perf_logical_counter_group *group)
{
	struct intel_perf_logical_counter_group *child, *tmp;

	igt_list_for_each_entry_safe(child, tmp, &group->groups, link) {
		igt_list_del(&child->link);
		intel_perf_logical_counter_group_free(child);
	}

	free(group->name);
	free(group);
}

static void
intel_perf_metric_set_free(struct intel_perf_metric_set *metric_set)
{
	free(metric_set->counters);
	free(metric_set);
}

static bool
slice_available(const struct drm_i915_query_topology_info *topo,
		int s)
{
	return (topo->data[s / 8] >> (s % 8)) & 1;
}

static bool
subslice_available(const struct drm_i915_query_topology_info *topo,
		   int s, int ss)
{
	return (topo->data[topo->subslice_offset +
			   s * topo->subslice_stride +
			   ss / 8] >> (ss % 8)) & 1;
}

static bool
eu_available(const struct drm_i915_query_topology_info *topo,
	     int s, int ss, int eu)
{
	return (topo->data[topo->eu_offset +
			   (s * topo->max_subslices + ss) * topo->eu_stride +
			   eu / 8] >> (eu % 8)) & 1;
}

static struct intel_perf *
unsupported_i915_perf_platform(struct intel_perf *perf)
{
	intel_perf_free(perf);
	return NULL;
}

struct intel_perf *
intel_perf_for_devinfo(uint32_t device_id,
		       uint32_t revision,
		       uint64_t timestamp_frequency,
		       uint64_t gt_min_freq,
		       uint64_t gt_max_freq,
		       const struct drm_i915_query_topology_info *topology)
{
	const struct intel_device_info *devinfo = intel_get_device_info(device_id);
	struct intel_perf *perf;
	int bits_per_subslice;

	if (!devinfo)
		return NULL;

	perf = calloc(1, sizeof(*perf));;
	perf->root_group = intel_perf_logical_counter_group_new(perf, NULL, "");

	IGT_INIT_LIST_HEAD(&perf->metric_sets);

	/* Initialize the device characterists first. Loading the
	 * metrics uses that information to detect whether some
	 * counters are available on a given device (for example BXT
	 * 2x6 does not have 2 samplers).
	 */
	perf->devinfo.devid = device_id;
	perf->devinfo.revision = revision;
	perf->devinfo.timestamp_frequency = timestamp_frequency;
	perf->devinfo.gt_min_freq = gt_min_freq;
	perf->devinfo.gt_max_freq = gt_max_freq;

	/* On Gen11+ the equations from the xml files expect an 8bits
	 * mask per subslice, versus only 3bits on prior Gens.
	 */
	bits_per_subslice = devinfo->gen >= 11 ? 8 : 3;
	for (uint32_t s = 0; s < topology->max_slices; s++) {
		if (!slice_available(topology, s))
			continue;

		perf->devinfo.slice_mask |= 1ULL << s;
		for (uint32_t ss = 0; ss < topology->max_subslices; ss++) {
			if (!subslice_available(topology, s, ss))
				continue;

			perf->devinfo.subslice_mask |= 1ULL << (s * bits_per_subslice + ss);

			for (uint32_t eu = 0; eu < topology->max_eus_per_subslice; eu++) {
				if (eu_available(topology, s, ss, eu))
					perf->devinfo.n_eus++;
			}
		}
	}

	perf->devinfo.n_eu_slices = __builtin_popcount(perf->devinfo.slice_mask);
	perf->devinfo.n_eu_sub_slices = __builtin_popcount(perf->devinfo.subslice_mask);

	/* Valid on most generations except Gen9LP. */
	perf->devinfo.eu_threads_count = 7;

	if (devinfo->is_haswell) {
		intel_perf_load_metrics_hsw(perf);
	} else if (devinfo->is_broadwell) {
		intel_perf_load_metrics_bdw(perf);
	} else if (devinfo->is_cherryview) {
		intel_perf_load_metrics_chv(perf);
	} else if (devinfo->is_skylake) {
		switch (devinfo->gt) {
		case 2:
			intel_perf_load_metrics_sklgt2(perf);
			break;
		case 3:
			intel_perf_load_metrics_sklgt3(perf);
			break;
		case 4:
			intel_perf_load_metrics_sklgt4(perf);
			break;
		default:
			return unsupported_i915_perf_platform(perf);
		}
	} else if (devinfo->is_broxton) {
		perf->devinfo.eu_threads_count = 6;
		intel_perf_load_metrics_bxt(perf);
	} else if (devinfo->is_kabylake) {
		switch (devinfo->gt) {
		case 2:
			intel_perf_load_metrics_kblgt2(perf);
			break;
		case 3:
			intel_perf_load_metrics_kblgt3(perf);
			break;
		default:
			return unsupported_i915_perf_platform(perf);
		}
	} else if (devinfo->is_geminilake) {
		perf->devinfo.eu_threads_count = 6;
		intel_perf_load_metrics_glk(perf);
	} else if (devinfo->is_coffeelake || devinfo->is_cometlake) {
		switch (devinfo->gt) {
		case 2:
			intel_perf_load_metrics_cflgt2(perf);
			break;
		case 3:
			intel_perf_load_metrics_cflgt3(perf);
			break;
		default:
			return unsupported_i915_perf_platform(perf);
		}
	} else if (devinfo->is_cannonlake) {
		intel_perf_load_metrics_cnl(perf);
	} else if (devinfo->is_icelake) {
		intel_perf_load_metrics_icl(perf);
	} else if (devinfo->is_elkhartlake) {
		intel_perf_load_metrics_ehl(perf);
	} else if (devinfo->is_tigerlake) {
		switch (devinfo->gt) {
		case 1:
			intel_perf_load_metrics_tglgt1(perf);
			break;
		case 2:
			intel_perf_load_metrics_tglgt2(perf);
			break;
		default:
			unsupported_i915_perf_platform(perf);
		}
	} else if (devinfo->is_rocketlake) {
		intel_perf_load_metrics_rkl(perf);
	} else if (devinfo->is_dg1) {
		intel_perf_load_metrics_dg1(perf);
	} else {
		return unsupported_i915_perf_platform(perf);
	}

	return perf;
}

static uint32_t
getparam(int drm_fd, uint32_t param)
{
        struct drm_i915_getparam gp;
        int val = -1;

        memset(&gp, 0, sizeof(gp));
        gp.param = param;
        gp.value = &val;

	perf_ioctl(drm_fd, DRM_IOCTL_I915_GETPARAM, &gp);

        return val;
}

static bool
read_fd_uint64(int fd, uint64_t *out_value)
{
	char buf[32];
	int n;

	n = read(fd, buf, sizeof (buf) - 1);
	if (n < 0)
		return false;

	buf[n] = '\0';
	*out_value = strtoull(buf, 0, 0);

	return true;
}

static bool
read_sysfs(int sysfs_dir_fd, const char *file_path, uint64_t *out_value)
{
	int fd = openat(sysfs_dir_fd, file_path, O_RDONLY);
	bool res;

	if (fd < 0)
		return false;

	res = read_fd_uint64(fd, out_value);
	close(fd);

	return res;
}

static int
query_items(int drm_fd, struct drm_i915_query_item *items, uint32_t n_items)
{
	struct drm_i915_query q = {
		.num_items = n_items,
		.items_ptr = (uintptr_t) items,
	};

	return perf_ioctl(drm_fd, DRM_IOCTL_I915_QUERY, &q);
}

static struct drm_i915_query_topology_info *
query_topology(int drm_fd)
{
	struct drm_i915_query_item item;
	struct drm_i915_query_topology_info *topo_info;
	int ret;

	memset(&item, 0, sizeof(item));
	item.query_id = DRM_I915_QUERY_TOPOLOGY_INFO;
	ret = query_items(drm_fd, &item, 1);
	if (ret < 0 || item.length < 0)
		return NULL;

	topo_info = calloc(1, item.length);
	item.data_ptr = (uintptr_t) topo_info;
	ret = query_items(drm_fd, &item, 1);
	if (ret < 0 || item.length < 0) {
		free(topo_info);
		return NULL;
	}

	return topo_info;
}

static int
open_master_sysfs_dir(int drm_fd)
{
	char path[128];
	struct stat st;

	if (fstat(drm_fd, &st) || !S_ISCHR(st.st_mode))
                return -1;

        snprintf(path, sizeof(path), "/sys/dev/char/%d:0",
                 major(st.st_rdev));

	return open(path, O_DIRECTORY);
}

struct intel_perf *
intel_perf_for_fd(int drm_fd)
{
	uint32_t device_id = getparam(drm_fd, I915_PARAM_CHIPSET_ID);
	uint32_t device_revision = getparam(drm_fd, I915_PARAM_REVISION);
	uint32_t timestamp_frequency = getparam(drm_fd, I915_PARAM_CS_TIMESTAMP_FREQUENCY);
	uint64_t gt_min_freq;
	uint64_t gt_max_freq;
	struct drm_i915_query_topology_info *topology;
	struct intel_perf *ret;
	int sysfs_dir_fd = open_master_sysfs_dir(drm_fd);

	if (sysfs_dir_fd < 0)
		return NULL;

	if (!read_sysfs(sysfs_dir_fd, "gt_min_freq_mhz", &gt_min_freq) ||
	    !read_sysfs(sysfs_dir_fd, "gt_max_freq_mhz", &gt_max_freq)) {
		close(sysfs_dir_fd);
		return NULL;
	}
	close(sysfs_dir_fd);

	topology = query_topology(drm_fd);
	if (!topology)
		return NULL;

	ret = intel_perf_for_devinfo(device_id,
				     device_revision,
				     timestamp_frequency,
				     gt_min_freq * 1000000,
				     gt_max_freq * 1000000,
				     topology);
	free(topology);

	return ret;
}

void
intel_perf_free(struct intel_perf *perf)
{
	struct intel_perf_metric_set *metric_set, *tmp;

	intel_perf_logical_counter_group_free(perf->root_group);

	igt_list_for_each_entry_safe(metric_set, tmp, &perf->metric_sets, link) {
		igt_list_del(&metric_set->link);
		intel_perf_metric_set_free(metric_set);
	}

	free(perf);
}

void
intel_perf_add_logical_counter(struct intel_perf *perf,
			       struct intel_perf_logical_counter *counter,
			       const char *group_path)
{
	const char *group_path_end = group_path + strlen(group_path);
	struct intel_perf_logical_counter_group *group = perf->root_group, *child_group = NULL;
	const char *name = group_path;

	while (name < group_path_end) {
		const char *name_end = strstr(name, "/");
		char group_name[128] = { 0, };
		struct intel_perf_logical_counter_group *iter_group;

		if (!name_end)
			name_end = group_path_end;

		memcpy(group_name, name, name_end - name);

		child_group = NULL;
		igt_list_for_each_entry(iter_group, &group->groups, link) {
			if (!strcmp(iter_group->name, group_name)) {
				child_group = iter_group;
				break;
			}
		}

		if (!child_group)
			child_group = intel_perf_logical_counter_group_new(perf, group, group_name);

		name = name_end + 1;
		group = child_group;
	}

	igt_list_add_tail(&counter->link, &child_group->counters);
}

void
intel_perf_add_metric_set(struct intel_perf *perf,
			  struct intel_perf_metric_set *metric_set)
{
	igt_list_add_tail(&metric_set->link, &perf->metric_sets);
}

static void
load_metric_set_config(struct intel_perf_metric_set *metric_set, int drm_fd)
{
	struct drm_i915_perf_oa_config config;
	int ret;

	memset(&config, 0, sizeof(config));

	memcpy(config.uuid, metric_set->hw_config_guid, sizeof(config.uuid));

	config.n_mux_regs = metric_set->n_mux_regs;
	config.mux_regs_ptr = (uintptr_t) metric_set->mux_regs;

	config.n_boolean_regs = metric_set->n_b_counter_regs;
	config.boolean_regs_ptr = (uintptr_t) metric_set->b_counter_regs;

	config.n_flex_regs = metric_set->n_flex_regs;
	config.flex_regs_ptr = (uintptr_t) metric_set->flex_regs;

	ret = perf_ioctl(drm_fd, DRM_IOCTL_I915_PERF_ADD_CONFIG, &config);
	if (ret >= 0)
		metric_set->perf_oa_metrics_set = ret;
}

void
intel_perf_load_perf_configs(struct intel_perf *perf, int drm_fd)
{
	int sysfs_dir_fd = open_master_sysfs_dir(drm_fd);
	struct dirent *entry;
	int metrics_dir_fd;
	DIR *metrics_dir;
	struct intel_perf_metric_set *metric_set;

	if (sysfs_dir_fd < 0)
		return;

	metrics_dir_fd = openat(sysfs_dir_fd, "metrics", O_DIRECTORY);
	close(sysfs_dir_fd);
	if (metrics_dir_fd < -1)
		return;

	metrics_dir = fdopendir(metrics_dir_fd);
	if (!metrics_dir) {
		close(metrics_dir_fd);
		return;
	}

	while ((entry = readdir(metrics_dir))) {
		bool metric_id_read;
		uint64_t metric_id;
		char path[256 + 4];
		int id_fd;

		if (entry->d_type != DT_DIR)
			continue;

		snprintf(path, sizeof(path), "%s/id", entry->d_name);

		id_fd = openat(metrics_dir_fd, path, O_RDONLY);
		if (id_fd < 0)
			continue;

		metric_id_read = read_fd_uint64(id_fd, &metric_id);
		close(id_fd);

		if (!metric_id_read)
			continue;

		igt_list_for_each_entry(metric_set, &perf->metric_sets, link) {
			if (!strcmp(metric_set->hw_config_guid, entry->d_name)) {
				metric_set->perf_oa_metrics_set = metric_id;
				break;
			}
		}
	}

	closedir(metrics_dir);

	igt_list_for_each_entry(metric_set, &perf->metric_sets, link) {
		if (metric_set->perf_oa_metrics_set)
			continue;

		load_metric_set_config(metric_set, drm_fd);
	}
}

static void
accumulate_uint32(const uint32_t *report0,
                  const uint32_t *report1,
                  uint64_t *deltas)
{
	*deltas += (uint32_t)(*report1 - *report0);
}

static void
accumulate_uint40(int a_index,
                  const uint32_t *report0,
                  const uint32_t *report1,
                  uint64_t *deltas)
{
	const uint8_t *high_bytes0 = (uint8_t *)(report0 + 40);
	const uint8_t *high_bytes1 = (uint8_t *)(report1 + 40);
	uint64_t high0 = (uint64_t)(high_bytes0[a_index]) << 32;
	uint64_t high1 = (uint64_t)(high_bytes1[a_index]) << 32;
	uint64_t value0 = report0[a_index + 4] | high0;
	uint64_t value1 = report1[a_index + 4] | high1;
	uint64_t delta;

	if (value0 > value1)
		delta = (1ULL << 40) + value1 - value0;
	else
		delta = value1 - value0;

	*deltas += delta;
}

void intel_perf_accumulate_reports(struct intel_perf_accumulator *acc,
				   int oa_format,
				   const struct drm_i915_perf_record_header *record0,
				   const struct drm_i915_perf_record_header *record1)
{
	const uint32_t *start = (const uint32_t *)(record0 + 1);
	const uint32_t *end = (const uint32_t *)(record1 + 1);
	uint64_t *deltas = acc->deltas;
	int idx = 0;
	int i;

	memset(acc, 0, sizeof(*acc));

	switch (oa_format) {
	case I915_OA_FORMAT_A32u40_A4u32_B8_C8:
		accumulate_uint32(start + 1, end + 1, deltas + idx++); /* timestamp */
		accumulate_uint32(start + 3, end + 3, deltas + idx++); /* clock */

		/* 32x 40bit A counters... */
		for (i = 0; i < 32; i++)
			accumulate_uint40(i, start, end, deltas + idx++);

		/* 4x 32bit A counters... */
		for (i = 0; i < 4; i++)
			accumulate_uint32(start + 36 + i, end + 36 + i, deltas + idx++);

		/* 8x 32bit B counters + 8x 32bit C counters... */
		for (i = 0; i < 16; i++)
			accumulate_uint32(start + 48 + i, end + 48 + i, deltas + idx++);
		break;

	case I915_OA_FORMAT_A45_B8_C8:
		accumulate_uint32(start + 1, end + 1, deltas); /* timestamp */

		for (i = 0; i < 61; i++)
			accumulate_uint32(start + 3 + i, end + 3 + i, deltas + 1 + i);
		break;
	default:
		assert(0);
	}

}
