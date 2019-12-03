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
 *
 */

#include "config.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>

#include "igt.h"
#include "igt_kms.h"

IGT_TEST_DESCRIPTION("Test that /dev/drm_dp_aux reads work");

static bool test(int drm_fd, uint32_t connector_id)
{
	drmModeConnector *connector;
	DIR *dir;
	int dir_fd;

	connector = drmModeGetConnectorCurrent(drm_fd, connector_id);
	dir_fd = igt_connector_sysfs_open(drm_fd, connector);
	drmModeFreeConnector(connector);
	igt_assert(dir_fd >= 0);

	dir = fdopendir(dir_fd);
	igt_assert(dir);

	for (;;) {
		struct dirent *ent;
		char path[5 + sizeof(ent->d_name)];
		uint8_t buf[16];
		int fd, ret;

		ent = readdir(dir);
		if (!ent)
			break;

		if (strncmp(ent->d_name, "drm_dp_aux", 10))
			continue;

		snprintf(path, sizeof(path), "/dev/%s", ent->d_name);

		fd = open(path, O_RDONLY);
		igt_assert(fd >= 0);

		ret = read(fd, buf, sizeof(buf));
		igt_assert(ret == sizeof(buf) || errno == ETIMEDOUT);

		igt_info("%s: %s\n", path,
			 ret > 0 ? "success" : "timed out");

		close(fd);

		closedir(dir);
		close(dir_fd);

		if (ret > 0) {
			/* DPCD rev sanity check */
			igt_assert_f(buf[0] == 0x10 ||
				     buf[0] == 0x11 ||
				     buf[0] == 0x12 ||
				     buf[0] == 0x13 ||
				     buf[0] == 0x14,
				     "Read bogus DPCD rev 0x%02x\n",
				     buf[0]);
			/* DPCD max lane count sanity check */
			igt_assert_f((buf[2] & 0x1f) == 0x01 ||
				     (buf[2] & 0x1f) == 0x02 ||
				     (buf[2] & 0x1f) == 0x04,
				     "Read bogus DPCD max lane count 0x%02x\n",
				     buf[2] & 0x1f);
		}

		return ret > 0;
	}

	closedir(dir);
	close(dir_fd);
	return false;
}

igt_simple_main
{
	int valid_connectors = 0;
	drmModeRes *res;
	int drm_fd;

	drm_fd = drm_open_driver_master(DRIVER_ANY);

	res = drmModeGetResources(drm_fd);
	igt_require(res);

	for (int i = 0; i < res->count_connectors; i++)
		valid_connectors += test(drm_fd, res->connectors[i]);
	igt_require(valid_connectors);

	drmModeFreeResources(res);
}
