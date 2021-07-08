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
 */

#include "igt.h"
#include "igt_sysfs.h"
#include "igt_psr.h"
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

enum operations {
	PAGE_FLIP,
	MMAP_GTT,
	MMAP_CPU,
	BLT,
	RENDER,
	PLANE_MOVE,
	PLANE_ONOFF,
};

static const char *op_str(enum operations op)
{
	static const char * const name[] = {
		[PAGE_FLIP] = "page_flip",
		[MMAP_GTT] = "mmap_gtt",
		[MMAP_CPU] = "mmap_cpu",
		[BLT] = "blt",
		[RENDER] = "render",
		[PLANE_MOVE] = "plane_move",
		[PLANE_ONOFF] = "plane_onoff",
	};

	return name[op];
}

typedef struct {
	int drm_fd;
	int debugfs_fd;
	enum operations op;
	int test_plane_id;
	enum psr_mode op_psr_mode;
	uint32_t devid;
	uint32_t crtc_id;
	igt_display_t display;
	struct buf_ops *bops;
	struct igt_fb fb_green, fb_white;
	igt_plane_t *test_plane;
	int mod_size;
	int mod_stride;
	drmModeModeInfo *mode;
	igt_output_t *output;
	bool with_psr_disabled;
	bool supports_psr2;
} data_t;

static void create_cursor_fb(data_t *data)
{
	cairo_t *cr;
	uint32_t fb_id;

	fb_id = igt_create_fb(data->drm_fd, 64, 64,
			      DRM_FORMAT_ARGB8888, LOCAL_DRM_FORMAT_MOD_NONE,
			      &data->fb_white);
	igt_assert(fb_id);

	cr = igt_get_cairo_ctx(data->drm_fd, &data->fb_white);
	igt_paint_color_alpha(cr, 0, 0, 64, 64, 1.0, 1.0, 1.0, 1.0);
	igt_put_cairo_ctx(cr);
}

static void setup_output(data_t *data)
{
	igt_display_t *display = &data->display;
	igt_output_t *output;
	enum pipe pipe;

	for_each_pipe_with_valid_output(display, pipe, output) {
		drmModeConnectorPtr c = output->config.connector;

		if (c->connector_type != DRM_MODE_CONNECTOR_eDP)
			continue;

		igt_output_set_pipe(output, pipe);
		data->crtc_id = output->config.crtc->crtc_id;
		data->output = output;
		data->mode = igt_output_get_mode(output);

		return;
	}
}

static void display_init(data_t *data)
{
	igt_display_require(&data->display, data->drm_fd);
	setup_output(data);
}

static void display_fini(data_t *data)
{
	igt_display_fini(&data->display);
}

static void color_blit_start(struct intel_bb *ibb)
{
	intel_bb_out(ibb, XY_COLOR_BLT_CMD_NOLEN |
		     COLOR_BLT_WRITE_ALPHA |
		     XY_COLOR_BLT_WRITE_RGB |
		     (4 + (ibb->gen >= 8)));
}

static struct intel_buf *create_buf_from_fb(data_t *data,
					    const struct igt_fb *fb)
{
	uint32_t name, handle, tiling, stride, width, height, bpp, size;
	struct intel_buf *buf;

	igt_assert_eq(fb->offsets[0], 0);

	tiling = igt_fb_mod_to_tiling(fb->modifier);
	stride = fb->strides[0];
	bpp = fb->plane_bpp[0];
	size = fb->size;
	width = stride / (bpp / 8);
	height = size / stride;

	buf = calloc(1, sizeof(*buf));
	igt_assert(buf);

	name = gem_flink(data->drm_fd, fb->gem_handle);
	handle = gem_open(data->drm_fd, name);
	buf = intel_buf_create_using_handle(data->bops, handle,
					    width, height, bpp, 0, tiling, 0);
	intel_buf_set_ownership(buf, true);

	return buf;
}

static void fill_blt(data_t *data, const struct igt_fb *fb, unsigned char color)
{
	struct intel_bb *ibb;
	struct intel_buf *dst;

	ibb = intel_bb_create(data->drm_fd, 4096);
	dst = create_buf_from_fb(data, fb);
	intel_bb_add_intel_buf(ibb, dst, true);

	color_blit_start(ibb);
	intel_bb_out(ibb, (1 << 24) | (0xf0 << 16) | 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0xfff << 16 | 0xfff);
	intel_bb_emit_reloc(ibb, dst->handle, I915_GEM_DOMAIN_RENDER,
			    I915_GEM_DOMAIN_RENDER, 0, dst->addr.offset);
	intel_bb_out(ibb, color);

	intel_bb_flush_blit(ibb);
	intel_bb_destroy(ibb);
	intel_buf_destroy(dst);

	gem_bo_busy(data->drm_fd, fb->gem_handle);
}

