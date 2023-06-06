// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023 Intel Corporation
 */

/**
 * TEST: Basic tests for gpgpu functionality
 * Category: Software building block
 * Sub-category: gpgpu
 * Functionality: gpgpu
 * Test category: functionality test
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "drm.h"
#include "i915/gem.h"
#include "igt.h"
#include "igt_collection.h"
#include "intel_bufops.h"
#include "xe/xe_ioctl.h"
#include "xe/xe_query.h"

#define WIDTH 64
#define HEIGHT 64
#define STRIDE (WIDTH)
#define SIZE (HEIGHT*STRIDE)
#define COLOR_C4	0xc4
#define COLOR_4C	0x4c

typedef struct {
	int drm_fd;
	uint32_t devid;
	struct buf_ops *bops;
} data_t;

static struct intel_buf *
create_buf(data_t *data, int width, int height, uint8_t color, uint64_t region)
{
	struct intel_buf *buf;
	uint8_t *ptr;
	int i;

	buf = calloc(1, sizeof(*buf));
	igt_assert(buf);

	buf = intel_buf_create(data->bops, width/4, height, 32, 0,
			       I915_TILING_NONE, 0);

	ptr = xe_bo_map(data->drm_fd, buf->handle, buf->surface[0].size);

	for (i = 0; i < buf->surface[0].size; i++)
		ptr[i] = color;

	munmap(ptr, buf->surface[0].size);

	return buf;
}

static void buf_check(uint8_t *ptr, int x, int y, uint8_t color)
{
	uint8_t val;

	val = ptr[y * WIDTH + x];
	igt_assert_f(val == color,
		     "Expected 0x%02x, found 0x%02x at (%d,%d)\n",
		     color, val, x, y);
}

/**
 * SUBTEST: basic
 * Description: run gpgpu fill
 * Run type: FULL
 * TODO: change ``'Run type' == FULL`` to a better category
 */

static void gpgpu_fill(data_t *data, igt_fillfunc_t fill, uint32_t region)
{
	struct intel_buf *buf;
	uint8_t *ptr;
	int i, j;

	buf = create_buf(data, WIDTH, HEIGHT, COLOR_C4, region);
	ptr = xe_bo_map(data->drm_fd, buf->handle, buf->surface[0].size);

	for (i = 0; i < WIDTH; i++)
		for (j = 0; j < HEIGHT; j++)
			buf_check(ptr, i, j, COLOR_C4);

	fill(data->drm_fd, buf, 0, 0, WIDTH / 2, HEIGHT / 2, COLOR_4C);

	for (i = 0; i < WIDTH; i++)
		for (j = 0; j < HEIGHT; j++)
			if (i < WIDTH / 2 && j < HEIGHT / 2)
				buf_check(ptr, i, j, COLOR_4C);
			else
				buf_check(ptr, i, j, COLOR_C4);

	munmap(ptr, buf->surface[0].size);
}

igt_main
{
	data_t data = {0, };
	igt_fillfunc_t fill_fn = NULL;

	igt_fixture {
		data.drm_fd = drm_open_driver_render(DRIVER_XE);
		data.devid = intel_get_drm_devid(data.drm_fd);
		data.bops = buf_ops_create(data.drm_fd);

		fill_fn = igt_get_gpgpu_fillfunc(data.devid);
		igt_require_f(fill_fn, "no gpgpu-fill function\n");

		xe_device_get(data.drm_fd);
	}

	igt_subtest("basic") {
		gpgpu_fill(&data, fill_fn, 0);
	}

	igt_fixture {
		xe_device_put(data.drm_fd);
		buf_ops_destroy(data.bops);
	}
}
