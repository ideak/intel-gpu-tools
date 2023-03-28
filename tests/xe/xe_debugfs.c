// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023 Intel Corporation
 */

/**
 * TEST: Check debugfs userspace API
 * Category: Software building block
 * Sub-category: debugfs
 * Test category: functionality test
 * Run type: BAT
 * Description: Validate debugfs entries
 */

#include "igt.h"

#include "xe_drm.h"
#include "xe/xe_ioctl.h"
#include "xe/xe_query.h"

#include <fcntl.h>
#include <string.h>
#include <sys/types.h>
#include <dirent.h>

struct {
	bool warn_on_not_hit;
} opt = { 0 };

static int validate_entries(int fd, const char *add_path, const char * const str_val[], int str_cnt)
{
	int i;
	int hit;
	int found = 0;
	int not_found = 0;
	DIR *dir;
	struct dirent *de;
	char path[PATH_MAX];

	if (!igt_debugfs_path(fd, path, sizeof(path)))
		return -1;

	strcat(path, add_path);
	dir = opendir(path);
	if (!dir)
		return -1;

	while ((de = readdir(dir))) {
		if (de->d_name[0] == '.')
			continue;
		hit = 0;
		for (i = 0; i < str_cnt; i++) {
			if (!strcmp(str_val[i], de->d_name)) {
				hit = 1;
				break;
			}
		}
		if (hit) {
			found++;
		} else if (opt.warn_on_not_hit) {
			not_found++;
			igt_warn("no test for: %s/%s\n", path, de->d_name);
		}
	}
	closedir(dir);
	return 0;
}

/**
 * SUBTEST: base
 * Description: Check if various debugfs devnodes exist and test reading them.
 */
static void
test_base(int fd, struct drm_xe_query_config *config)
{
	static const char * const expected_files[] = {
		"gt0",
		"gt1",
		"stolen_mm",
		"gtt_mm",
		"vram0_mm",
		"forcewake_all",
		"info",
		"gem_names",
		"clients",
		"name"
	};

	char reference[4096];
	int val = 0;

	igt_assert(config);
	sprintf(reference, "devid 0x%llx",
			config->info[XE_QUERY_CONFIG_REV_AND_DEVICE_ID] & 0xffff);
	igt_assert(igt_debugfs_search(fd, "info", reference));

	sprintf(reference, "revid %lld",
			config->info[XE_QUERY_CONFIG_REV_AND_DEVICE_ID] >> 16);
	igt_assert(igt_debugfs_search(fd, "info", reference));

	sprintf(reference, "is_dgfx %s", config->info[XE_QUERY_CONFIG_FLAGS] &
		XE_QUERY_CONFIG_FLAGS_HAS_VRAM ? "yes" : "no");

	igt_assert(igt_debugfs_search(fd, "info", reference));

	sprintf(reference, "enable_guc %s", config->info[XE_QUERY_CONFIG_FLAGS] &
		XE_QUERY_CONFIG_FLAGS_USE_GUC ? "yes" : "no");
	igt_assert(igt_debugfs_search(fd, "info", reference));

	sprintf(reference, "tile_count %lld", config->info[XE_QUERY_CONFIG_GT_COUNT]);
	igt_assert(igt_debugfs_search(fd, "info", reference));

	switch (config->info[XE_QUERY_CONFIG_VA_BITS]) {
	case 48:
		val = 3;
		break;
	case 57:
		val = 4;
		break;
	}
	sprintf(reference, "vm_max_level %d", val);
	igt_assert(igt_debugfs_search(fd, "info", reference));

	igt_assert(igt_debugfs_exists(fd, "gt0", O_RDONLY));
	if (config->info[XE_QUERY_CONFIG_GT_COUNT] > 1)
		igt_assert(igt_debugfs_exists(fd, "gt1", O_RDONLY));

	igt_assert(igt_debugfs_exists(fd, "gtt_mm", O_RDONLY));
	igt_debugfs_dump(fd, "gtt_mm");

	if (config->info[XE_QUERY_CONFIG_FLAGS] & XE_QUERY_CONFIG_FLAGS_HAS_VRAM) {
		igt_assert(igt_debugfs_exists(fd, "vram0_mm", O_RDONLY));
		igt_debugfs_dump(fd, "vram0_mm");
	}

	if (igt_debugfs_exists(fd, "stolen_mm", O_RDONLY))
		igt_debugfs_dump(fd, "stolen_mm");

	igt_assert(igt_debugfs_exists(fd, "clients", O_RDONLY));
	igt_debugfs_dump(fd, "clients");

	igt_assert(igt_debugfs_exists(fd, "gem_names", O_RDONLY));
	igt_debugfs_dump(fd, "gem_names");

	validate_entries(fd, "", expected_files, ARRAY_SIZE(expected_files));
}

