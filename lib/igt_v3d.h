/*
 * Copyright © 2016 Broadcom
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

#ifndef IGT_V3D_H
#define IGT_V3D_H

#include "v3d_drm.h"

#define PAGE_SIZE 4096

struct v3d_bo {
	int handle;
	uint32_t offset;
	uint32_t size;
	void *map;
};

struct v3d_bo *igt_v3d_create_bo(int fd, size_t size);
void igt_v3d_free_bo(int fd, struct v3d_bo *bo);

/* IOCTL wrappers */
uint32_t igt_v3d_get_bo_offset(int fd, uint32_t handle);
uint32_t igt_v3d_get_param(int fd, enum drm_v3d_param param);
void *igt_v3d_mmap_bo(int fd, uint32_t handle, uint32_t size, unsigned prot);

void igt_v3d_bo_mmap(int fd, struct v3d_bo *bo);

uint32_t igt_v3d_perfmon_create(int fd, uint32_t ncounters, uint8_t *counters);
void igt_v3d_perfmon_get_values(int fd, uint32_t id);
void igt_v3d_perfmon_destroy(int fd, uint32_t id);

#endif /* IGT_V3D_H */
