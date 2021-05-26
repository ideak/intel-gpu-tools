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
 * This file is a basic test for the gpgpu_fill() function, a very simple
 * workload for the GPGPU pipeline.
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
#include "igt_collection.h"
#include "intel_bufops.h"
#include "i915/intel_memory_region.h"

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
create_buf(data_t *data, int width, int height, uint8_t color, uint32_t region)
{
	struct intel_buf *buf;
	uint8_t *ptr;
	uint32_t handle;
	int i;

	buf = calloc(1, sizeof(*buf));
	igt_assert(buf);

	/*
	 * Legacy code uses 32 bpp after buffer creation.
	 * Let's do the same due to keep shader intact.
	 */
	handle = gem_create_in_memory_regions(data->drm_fd, SIZE, region);
	intel_buf_init_using_handle(data->bops, handle, buf,
				    width/4, height, 32, 0,
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

static void gpgpu_fill(data_t *data, igt_fillfunc_t fill, uint32_t region)
{
	struct intel_buf *buf;
	uint8_t *ptr;
	int i, j;

	buf = create_buf(data, WIDTH, HEIGHT, COLOR_C4, region);
	ptr = gem_mmap__device_coherent(data->drm_fd, buf->handle, 0,
					buf->surface[0].size, PROT_READ);

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
	struct drm_i915_query_memory_regions *region_info;
	struct igt_collection *region_set;

	igt_fixture {
		data.drm_fd = drm_open_driver_render(DRIVER_INTEL);
		data.devid = intel_get_drm_devid(data.drm_fd);
		igt_require_gem(data.drm_fd);
		data.bops = buf_ops_create(data.drm_fd);

		fill_fn = igt_get_gpgpu_fillfunc(data.devid);

		igt_require_f(fill_fn, "no gpgpu-fill function\n");

		region_info = gem_get_query_memory_regions(data.drm_fd);
		igt_assert(region_info);

		region_set = get_memory_region_set(region_info,
						   I915_SYSTEM_MEMORY);
	}

	igt_subtest_with_dynamic("basic") {
		struct igt_collection *region;

		for_each_combination(region, 1, region_set) {
			char *name = memregion_dynamic_subtest_name(region);
			uint32_t id = igt_collection_get_value(region, 0);

			igt_dynamic(name)
				gpgpu_fill(&data, fill_fn, id);

			free(name);
		}
	}

	igt_fixture {
		igt_collection_destroy(region_set);
		free(region_info);
		buf_ops_destroy(data.bops);
	}
}
