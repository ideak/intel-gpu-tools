/*
 * Copyright © 2019 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "drmtest.h"
#include "i915/gem.h"
#include "i915/gem_engine_topology.h"
#include "igt_sysfs.h"

static bool may_write(int dir, const char *file)
{
	struct stat st;

	igt_assert(fstatat(dir, file, &st, 0) == 0);
	return st.st_mode & 0222;
}

static void test_writable(int i915, int engine)
{
	struct dirent *de;
	int defaults;
	DIR *dir;

	defaults = openat(engine, ".defaults", O_DIRECTORY);
	igt_require(defaults != -1);

	dir = fdopendir(engine);
	while ((de = readdir(dir))) {
		if (!(de->d_type & DT_REG))
			continue;

		if (!may_write(engine, de->d_name)) {
			igt_debug("Skipping constant attr '%s'\n", de->d_name);
			continue;
		}

		igt_debug("Checking attr '%s'\n", de->d_name);

		/* Every attribute should have a default value */
		igt_assert_f(faccessat(defaults, de->d_name, F_OK, 0) == 0,
			     "default value for %s not accessible\n",
			     de->d_name);

		/* But no one is allowed to change the default */
		igt_assert_f(!may_write(defaults, de->d_name),
			     "default value for %s writable!\n",
			     de->d_name);

		igt_assert_f(!igt_sysfs_set(defaults, de->d_name, "garbage"),
			     "write into default value of %s succeeded!\n",
			     de->d_name);
	}
	closedir(dir);
}

igt_main
{
	int i915 = -1, engines = -1;

	igt_fixture {
		int sys;

		i915 = drm_open_driver(DRIVER_INTEL);
		igt_require_gem(i915);
		igt_allow_hang(i915, 0, 0);

		sys = igt_sysfs_open(i915);
		igt_require(sys != -1);

		engines = openat(sys, "engine", O_RDONLY);
		igt_require(engines != -1);

		close(sys);
	}

	igt_subtest_with_dynamic("readonly")
		dyn_sysfs_engines(i915, engines, NULL, test_writable);

	igt_fixture {
		close(engines);
		close(i915);
	}
}