static void fill_render(data_t *data, const struct igt_fb *fb,
			unsigned char color)
{
	struct intel_buf *src, *dst;
	struct intel_bb *ibb;
	const uint8_t buf[4] = { color, color, color, color };
	igt_render_copyfunc_t rendercopy = igt_get_render_copyfunc(data->devid);
	int height, width, tiling;

	igt_skip_on(!rendercopy);

	ibb = intel_bb_create(data->drm_fd, 4096);
	dst = create_buf_from_fb(data, fb);

	width = fb->strides[0] / (fb->plane_bpp[0] / 8);
	height = fb->size / fb->strides[0];
	tiling = igt_fb_mod_to_tiling(fb->modifier);

	src = intel_buf_create(data->bops, width, height, fb->plane_bpp[0],
			       0, tiling, 0);
	gem_write(data->drm_fd, src->handle, 0, buf, 4);

	rendercopy(ibb,
		   src, 0, 0, 0xff, 0xff,
		   dst, 0, 0);

	intel_bb_destroy(ibb);
	intel_buf_destroy(src);
	intel_buf_destroy(dst);

	gem_bo_busy(data->drm_fd, fb->gem_handle);
}

static bool sink_support(data_t *data, enum psr_mode mode)
{
	return data->with_psr_disabled ||
	       psr_sink_support(data->drm_fd, data->debugfs_fd, mode);
}

static bool psr_wait_entry_if_enabled(data_t *data)
{
	if (data->with_psr_disabled)
		return true;

	return psr_wait_entry(data->debugfs_fd, data->op_psr_mode);
}

static bool psr_wait_update_if_enabled(data_t *data)
{
	if (data->with_psr_disabled)
		return true;

	return psr_wait_update(data->debugfs_fd, data->op_psr_mode);
}

static bool psr_enable_if_enabled(data_t *data)
{
	if (data->with_psr_disabled)
		return true;

	return psr_enable(data->drm_fd, data->debugfs_fd, data->op_psr_mode);
}

static inline void manual(const char *expected)
{
	igt_debug_manual_check("all", expected);
}

static bool drrs_disabled(data_t *data)
{
	char buf[512];

	igt_debugfs_simple_read(data->debugfs_fd, "i915_drrs_status",
			 buf, sizeof(buf));

	return !strstr(buf, "DRRS Enabled : Yes\n");
}

static void run_test(data_t *data)
{
	uint32_t handle = data->fb_white.gem_handle;
	igt_plane_t *test_plane = data->test_plane;
	void *ptr;
	const char *expected = "";

	/* Confirm that screen became Green */
	manual("screen GREEN");

	/* Confirm screen stays Green after PSR got active */
	igt_assert(psr_wait_entry_if_enabled(data));
	manual("screen GREEN");

	/* Setting a secondary fb/plane */
	igt_plane_set_fb(test_plane, &data->fb_white);
	igt_display_commit(&data->display);

	/* Confirm it is not Green anymore */
	if (test_plane->type == DRM_PLANE_TYPE_PRIMARY)
		manual("screen WHITE");
	else
		manual("GREEN background with WHITE box");

	igt_assert(psr_wait_entry_if_enabled(data));
	switch (data->op) {
	case PAGE_FLIP:
		/* Only in use when testing primary plane */
		igt_assert(drmModePageFlip(data->drm_fd, data->crtc_id,
					   data->fb_green.fb_id, 0, NULL) == 0);
		expected = "GREEN";
		break;
	case MMAP_GTT:
		gem_require_mappable_ggtt(data->drm_fd);
		ptr = gem_mmap__gtt(data->drm_fd, handle, data->mod_size,
				    PROT_WRITE);
		gem_set_domain(data->drm_fd, handle,
			       I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);
		memset(ptr, 0xcc, data->mod_size);
		munmap(ptr, data->mod_size);
		expected = "BLACK or TRANSPARENT mark on top of plane in test";
		break;
	case MMAP_CPU:
		ptr = gem_mmap__cpu(data->drm_fd, handle, 0, data->mod_size,
				    PROT_WRITE);
		gem_set_domain(data->drm_fd, handle,
			       I915_GEM_DOMAIN_CPU, I915_GEM_DOMAIN_CPU);
		memset(ptr, 0, data->mod_size);
		munmap(ptr, data->mod_size);
		gem_sw_finish(data->drm_fd, handle);
		expected = "BLACK or TRANSPARENT mark on top of plane in test";
		break;
	case BLT:
		fill_blt(data, &data->fb_white, 0);
		expected = "BLACK or TRANSPARENT mark on top of plane in test";
		break;
	case RENDER:
		fill_render(data, &data->fb_white, 0);
		expected = "BLACK or TRANSPARENT mark on top of plane in test";
		break;
	case PLANE_MOVE:
		/* Only in use when testing Sprite and Cursor */
		igt_plane_set_position(test_plane, 500, 500);
		igt_display_commit(&data->display);
		expected = "White box moved to 500x500";
		break;
	case PLANE_ONOFF:
		/* Only in use when testing Sprite and Cursor */
		igt_plane_set_fb(test_plane, NULL);
		igt_display_commit(&data->display);
		expected = "screen GREEN";
		break;
	}
	igt_assert(psr_wait_update_if_enabled(data));
	manual(expected);
}

