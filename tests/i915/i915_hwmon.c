// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include <dirent.h>
#include <sys/stat.h>
#include "igt.h"
#include "igt_hwmon.h"
#include "igt_sysfs.h"
/**
 * TEST: i915 hwmon
 * Description: Tests for i915 hwmon
 * Feature: hwmon
 * Run type: FULL
 *
 * SUBTEST: hwmon-read
 * Description: Verify we can read all hwmon attributes
 *
 * SUBTEST: hwmon-write
 * Description: Verify writable hwmon attributes
 */

IGT_TEST_DESCRIPTION("Tests for i915 hwmon");

static void hwmon_read(int hwm)
{
	struct dirent *de;
	char val[128];
	DIR *dir;

	dir = fdopendir(dup(hwm));
	igt_assert(dir);
	rewinddir(dir);

	while ((de = readdir(dir))) {
		if (de->d_type != DT_REG || !strcmp(de->d_name, "uevent"))
			continue;

		igt_assert(igt_sysfs_scanf(hwm, de->d_name, "%127s", val) == 1);
		igt_debug("'%s': %s\n", de->d_name, val);

	}
	closedir(dir);
}

static void hwmon_write(int hwm)
{
	igt_sysfs_rw_attr_t rw;
	struct dirent *de;
	struct stat st;
	DIR *dir;

	dir = fdopendir(dup(hwm));
	igt_assert(dir);
	rewinddir(dir);

	rw.dir = hwm;
	rw.start = 1;
	rw.tol = 0.1;

	while ((de = readdir(dir))) {
		if (de->d_type != DT_REG || !strcmp(de->d_name, "uevent"))
			continue;

		igt_assert(!fstatat(hwm, de->d_name, &st, 0));
		if (!(st.st_mode & 0222))
			continue;

		rw.attr = de->d_name;
		igt_sysfs_rw_attr_verify(&rw);
	}
	closedir(dir);
}

igt_main
{
	int fd, hwm;

	igt_fixture {
		fd = drm_open_driver_master(DRIVER_INTEL);
		hwm = igt_hwmon_open(fd);
		igt_require(hwm >= 0);
	}

	igt_describe("Verify we can read all hwmon attributes");
	igt_subtest("hwmon-read") {
		hwmon_read(hwm);
	}

	igt_describe("Verify writable hwmon attributes");
	igt_subtest("hwmon-write") {
		hwmon_write(hwm);
	}

	igt_fixture {
		close(hwm);
		close(fd);
	}
}