/**
 * SUBTEST: gt
 * Description: Check all gt debugfs devnodes
 * TODO: add support for ``force_reset`` entries
 */
static void
test_gt(int fd, int gt_id)
{
	char name[256];
	static const char * const expected_files[] = {
		"uc",
		"steering",
		"topology",
		"sa_info",
		"hw_engines",
//		"force_reset"
	};
	static const char * const expected_files_uc[] = {
		"huc_info",
		"guc_log",
		"guc_info",
//		"guc_ct_selftest"
	};

	sprintf(name, "gt%d/hw_engines", gt_id);
	igt_assert(igt_debugfs_exists(fd, name, O_RDONLY));
	igt_debugfs_dump(fd, name);

	sprintf(name, "gt%d/sa_info", gt_id);
	igt_assert(igt_debugfs_exists(fd, name, O_RDONLY));
	igt_debugfs_dump(fd, name);

	sprintf(name, "gt%d/steering", gt_id);
	igt_assert(igt_debugfs_exists(fd, name, O_RDONLY));
	igt_debugfs_dump(fd, name);

	sprintf(name, "gt%d/topology", gt_id);
	igt_assert(igt_debugfs_exists(fd, name, O_RDONLY));
	igt_debugfs_dump(fd, name);

	sprintf(name, "gt%d/uc/guc_info", gt_id);
	igt_assert(igt_debugfs_exists(fd, name, O_RDONLY));
	igt_debugfs_dump(fd, name);

	sprintf(name, "gt%d/uc/huc_info", gt_id);
	igt_assert(igt_debugfs_exists(fd, name, O_RDONLY));
	igt_debugfs_dump(fd, name);

	sprintf(name, "gt%d/uc/guc_log", gt_id);
	igt_assert(igt_debugfs_exists(fd, name, O_RDONLY));
	igt_debugfs_dump(fd, name);

	sprintf(name, "/gt%d", gt_id);
	validate_entries(fd, name, expected_files, ARRAY_SIZE(expected_files));

	sprintf(name, "/gt%d/uc", gt_id);
	validate_entries(fd, name, expected_files_uc, ARRAY_SIZE(expected_files_uc));
}

/**
 * SUBTEST: forcewake
 * Description: check forcewake debugfs devnode
 */
static void
test_forcewake(int fd)
{
	int handle = igt_debugfs_open(fd, "forcewake_all", O_WRONLY);

	igt_assert(handle != -1);
	close(handle);
}

const char *help_str =
	"  -w\t--warn-not-hit Produce warnings if it founds a devfs node without tests";

struct option long_options[] = {
	{ "--warn-not-hit", no_argument, NULL, 'w'},
	{ 0, 0, 0, 0 }
};

static int opt_handler(int option, int option_index, void *input)
{
	switch (option) {
	case 'w':
		opt.warn_on_not_hit = true;
		break;
	default:
		return IGT_OPT_HANDLER_ERROR;
	}

	return IGT_OPT_HANDLER_SUCCESS;
}

igt_main_args("", long_options, help_str, opt_handler, NULL)
{
	struct xe_device *xe_dev;
	char devnode[PATH_MAX];
	int fd;
	int gt;

	igt_fixture {
		fd = drm_open_driver(DRIVER_XE);
		xe_dev = xe_device_get(fd);
		__igt_debugfs_dump(fd, "info", IGT_LOG_INFO);
	}

	igt_subtest("base") {
		test_base(fd, xe_dev->config);
	}


	igt_subtest("gt") {
		xe_for_each_gt(fd, gt) {
			snprintf(devnode, sizeof(devnode), "gt%d", gt);
			igt_require(igt_debugfs_exists(fd, devnode, O_RDONLY));
			test_gt(fd, gt);
		}
	}

	igt_subtest("forcewake") {
		test_forcewake(fd);
	}

	igt_fixture {
		xe_device_put(fd);
		close(fd);
	}
}