static void test_cleanup(data_t *data)
{
	igt_plane_t *primary;

	primary = igt_output_get_plane_type(data->output,
					    DRM_PLANE_TYPE_PRIMARY);
	igt_plane_set_fb(primary, NULL);
	igt_plane_set_fb(data->test_plane, NULL);
	igt_display_commit(&data->display);

	igt_remove_fb(data->drm_fd, &data->fb_green);
	igt_remove_fb(data->drm_fd, &data->fb_white);
}

static void setup_test_plane(data_t *data, int test_plane)
{
	uint32_t white_h, white_v;
	igt_plane_t *primary, *sprite, *cursor;

	igt_create_color_fb(data->drm_fd,
			    data->mode->hdisplay, data->mode->vdisplay,
			    DRM_FORMAT_XRGB8888,
			    LOCAL_I915_FORMAT_MOD_X_TILED,
			    0.0, 1.0, 0.0,
			    &data->fb_green);

	primary = igt_output_get_plane_type(data->output,
					    DRM_PLANE_TYPE_PRIMARY);
	igt_plane_set_fb(primary, NULL);
	data->test_plane = primary;

	white_h = data->mode->hdisplay;
	white_v = data->mode->vdisplay;

	/* Ignoring pitch and bpp to avoid changing full screen */
	data->mod_size = white_h * white_v;
	data->mod_stride = white_h * 4;

	switch (test_plane) {
	case DRM_PLANE_TYPE_OVERLAY:
		sprite = igt_output_get_plane_type(data->output,
						   DRM_PLANE_TYPE_OVERLAY);
		igt_plane_set_fb(sprite, NULL);
		white_h = white_h/2;
		white_v = white_v/2;
		data->test_plane = sprite;
	case DRM_PLANE_TYPE_PRIMARY:
		igt_create_color_fb(data->drm_fd,
				    white_h, white_v,
				    DRM_FORMAT_XRGB8888,
				    LOCAL_I915_FORMAT_MOD_X_TILED,
				    1.0, 1.0, 1.0,
				    &data->fb_white);
		break;
	case DRM_PLANE_TYPE_CURSOR:
		cursor = igt_output_get_plane_type(data->output,
						   DRM_PLANE_TYPE_CURSOR);
		igt_plane_set_fb(cursor, NULL);
		create_cursor_fb(data);
		igt_plane_set_position(cursor, 0, 0);

		/* Cursor is 64 x 64, ignoring pitch and bbp again */
		data->mod_size = 64 * 64;
		data->test_plane = cursor;
		break;
	}

	igt_display_commit(&data->display);

	igt_plane_set_fb(primary, &data->fb_green);
	igt_display_commit(&data->display);
}

static void test_setup(data_t *data)
{
	igt_require_f(data->output,
		      "No available output found\n");
	igt_require_f(data->mode,
		      "No available mode found on %s\n",
		      data->output->name);

	if (data->op_psr_mode == PSR_MODE_2)
		igt_require_f(intel_display_ver(intel_get_drm_devid(data->drm_fd)) < 13,
			      "Intentionally not testing this on Display 13+, Kernel change required to enable testing\n");

	if (data->op_psr_mode == PSR_MODE_2)
		igt_require(data->supports_psr2);

	psr_enable_if_enabled(data);
	setup_test_plane(data, data->test_plane_id);
	igt_assert(psr_wait_entry_if_enabled(data));
}

static void dpms_off_on(data_t *data)
{
	kmstest_set_connector_dpms(data->drm_fd, data->output->config.connector,
				   DRM_MODE_DPMS_OFF);
	kmstest_set_connector_dpms(data->drm_fd, data->output->config.connector,
				   DRM_MODE_DPMS_ON);
}

static int opt_handler(int opt, int opt_index, void *_data)
{
	data_t *data = _data;

	switch (opt) {
	case 'n':
		data->with_psr_disabled = true;
		break;
	default:
		return IGT_OPT_HANDLER_ERROR;
	}

	return IGT_OPT_HANDLER_SUCCESS;
}

