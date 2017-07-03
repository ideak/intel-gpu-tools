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
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "igt.h"
#include "igt_sysfs.h"
#include <fcntl.h>
#include <sys/types.h>
#include <dirent.h>

static void read_and_discard_sysfs_entries(int path_fd, bool is_crc)
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
			if (strstr(dirent->d_name, "crtc-"))
				continue;
			igt_assert((sub_fd =
				    openat(path_fd, dirent->d_name, O_RDONLY |
					   O_DIRECTORY)) > 0);
			read_and_discard_sysfs_entries(sub_fd, !strcmp(dirent->d_name, "crc"));
			close(sub_fd);
		} else {
			char *buf = igt_sysfs_get(path_fd, dirent->d_name);

			/*
			 * /crtc-XX/crc/data may fail with -EIO if the CRTC
			 * is not active.
			 */
			if (!buf && is_crc && errno == EIO &&
			    !strcmp(dirent->d_name, "data"))
				continue;

			igt_assert(buf);
			free(buf);
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
		read_and_discard_sysfs_entries(debugfs, false);
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
