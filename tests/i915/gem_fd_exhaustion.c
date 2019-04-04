/*
 * Copyright © 2014 Intel Corporation
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
 *
 * Authors:
 *    Daniel Vetter <daniel.vetter@ffwll.ch>
 *
 */

#include "igt.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <fcntl.h>
#include <limits.h>

unsigned int original_nr_open;

static int read_sysctl(const char *path)
{
	unsigned int val;
	FILE *f = fopen(path, "r");

	if (f) {
		igt_assert(fscanf(f, "%u", &val) == 1);
		fclose(f);
		return val;
	}
	return -errno;
}

static int write_sysctl(const char *path, unsigned int val)
{
	FILE *f = fopen(path, "w");

	if (f) {
		igt_assert(fprintf(f, "%u", val));
		fclose(f);
		return 0;
	}
	return -errno;
}

static void restore_original_sysctl(int sig)
{
	if (original_nr_open > 0)
		write_sysctl("/proc/sys/fs/nr_open", original_nr_open);
}

igt_simple_main
{
	int fd;

	igt_require(igt_allow_unlimited_files());

	fd = drm_open_driver(DRIVER_INTEL);

	igt_install_exit_handler(restore_original_sysctl);

	igt_fork(n, 1) {
		igt_drop_root();

		for (int i = 0; ; i++) {
			int leak = open("/dev/null", O_RDONLY);
			uint32_t handle;

			if (__gem_create(fd, 4096, &handle) == 0)
				gem_close(fd, handle);

			if (leak < 0) {
				igt_info("fd exhaustion after %i rounds.\n", i);
				igt_assert(__gem_create(fd, 4096,
							&handle) < 0);
				break;
			}
		}

		/* The child will free all the fds when exiting, so no need to
		 * clean up to mess to ensure that the parent can at least run
		 * the exit handlers. */
	}

	igt_waitchildren();

	close(fd);
}