const char *help_str =
	"  --no-psr\tRun test without PSR/PSR2.";
static struct option long_options[] = {
	{"no-psr", 0, 0, 'n'},
	{ 0, 0, 0, 0 }
};
data_t data = {};

igt_main_args("", long_options, help_str, opt_handler, &data)
{
	enum operations op;
	const char *append_subtest_name[2] = {
		"",
		"psr2_"
	};

	igt_fixture {
		data.drm_fd = drm_open_driver_master(DRIVER_INTEL);
		data.debugfs_fd = igt_debugfs_dir(data.drm_fd);
		kmstest_set_vt_graphics_mode();
		data.devid = intel_get_drm_devid(data.drm_fd);

		igt_require_f(sink_support(&data, PSR_MODE_1),
			      "Sink does not support PSR\n");

		data.supports_psr2 = sink_support(&data, PSR_MODE_2);
		data.bops = buf_ops_create(data.drm_fd);
		display_init(&data);
	}

	for (data.op_psr_mode = PSR_MODE_1; data.op_psr_mode <= PSR_MODE_2;
	     data.op_psr_mode++) {

		igt_describe("Basic check for psr if it is detecting changes made in planes");
		igt_subtest_f("%sbasic", append_subtest_name[data.op_psr_mode]) {
			data.test_plane_id = DRM_PLANE_TYPE_PRIMARY;
			test_setup(&data);
			test_cleanup(&data);
		}

		igt_describe("Check if psr is detecting changes when drrs is disabled");
		igt_subtest_f("%sno_drrs", append_subtest_name[data.op_psr_mode]) {
			data.test_plane_id = DRM_PLANE_TYPE_PRIMARY;
			test_setup(&data);
			igt_assert(drrs_disabled(&data));
			test_cleanup(&data);
		}

		for (op = PAGE_FLIP; op <= RENDER; op++) {
			igt_describe("Check if psr is detecting page-flipping,memory mapping and "
					"rendering operations performed on primary planes");
			igt_subtest_f("%sprimary_%s",
				      append_subtest_name[data.op_psr_mode],
				      op_str(op)) {
				data.op = op;
				data.test_plane_id = DRM_PLANE_TYPE_PRIMARY;
				test_setup(&data);
				run_test(&data);
				test_cleanup(&data);
			}
		}

		for (op = MMAP_GTT; op <= PLANE_ONOFF; op++) {
			igt_describe("Check if psr is detecting memory mapping,rendering "
					"and plane operations performed on sprite planes");
			igt_subtest_f("%ssprite_%s",
				      append_subtest_name[data.op_psr_mode],
				      op_str(op)) {
				data.op = op;
				data.test_plane_id = DRM_PLANE_TYPE_OVERLAY;
				test_setup(&data);
				run_test(&data);
				test_cleanup(&data);
			}

			igt_describe("Check if psr is detecting memory mapping, rendering "
					"and plane operations performed on cursor planes");
			igt_subtest_f("%scursor_%s",
				      append_subtest_name[data.op_psr_mode],
				      op_str(op)) {
				data.op = op;
				data.test_plane_id = DRM_PLANE_TYPE_CURSOR;
				test_setup(&data);
				run_test(&data);
				test_cleanup(&data);
			}
		}

		igt_describe("Check if psr is detecting changes when rendering operation is performed"
				"  with dpms enabled or disabled");
		igt_subtest_f("%sdpms", append_subtest_name[data.op_psr_mode]) {
			data.op = RENDER;
			data.test_plane_id = DRM_PLANE_TYPE_PRIMARY;
			test_setup(&data);
			dpms_off_on(&data);
			run_test(&data);
			test_cleanup(&data);
		}

		igt_describe("Check if psr is detecting changes when plane operation is performed "
				"with suspend resume cycles");
		igt_subtest_f("%ssuspend", append_subtest_name[data.op_psr_mode]) {
			data.op = PLANE_ONOFF;
			data.test_plane_id = DRM_PLANE_TYPE_CURSOR;
			test_setup(&data);
			igt_system_suspend_autoresume(SUSPEND_STATE_MEM,
						      SUSPEND_TEST_NONE);
			igt_assert(psr_wait_entry_if_enabled(&data));
			run_test(&data);
			test_cleanup(&data);
		}
	}

	igt_fixture {
		if (!data.with_psr_disabled)
			psr_disable(data.drm_fd, data.debugfs_fd);

		close(data.debugfs_fd);
		buf_ops_destroy(data.bops);
		display_fini(&data);
	}
}
