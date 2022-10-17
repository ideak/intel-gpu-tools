// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <dirent.h>
#include <fcntl.h>
#include <inttypes.h>
#include <unistd.h>

#include "drmtest.h"
#include "igt_core.h"
#include "igt_hwmon.h"
#include "igt_sysfs.h"

static char *igt_hwmon_path(int device, char *path, const char *name)
{
	char buf[80];
	int path_offset;
	struct dirent *entry;
	struct stat st;
	DIR *dir;

	if (igt_debug_on(device < 0))
		return NULL;

	if (igt_debug_on(fstat(device, &st)) || igt_debug_on(!S_ISCHR(st.st_mode)))
		return NULL;

	path_offset = snprintf(path, PATH_MAX, "/sys/dev/char/%d:%d/device/hwmon",
			       major(st.st_rdev), minor(st.st_rdev));

	dir = opendir(path);
	if (!dir)
		return NULL;

	while ((entry = readdir(dir))) {
		if (entry->d_name[0] == '.')
			continue;

		snprintf(path + path_offset, PATH_MAX - path_offset, "/%s/name", entry->d_name);
		igt_sysfs_scanf(dirfd(dir), path, "%s", buf);

		if (strncmp(buf, name, strlen(name)) == 0) {
			snprintf(path + path_offset, PATH_MAX - path_offset, "/%s", entry->d_name);
			closedir(dir);
			return path;
		}
	}

	closedir(dir);
	return NULL;
}

/**
 * igt_hwmon_open:
 * @device: fd of the device
 *
 * Opens the hwmon directory corresponding to device
 *
 * Returns:
 * The directory fd, or -1 on failure.
 */
int igt_hwmon_open(int device)
{
	char path[PATH_MAX];

	if (!is_i915_device(device) || !igt_hwmon_path(device, path, "i915"))
		return -1;

	return open(path, O_RDONLY);
}

