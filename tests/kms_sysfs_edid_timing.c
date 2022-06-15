/*
 * Copyright © 2016 Intel Corporation
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
#include "igt.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>

#define THRESHOLD_PER_CONNECTOR		150
#define THRESHOLD_PER_CONNECTOR_MEAN	140
#define THRESHOLD_ALL_CONNECTORS_MEAN	100
#define CHECK_TIMES			15

IGT_TEST_DESCRIPTION("This test checks the time it takes to reprobe each "
		     "connector and fails if either the time it takes for "
		     "one reprobe is too long or if the mean time it takes "
		     "to reprobe one connector is too long.  Additionally, "
		     "make sure that the mean time for all connectors is "
		     "not too long.");

igt_simple_main
{
	DIR *dirp;
	struct dirent *de;
	struct igt_mean all_mean;

	dirp = opendir("/sys/class/drm");
	igt_assert(dirp != NULL);

	igt_mean_init(&all_mean);

	while ((de = readdir(dirp))) {
		struct igt_mean mean = {};
		struct stat st;
		char path[PATH_MAX];
		int i;

		if (*de->d_name == '.')
			continue;;

		snprintf(path, sizeof(path), "/sys/class/drm/%s/status",
				de->d_name);

		if (stat(path, &st))
			continue;

		igt_mean_init(&mean);
		for (i = 0; i < CHECK_TIMES; i++) {
			struct timespec ts = {};
			int fd;

			if ((fd = open(path, O_WRONLY)) < 0)
				continue;

			igt_nsec_elapsed(&ts);
			igt_ignore_warn(write(fd, "detect\n", 7));
			igt_mean_add(&mean, igt_nsec_elapsed(&ts));

			close(fd);
		}

		igt_debug("%s: mean.max %.2fns, %.2fus, %.2fms, "
			  "mean.avg %.2fns, %.2fus, %.2fms\n",
			  de->d_name,
			  mean.max, mean.max / 1e3, mean.max / 1e6,
			  mean.mean, mean.mean / 1e3, mean.mean / 1e6);

		igt_assert_f(mean.max < THRESHOLD_PER_CONNECTOR * 1e6,
			     "%s: single probe time exceeded %dms, max=%.2fms, avg=%.2fms\n",
			     de->d_name, THRESHOLD_PER_CONNECTOR,
			     mean.max / 1e6, mean.mean / 1e6);

		igt_assert_f(mean.mean < (THRESHOLD_PER_CONNECTOR_MEAN * 1e6),
			     "%s: mean probe time exceeded %dms, max=%.2fms, avg=%.2fms\n",
			     de->d_name, THRESHOLD_PER_CONNECTOR_MEAN,
			     mean.max / 1e6, mean.mean / 1e6);

		igt_mean_add(&all_mean, mean.mean);
	}

	igt_assert_f(all_mean.mean < THRESHOLD_ALL_CONNECTORS_MEAN * 1e6,
		     "Mean of all connector means exceeds %dms, max=%.2fms, mean=%.2fms\n",
		     THRESHOLD_ALL_CONNECTORS_MEAN, all_mean.max / 1e6,
		     all_mean.mean / 1e6);

	closedir(dirp);
}
