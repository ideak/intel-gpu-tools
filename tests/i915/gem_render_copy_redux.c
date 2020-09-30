/*
 * Copyright © 2013-2014 Intel Corporation
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
 */

/*
 * This file is an "advanced" test for the render_copy() function, a very simple
 * workload for the 3D engine. The basic test in gem_render_copy.c is intentionally
 * kept extremely simple to allow for aub instrumentation and to ease debugging of
 * the render copy functions themselves. This test on the overhand aims to stress
 * the execbuffer interface with a simple render workload.
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

#include <drm.h>

#include "i915/gem.h"
#include "igt.h"

IGT_TEST_DESCRIPTION("Advanced test for the render_copy() function.");

#define WIDTH 512
#define STRIDE (WIDTH*4)
#define HEIGHT 512
#define SIZE (HEIGHT*STRIDE)

#define SRC_COLOR	0xffff00ff
#define DST_COLOR	0xfff0ff00

typedef struct {
	int fd;
	uint32_t devid;
	struct buf_ops *bops;
	struct intel_bb *ibb;
	igt_render_copyfunc_t render_copy;
	uint32_t linear[WIDTH * HEIGHT];
} data_t;

static void data_init(data_t *data)
{
	data->fd = drm_open_driver(DRIVER_INTEL);
	data->devid = intel_get_drm_devid(data->fd);

	data->bops = buf_ops_create(data->fd);
	data->render_copy = igt_get_render_copyfunc(data->devid);
	igt_require_f(data->render_copy,
		      "no render-copy function\n");

	data->ibb = intel_bb_create(data->fd, 4096);
}

static void data_fini(data_t *data)
{
	intel_bb_destroy(data->ibb);
	buf_ops_destroy(data->bops);
	close(data->fd);
}

static void scratch_buf_init(data_t *data, struct intel_buf *buf,
			     int width, int height, int stride, uint32_t color)
{
	int i;

	intel_buf_init(data->bops, buf, width, height, 32, 0,
		       I915_TILING_NONE, I915_COMPRESSION_NONE);
	igt_assert(buf->surface[0].size == SIZE);
	igt_assert(buf->surface[0].stride == stride);

	for (i = 0; i < width * height; i++)
		data->linear[i] = color;
	gem_write(data->fd, buf->handle, 0, data->linear,
		  sizeof(data->linear));
}

static void scratch_buf_fini(data_t *data, struct intel_buf *buf)
{
	intel_buf_close(data->bops, buf);
	memset(buf, 0, sizeof(*buf));
}

static void
scratch_buf_check(data_t *data, struct intel_buf *buf, int x, int y,
		  uint32_t color)
{
	uint32_t val;

	gem_read(data->fd, buf->handle, 0,
		 data->linear, sizeof(data->linear));
	val = data->linear[y * WIDTH + x];
	igt_assert_f(val == color,
		     "Expected 0x%08x, found 0x%08x at (%d,%d)\n",
		     color, val, x, y);
}

static void copy(data_t *data)
{
	struct intel_buf src, dst;

	scratch_buf_init(data, &src, WIDTH, HEIGHT, STRIDE, SRC_COLOR);
	scratch_buf_init(data, &dst, WIDTH, HEIGHT, STRIDE, DST_COLOR);

	scratch_buf_check(data, &src, WIDTH / 2, HEIGHT / 2, SRC_COLOR);
	scratch_buf_check(data, &dst, WIDTH / 2, HEIGHT / 2, DST_COLOR);

	data->render_copy(data->ibb,
			  &src, 0, 0, WIDTH, HEIGHT,
			  &dst, WIDTH / 2, HEIGHT / 2);

	scratch_buf_check(data, &dst, 10, 10, DST_COLOR);
	scratch_buf_check(data, &dst, WIDTH - 10, HEIGHT - 10, SRC_COLOR);

	scratch_buf_fini(data, &src);
	scratch_buf_fini(data, &dst);
}

static void copy_flink(data_t *data)
{
	data_t local;
	struct intel_buf src, dst;
	struct intel_buf local_src, local_dst;
	struct intel_buf flink;
	uint32_t name;

	data_init(&local);

	scratch_buf_init(data, &src, WIDTH, HEIGHT, STRIDE, 0);
	scratch_buf_init(data, &dst, WIDTH, HEIGHT, STRIDE, DST_COLOR);

	data->render_copy(data->ibb,
			  &src, 0, 0, WIDTH, HEIGHT,
			  &dst, WIDTH, HEIGHT);

	scratch_buf_init(&local, &local_src, WIDTH, HEIGHT, STRIDE, 0);
	scratch_buf_init(&local, &local_dst, WIDTH, HEIGHT, STRIDE, SRC_COLOR);

	local.render_copy(local.ibb,
			  &local_src, 0, 0, WIDTH, HEIGHT,
			  &local_dst, WIDTH, HEIGHT);

	name = gem_flink(local.fd, local_dst.handle);
	flink = local_dst;
	flink.handle = gem_open(data->fd, name);

	data->render_copy(data->ibb,
			  &flink, 0, 0, WIDTH, HEIGHT,
			  &dst, WIDTH / 2, HEIGHT / 2);

	scratch_buf_check(data, &dst, 10, 10, DST_COLOR);
	scratch_buf_check(data, &dst, WIDTH - 10, HEIGHT - 10, SRC_COLOR);

	scratch_buf_check(data, &dst, 10, 10, DST_COLOR);
	scratch_buf_check(data, &dst, WIDTH - 10, HEIGHT - 10, SRC_COLOR);

	intel_bb_reset(data->ibb, true);
	scratch_buf_fini(data, &src);
	scratch_buf_fini(data, &flink);
	scratch_buf_fini(data, &dst);

	scratch_buf_fini(&local, &local_src);
	scratch_buf_fini(&local, &local_dst);

	data_fini(&local);
}

igt_main
{
	data_t data = {0, };

	igt_fixture {
		data_init(&data);
		igt_require_gem(data.fd);
	}

	igt_subtest("normal") {
		int loop = 100;
		while (loop--)
			copy(&data);
	}

	igt_subtest("interruptible") {
		int loop = 100;
		igt_fork_signal_helper();
		while (loop--)
			copy(&data);
		igt_stop_signal_helper();
	}

	igt_subtest("flink") {
		int loop = 100;
		while (loop--)
			copy_flink(&data);
	}

	igt_subtest("flink-interruptible") {
		int loop = 100;
		igt_fork_signal_helper();
		while (loop--)
			copy_flink(&data);
		igt_stop_signal_helper();
	}
}
