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
 */

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "i915/gem.h"
#include "igt.h"

IGT_TEST_DESCRIPTION("Exercises full ppgtt fence pin_count leak in the "
		     "kernel.");

typedef struct {
	int drm_fd;
	uint32_t devid;
	struct buf_ops *bops;
	igt_display_t display;
	struct intel_buf *bos[64]; /* >= num fence registers */
} data_t;

static void exec_nop(data_t *data, struct igt_fb *fb, uint32_t ctx)
{
	struct intel_buf *dst;
	struct intel_bb *ibb;
	uint32_t name, handle, tiling, stride, width, height, bpp, size;

	tiling = igt_fb_mod_to_tiling(fb->modifier);
	stride = fb->strides[0];
	bpp = fb->plane_bpp[0];
	size = fb->size;
	width = stride / (bpp / 8);
	height = size / stride;

	name = gem_flink(data->drm_fd, fb->gem_handle);
	handle = gem_open(data->drm_fd, name);
	dst = intel_buf_create_using_handle(data->bops, handle,
					    width, height, bpp, 0, tiling, 0);
	intel_buf_set_ownership(dst, true);

	ibb = intel_bb_create_with_context(buf_ops_get_fd(data->bops),
					   ctx, 4096);

	/* add the reloc to make sure the kernel will think we write to dst */
	intel_bb_add_intel_buf(ibb, dst, true);
	intel_bb_out(ibb, MI_BATCH_BUFFER_END);
	intel_bb_out(ibb, MI_NOOP);
	intel_bb_emit_reloc(ibb, dst->handle, I915_GEM_DOMAIN_RENDER,
			    I915_GEM_DOMAIN_RENDER, 0, 0x0);
	intel_bb_out(ibb, MI_NOOP);

	intel_bb_flush_render(ibb);

	intel_bb_destroy(ibb);

	intel_buf_destroy(dst);
}

static void alloc_fence_objs(data_t *data)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(data->bos); i++) {
		struct intel_buf *buf;

		buf = intel_buf_create(data->bops, 128, 8, 32, 0,
				       I915_TILING_X, I915_COMPRESSION_NONE);
		igt_assert(buf->surface[0].stride == 512);
		data->bos[i] = buf;
	}
}

static void touch_fences(data_t *data)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(data->bos); i++) {
		uint32_t handle = data->bos[i]->handle;
		void *ptr;

		ptr = gem_mmap__gtt(data->drm_fd, handle, 4096, PROT_WRITE);
		gem_set_domain(data->drm_fd, handle, I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);
		memset(ptr, 0, 4);
		munmap(ptr, 4096);
	}
}

static void free_fence_objs(data_t *data)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(data->bos); i++)
		intel_buf_destroy(data->bos[i]);
}

static void run_single_test(data_t *data, enum pipe pipe, igt_output_t *output)
{
	igt_display_t *display = &data->display;
	drmModeModeInfo *mode;
	igt_plane_t *primary;
	struct igt_fb fb[2];
	int i;

	igt_output_set_pipe(output, pipe);

	mode = igt_output_get_mode(output);
	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);

	igt_create_color_fb(data->drm_fd, mode->hdisplay, mode->vdisplay,
			    DRM_FORMAT_XRGB8888,
			    I915_FORMAT_MOD_X_TILED , /* need a fence so must be tiled */
			    0.0, 0.0, 0.0,
			    &fb[0]);
	igt_create_color_fb(data->drm_fd, mode->hdisplay, mode->vdisplay,
			    DRM_FORMAT_XRGB8888,
			    I915_FORMAT_MOD_X_TILED, /* need a fence so must be tiled */
			    0.0, 0.0, 0.0,
			    &fb[1]);

	igt_plane_set_fb(primary, &fb[0]);
	igt_display_commit(display);

	for (i = 0; i < 64; i++) {
		uint32_t ctx;

		/*
		 * Link fb.gem_handle to the ppgtt vm of ctx so that the context
		 * destruction will unbind the obj from the ppgtt vm in question.
		 */
		ctx = gem_context_create(data->drm_fd);
		exec_nop(data, &fb[i&1], ctx);
		gem_context_destroy(data->drm_fd, ctx);

		/* Force a context switch to make sure ctx gets destroyed for real. */
		exec_nop(data, &fb[i&1], 0);

		gem_sync(data->drm_fd, fb[i&1].gem_handle);

		/*
		 * Make only the current fb has a fence and
		 * the next fb will pick a new fence. Assuming
		 * all fences are associated with an object, the
		 * kernel will always pick a fence with pin_count==0.
		 */
		touch_fences(data);

		/*
		 * Pin the new buffer and unpin the old buffer from display. If
		 * the kernel is buggy the ppgtt unbind will have dropped the
		 * fence for the old buffer, and now the display code will try
		 * to unpin only to find no fence there. So the pin_count will leak.
		 */
		igt_plane_set_fb(primary, &fb[!(i&1)]);
		igt_display_commit(display);

		igt_print_activity();
	}

	igt_plane_set_fb(primary, NULL);
	igt_output_set_pipe(output, PIPE_ANY);
	igt_display_commit(display);

	igt_remove_fb(data->drm_fd, &fb[1]);
	igt_remove_fb(data->drm_fd, &fb[0]);

	igt_info("\n");
}

static void run_test(data_t *data)
{
	igt_display_t *display = &data->display;
	igt_output_t *output;
	enum pipe p;

	for_each_pipe_with_valid_output(display, p, output) {
		run_single_test(data, p, output);

		return; /* one time ought to be enough */
	}

	igt_skip("no valid crtc/connector combinations found\n");
}

igt_simple_main
{
	data_t data = {};

	data.drm_fd = drm_open_driver_master(DRIVER_INTEL);
	igt_require_gem(data.drm_fd);
	igt_require(gem_available_fences(data.drm_fd) > 0);
	igt_require(gem_has_contexts(data.drm_fd));

	data.devid = intel_get_drm_devid(data.drm_fd);

	kmstest_set_vt_graphics_mode();

	data.bops = buf_ops_create(data.drm_fd);

	igt_display_require(&data.display, data.drm_fd);

	alloc_fence_objs(&data);

	run_test(&data);

	free_fence_objs(&data);

	buf_ops_destroy(data.bops);
	igt_display_fini(&data.display);
}
