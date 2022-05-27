/*
 * Copyright © 2022 Intel Corporation
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

#ifndef IGT_DRM_FDINFO_H
#define IGT_DRM_FDINFO_H

#include <sys/types.h>
#include <dirent.h>
#include <stdint.h>
#include <stdbool.h>

#define DRM_CLIENT_FDINFO_MAX_ENGINES 16

struct drm_client_fdinfo {
	char driver[128];
	char pdev[128];
	unsigned long id;

	unsigned int num_engines;
	unsigned int capacity[DRM_CLIENT_FDINFO_MAX_ENGINES];
	uint64_t busy[DRM_CLIENT_FDINFO_MAX_ENGINES];
};

/**
 * igt_parse_drm_fdinfo: Parses the drm fdinfo file
 *
 * @drm_fd: DRM file descriptor
 * @info: Structure to populate with read data. Must be zeroed.
 *
 * Returns the number of valid drm fdinfo keys found or zero if not all
 * mandatory keys were present or no engines found.
 */
unsigned int igt_parse_drm_fdinfo(int drm_fd, struct drm_client_fdinfo *info);

/**
 * __igt_parse_drm_fdinfo: Parses the drm fdinfo file
 *
 * @dir: File descriptor pointing to /proc/pid/fdinfo directory
 * @fd: String representation of the file descriptor number to parse.
 * @info: Structure to populate with read data. Must be zeroed.
 *
 * Returns the number of valid drm fdinfo keys found or zero if not all
 * mandatory keys were present or no engines found.
 */
unsigned int __igt_parse_drm_fdinfo(int dir, const char *fd,
				    struct drm_client_fdinfo *info);

#endif /* IGT_DRM_FDINFO_H */
