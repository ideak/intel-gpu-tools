/*
 * Copyright © 2017 Intel Corporation
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
#include "config.h"
#include "igt.h"
#include "igt_sysfs.h"
#include <fcntl.h>
#include <sys/types.h>
#include <dirent.h>

static void read_and_discard_sysfs_entries(int path_fd)
{
	struct dirent *dirent;
	DIR *dir;

	dir = fdopendir(path_fd);
	if (!dir)
		return;

	while ((dirent = readdir(dir))) {
		if (!strcmp(dirent->d_name, ".") ||
		    !strcmp(dirent->d_name, ".."))
			continue;
		if (dirent->d_type == DT_DIR) {
			int sub_fd = -1;
			igt_assert((sub_fd =
				    openat(path_fd, dirent->d_name, O_RDONLY |
					   O_DIRECTORY)) > 0);
			read_and_discard_sysfs_entries(sub_fd);
			close(sub_fd);
		} else {
			char buf[512];
			int sub_fd;
			ssize_t ret;

			igt_set_timeout(5, "reading sysfs entry");
			igt_debug("Reading file \"%s\"\n", dirent->d_name);

			sub_fd = openat(path_fd, dirent->d_name, O_RDONLY);
			if (sub_fd == -1) {
				igt_debug("Could not open file \"%s\" with error: %m\n", dirent->d_name);
				continue;
			}

			do {
				ret = read(sub_fd, buf, sizeof(buf));
			} while (ret == sizeof(buf));

			if (ret == -1)
				igt_debug("Could not read file \"%s\" with error: %m\n", dirent->d_name);

			igt_reset_timeout();
			close(sub_fd);
		}
	}
	closedir(dir);
}

igt_main
{
	int fd = -1, debugfs;
	igt_skip_on_simulation();

	igt_fixture {
		fd = drm_open_driver_master(DRIVER_INTEL);
		igt_require_gem(fd);
		debugfs = igt_debugfs_dir(fd);
	}

	igt_subtest("read_all_entries") {
		read_and_discard_sysfs_entries(debugfs);
	}

	igt_subtest("emon_crash") {
		int i;
		/*
		 * This check if we can crash the kernel with
		 * segmentation-fault by reading
		 * /sys/kernel/debug/dri/0/i915_emon_status too quickly
		 */
		for (i = 0; i < 1000; i++) {
			char *buf = igt_sysfs_get(debugfs,
						  "i915_emon_status");

			igt_skip_on_f(!buf && !i, "i915_emon_status could not be read\n");

			igt_assert(buf);
			free(buf);
		}

		/* If we got here, we haven't crashed */
		igt_success();
	}

	igt_fixture {
		close(debugfs);
		close(fd);
	}
}
