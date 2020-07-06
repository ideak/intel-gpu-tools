/*
 * Copyright © 2013 Intel Corporation
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
 *    Damien Lespiau <damien.lespiau@intel.com>
 *    Xiang, Haihao <haihao.xiang@intel.com>
 */

/*
 * This file is a basic test for the media_fill() function, a very simple
 * workload for the Media pipeline.
 */

#include <stdbool.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <inttypes.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/time.h>

#include "drm.h"
#include "i915/gem.h"
#include "igt.h"

IGT_TEST_DESCRIPTION("Basic test for the media_fill() function, a very simple"
		     " workload for the Media pipeline.");

#define WIDTH 64
#define STRIDE (WIDTH)
#define HEIGHT 64
#define SIZE (HEIGHT*STRIDE)

#define COLOR_C4	0xc4
#define COLOR_4C	0x4c

typedef struct {
	int drm_fd;
	uint32_t devid;
	struct buf_ops *bops;
} data_t;

static struct intel_buf *
create_buf(data_t *data, int width, int height, uint8_t color)
{
	struct intel_buf *buf;
	uint8_t *ptr;
	int i;

	buf = calloc(1, sizeof(*buf));
	igt_assert(buf);

	/*
	 * Legacy code uses 32 bpp after buffer creation.
	 * Let's do the same due to keep shader intact.
	 */
	intel_buf_init(data->bops, buf, width/4, height, 32, 0,
		       I915_TILING_NONE, 0);

	ptr = gem_mmap__cpu_coherent(data->drm_fd, buf->handle, 0,
				     buf->surface[0].size, PROT_WRITE);

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

static void media_fill(data_t *data, igt_fillfunc_t fill)
{
	struct intel_buf *buf;
	uint8_t *ptr;
	int i, j;

	buf = create_buf(data, WIDTH, HEIGHT, COLOR_C4);
	ptr = gem_mmap__device_coherent(data->drm_fd, buf->handle,
					0, buf->surface[0].size, PROT_READ);
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

igt_simple_main
{
	data_t data = {0, };
	igt_fillfunc_t fill_fn = NULL;

	data.drm_fd = drm_open_driver_render(DRIVER_INTEL);
	igt_require_gem(data.drm_fd);

	data.devid = intel_get_drm_devid(data.drm_fd);
	data.bops = buf_ops_create(data.drm_fd);

	fill_fn = igt_get_media_fillfunc(data.devid);

	igt_require_f(fill_fn, "no media-fill function\n");

	media_fill(&data, fill_fn);
}
