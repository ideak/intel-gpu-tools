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
#include "i915_perf_metrics.h"

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
			if (!subslice_available(topology, 0, s))
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
			assert(0); /* unreachable */
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
			assert(0); /* unreachable */
		}
	} else if (devinfo->is_geminilake) {
		perf->devinfo.eu_threads_count = 6;
		intel_perf_load_metrics_glk(perf);
	} else if (devinfo->is_coffeelake) {
		switch (devinfo->gt) {
		case 2:
			intel_perf_load_metrics_cflgt2(perf);
			break;
		case 3:
			intel_perf_load_metrics_cflgt3(perf);
			break;
		default:
			assert(0); /* unreachable */
		}
	} else if (devinfo->is_cannonlake) {
		intel_perf_load_metrics_cnl(perf);
	} else if (devinfo->is_icelake) {
		intel_perf_load_metrics_icl(perf);
	} else if (devinfo->is_elkhartlake) {
		intel_perf_load_metrics_ehl(perf);
	} else if (devinfo->is_tigerlake) {
		intel_perf_load_metrics_tgl(perf);
	} else {
		assert(0); /* unreachable */
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

	while (ioctl(drm_fd, DRM_IOCTL_I915_GETPARAM, &gp) < 0 &&
	       (errno == EAGAIN || errno == EINTR));

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
	int ret;

	while ((ret = ioctl(drm_fd, DRM_IOCTL_I915_QUERY, &q)) < 0 &&
	       (errno == EAGAIN || errno == EINTR));
	return ret;
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
