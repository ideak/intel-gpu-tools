/*
 * Copyright © 2018 Intel Corporation
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

#include "drm_mode.h"
#include "drm_fourcc.h"
#include "igt.h"

IGT_TEST_DESCRIPTION("CRC test all different plane modes which kernel advertises.");

typedef struct {
	int gfx_fd;
	igt_display_t display;
	enum igt_commit_style commit;

	struct igt_fb fb;
	struct igt_fb primary_fb;

	union {
		char name[5];
		uint32_t dword;
	} format;
	bool separateprimaryplane;

	uint32_t gem_handle;
	uint32_t gem_handle_yuv;
	unsigned int size;
	unsigned char* buf;

	/*
	 * comparison crcs
	 */
	igt_pipe_crc_t *pipe_crc;

	igt_crc_t cursor_crc;
	igt_crc_t fullscreen_crc;
} data_t;


static int do_write(int fd, int handle, void *buf, int size)
{
	struct drm_i915_gem_pwrite write;
	memset(&write, 0x00, sizeof(write));
	write.handle = handle;
	write.data_ptr = (uintptr_t)buf;
	write.size = size;
	write.offset = 0;
	return igt_ioctl(fd, DRM_IOCTL_I915_GEM_PWRITE, &write);
}


static void generate_comparison_crc_list(data_t *data, igt_output_t *output)
{
	drmModeModeInfo *mode;
	uint64_t w, h;
	int fbid;
	cairo_t *cr;
	igt_plane_t *primary;

	mode = igt_output_get_mode(output);
	fbid = igt_create_color_fb(data->gfx_fd,
				   mode->hdisplay,
				   mode->vdisplay,
				   DRM_FORMAT_XRGB8888,
				   LOCAL_DRM_FORMAT_MOD_NONE,
				   0, 0, 0,
				   &data->primary_fb);

	igt_assert(fbid);

	drmGetCap(data->gfx_fd, DRM_CAP_CURSOR_WIDTH, &w);
	drmGetCap(data->gfx_fd, DRM_CAP_CURSOR_HEIGHT, &h);

	cr = igt_get_cairo_ctx(data->gfx_fd, &data->primary_fb);
	igt_paint_color(cr, 0, 0, mode->hdisplay, mode->vdisplay,
			    0.0, 0.0, 0.0);
	igt_paint_color(cr, 0, 0, w, h, 1.0, 1.0, 1.0);
	igt_assert(cairo_status(cr) == 0);

	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
	igt_plane_set_fb(primary, &data->primary_fb);
	igt_display_commit2(&data->display, data->commit);

	igt_pipe_crc_get_current(data->gfx_fd, data->pipe_crc, &data->cursor_crc);
	igt_plane_set_fb(primary, NULL);
	igt_display_commit2(&data->display, data->commit);

	intel_gen(intel_get_drm_devid(data->gfx_fd)) < 9 ?
		  igt_paint_color(cr, 0, 0, mode->hdisplay, mode->vdisplay, 1.0, 1.0, 1.0) :
		  igt_paint_color_alpha(cr, 0, 0, mode->hdisplay, mode->vdisplay, 1.0, 1.0, 1.0, 1.0);

	igt_plane_set_fb(primary, &data->primary_fb);
	igt_display_commit2(&data->display, data->commit);

	igt_pipe_crc_get_current(data->gfx_fd, data->pipe_crc, &data->fullscreen_crc);

	cairo_destroy(cr);
	igt_remove_fb(data->gfx_fd, &data->primary_fb);
}

static const struct {
	uint32_t	fourcc;
	char		zeropadding;
	enum		{ BYTES_PP_1=1,
				BYTES_PP_2=2,
				BYTES_PP_4=4,
				NV12,
				P010,
				SKIP4 } bpp;
	uint32_t	value;
} fillers[] = {
	{ DRM_FORMAT_C8, 0, BYTES_PP_1, 0xff},
	{ DRM_FORMAT_RGB565, 0, BYTES_PP_2, 0xffff},
	{ DRM_FORMAT_XRGB8888, 0, BYTES_PP_4, 0xffffffff},
	{ DRM_FORMAT_XBGR8888, 0, BYTES_PP_4, 0xffffffff},

	/*
	 * following two are skipped because blending seems to work
	 * incorrectly with exception of AR24 on cursor plane.
	 * Test still creates the planes, just filling plane
	 * and getting crc is skipped.
	 */
	{ DRM_FORMAT_ARGB8888, 0, SKIP4, 0xffffffff},
	{ DRM_FORMAT_ABGR8888, 0, SKIP4, 0x00ffffff},

	{ DRM_FORMAT_XRGB2101010, 0, BYTES_PP_4, 0xffffffff},
	{ DRM_FORMAT_XBGR2101010, 0, BYTES_PP_4, 0xffffffff},

	{ DRM_FORMAT_YUYV, 0, BYTES_PP_4, 0x80eb80eb},
	{ DRM_FORMAT_YVYU, 0, BYTES_PP_4, 0x80eb80eb},
	{ DRM_FORMAT_VYUY, 0, BYTES_PP_4, 0xeb80eb80},
	{ DRM_FORMAT_UYVY, 0, BYTES_PP_4, 0xeb80eb80},

	/*
	 * (semi-)planar formats
	 */
	{ DRM_FORMAT_NV12, 0, NV12, 0x80eb},
#ifdef DRM_FORMAT_P010
	{ DRM_FORMAT_P010, 0, P010, 0x8000eb00},
#endif
#ifdef DRM_FORMAT_P012
	{ DRM_FORMAT_P012, 0, P010, 0x8000eb00},
#endif
#ifdef DRM_FORMAT_P016
	{ DRM_FORMAT_P016, 0, P010, 0x8000eb00},
#endif
	{ 0, 0, 0, 0 }
};

/*
 * fill_in_fb tell in return value if selected mode should be
 * proceed to crc check
 */
static bool fill_in_fb(data_t *data, igt_output_t *output, igt_plane_t *plane,
		       uint32_t format)
{
	signed i, c, writesize;
	unsigned short* ptemp_16_buf;
	unsigned int* ptemp_32_buf;

	for( i = 0; fillers[i].fourcc != 0; i++ ) {
		if( fillers[i].fourcc == format )
			break;
	}

	switch (fillers[i].bpp) {
	case BYTES_PP_4:
		ptemp_32_buf = (unsigned int*)data->buf;
		for (c = 0; c < data->size/4; c++)
			ptemp_32_buf[c] = fillers[i].value;
		writesize = data->size;
		break;
	case BYTES_PP_2:
		ptemp_16_buf = (unsigned short*)data->buf;
		for (c = 0; c < data->size/2; c++)
			ptemp_16_buf[c] = (unsigned short)fillers[i].value;
		writesize = data->size;
		break;
	case BYTES_PP_1:
		memset((void*)data->buf, fillers[i].value, data->size);
		writesize = data->size;
		break;
	case NV12:
		memset((void*)data->buf, fillers[i].value&0xff,
		       data->size);

		memset((void*)(data->buf+data->size),
		       (fillers[i].value>>8)&0xff, data->size/2);

		writesize = data->size+data->size/2;
		break;
	case P010:
		ptemp_16_buf = (unsigned short*)data->buf;
		for (c = 0; c < data->size/2; c++)
			ptemp_16_buf[c] = (unsigned short)fillers[i].value&0xffff;

		ptemp_16_buf = (unsigned short*)(data->buf+data->size);
		for (c = 0; c < data->size/2; c++)
			ptemp_16_buf[c] = (unsigned short)(fillers[i].value>>16)&0xffff;

		writesize = data->size+data->size/2;
		break;
	case SKIP4:
		if (fillers[i].fourcc == DRM_FORMAT_ARGB8888 &&
		    plane->type == DRM_PLANE_TYPE_CURSOR) {
		/*
		 * special for cursor plane where blending works correctly.
		 */
			ptemp_32_buf = (unsigned int*)data->buf;
			for (c = 0; c < data->size/4; c++)
				ptemp_32_buf[c] = fillers[i].value;
			writesize = data->size;
			break;
		}
		igt_info("Format %s CRC comparison skipped by design.\n",
			 (char*)&fillers[i].fourcc);

		return false;
	default:
		igt_info("Unsupported mode for test %s\n",
			 (char*)&fillers[i].fourcc);
		return false;
	}

	do_write(data->gfx_fd, data->gem_handle, (void*)data->buf, writesize);
	return true;
}


static bool setup_fb(data_t *data, igt_output_t *output, igt_plane_t *plane,
		     uint32_t format)
{
	drmModeModeInfo *mode;
	uint64_t w, h;
	signed ret, gemsize = 0;
	unsigned tile_width, tile_height;
	uint32_t strides[4] = {};
	uint32_t offsets[4] = {};
	uint64_t tiling;
	int bpp = 0;
	int i;

	mode = igt_output_get_mode(output);
	if (plane->type != DRM_PLANE_TYPE_CURSOR) {
		w = mode->hdisplay;
		h = mode->vdisplay;
		tiling = LOCAL_I915_FORMAT_MOD_X_TILED;
	} else {
		drmGetCap(data->gfx_fd, DRM_CAP_CURSOR_WIDTH, &w);
		drmGetCap(data->gfx_fd, DRM_CAP_CURSOR_HEIGHT, &h);
		tiling = LOCAL_DRM_FORMAT_MOD_NONE;
	}

	for (i = 0; fillers[i].fourcc != 0; i++) {
		if (fillers[i].fourcc == format)
			break;
	}

	switch (fillers[i].bpp) {
	case NV12:
	case BYTES_PP_1:
		bpp = 8;
		break;

	case P010:
	case BYTES_PP_2:
		bpp = 16;
		break;

	case SKIP4:
	case BYTES_PP_4:
		bpp = 32;
		break;
	}

	igt_get_fb_tile_size(data->gfx_fd, tiling, bpp,
			     &tile_width, &tile_height);
	strides[0] = ALIGN(w * bpp / 8, tile_width);
	gemsize = data->size = strides[0] * ALIGN(h, tile_height);

	if (fillers[i].bpp == P010 || fillers[i].bpp == NV12) {
		offsets[1] = data->size;
		strides[1] = strides[0];
		gemsize = data->size * 2;
	}

	data->gem_handle = gem_create(data->gfx_fd, gemsize);
	ret = __gem_set_tiling(data->gfx_fd, data->gem_handle,
			       igt_fb_mod_to_tiling(tiling), strides[0]);

	igt_assert_eq(ret, 0);

	ret = __kms_addfb(data->gfx_fd, data->gem_handle, w, h,
			  format, tiling, strides, offsets,
			  LOCAL_DRM_MODE_FB_MODIFIERS, &data->fb.fb_id);

	if(ret < 0) {
		igt_info("Creating fb for format %s failed, return code %d\n",
			 (char*)&data->format.name, ret);

		return false;
	}

	data->fb.width = w;
	data->fb.height = h;
	data->fb.gem_handle = data->gem_handle;
	return true;
}


static void remove_fb(data_t* data, igt_output_t* output, igt_plane_t* plane)
{
	if (data->separateprimaryplane) {
		igt_plane_t* primary = igt_output_get_plane_type(output,
								 DRM_PLANE_TYPE_PRIMARY);
		igt_plane_set_fb(primary, NULL);
		igt_remove_fb(data->gfx_fd, &data->primary_fb);
		data->separateprimaryplane = false;
	}

	igt_remove_fb(data->gfx_fd, &data->fb);
	free(data->buf);
	data->buf = NULL;
}


static bool prepare_crtc(data_t *data, igt_output_t *output,
			 igt_plane_t *plane, uint32_t format)
{
	drmModeModeInfo *mode;
	igt_plane_t *primary;

	if (plane->type != DRM_PLANE_TYPE_PRIMARY) {
		mode = igt_output_get_mode(output);
		igt_create_color_fb(data->gfx_fd,
				    mode->hdisplay, mode->vdisplay,
				    DRM_FORMAT_XRGB8888,
				    LOCAL_DRM_FORMAT_MOD_NONE,
				    0, 0, 0,
				    &data->primary_fb);

		primary = igt_output_get_plane_type(output,
						    DRM_PLANE_TYPE_PRIMARY);

		igt_plane_set_fb(primary, &data->primary_fb);
		igt_display_commit2(&data->display, data->commit);
		data->separateprimaryplane = true;
	}

	if (!setup_fb(data, output, plane, format))
		return false;

	free((void*)data->buf);
	data->buf = (unsigned char*)calloc(data->size*2, 1);
	return true;
}


static int
test_one_mode(data_t* data, igt_output_t *output, igt_plane_t* plane,
	      int mode)
{
	igt_crc_t current_crc;
	signed rVal = 0;
	bool do_crc;
	char* crccompare[2];

	if (prepare_crtc(data, output, plane, mode)){
		/*
		 * we have fb from prepare_crtc(..) so now fill it in
		 * correctly in fill_in_fb(..)
		 */
		do_crc = fill_in_fb(data, output, plane, mode);

		igt_plane_set_fb(plane, &data->fb);
		igt_fb_set_size(&data->fb, plane, data->fb.width, data->fb.height);
		igt_plane_set_size(plane, data->fb.width, data->fb.height);
		igt_fb_set_position(&data->fb, plane, 0, 0);
		igt_display_commit2(&data->display, data->commit);

		if (do_crc) {
			igt_pipe_crc_get_current(data->gfx_fd, data->pipe_crc, &current_crc);

			if (plane->type != DRM_PLANE_TYPE_CURSOR) {
				if (!igt_check_crc_equal(&current_crc,
					&data->fullscreen_crc)) {
					crccompare[0] = igt_crc_to_string(&current_crc);
					crccompare[1] = igt_crc_to_string(&data->fullscreen_crc);
					igt_warn("crc mismatch. target %.8s, result %.8s.\n", crccompare[0], crccompare[1]);
					free(crccompare[0]);
					free(crccompare[1]);
					rVal++;
				}
			} else {
				if (!igt_check_crc_equal(&current_crc,
					&data->cursor_crc)) {
					crccompare[0] = igt_crc_to_string(&current_crc);
					crccompare[1] = igt_crc_to_string(&data->cursor_crc);
					igt_warn("crc mismatch. target %.8s, result %.8s.\n", crccompare[0], crccompare[1]);
					free(crccompare[0]);
					free(crccompare[1]);
					rVal++;
				}
			}
		}
		remove_fb(data, output, plane);
		return rVal;
	}
	return 1;
}


static void
test_available_modes(data_t* data)
{
	igt_output_t *output;
	igt_plane_t *plane;
	int modeindex;
	enum pipe pipe;
	int invalids = 0;
	drmModePlane *modePlane;
	char planetype[3][8] = {"OVERLAY\0", "PRIMARY\0", "CURSOR\0" };

	for_each_pipe_with_valid_output(&data->display, pipe, output) {
		igt_output_set_pipe(output, pipe);
		igt_display_commit2(&data->display, data->commit);

		data->pipe_crc = igt_pipe_crc_new(data->gfx_fd, pipe,
						  INTEL_PIPE_CRC_SOURCE_AUTO);

		igt_pipe_crc_start(data->pipe_crc);

		/*
		 * regenerate comparison crcs for each pipe just in case.
		 */
		generate_comparison_crc_list(data, output);

		for_each_plane_on_pipe(&data->display, pipe, plane) {
			modePlane = drmModeGetPlane(data->gfx_fd,
						    plane->drm_plane->plane_id);

			for (modeindex = 0;
			     modeindex < modePlane->count_formats;
			     modeindex++) {
				data->format.dword = modePlane->formats[modeindex];

				igt_info("Testing connector %s using pipe %s" \
					 " plane index %d type %s mode %s\n",
					 igt_output_name(output),
					 kmstest_pipe_name(pipe),
					 plane->index,
					 planetype[plane->type],
					 (char*)&data->format.name);

				invalids += test_one_mode(data, output,
							  plane,
							  modePlane->formats[modeindex]);
			}
			drmModeFreePlane(modePlane);
		}
		igt_pipe_crc_stop(data->pipe_crc);
		igt_pipe_crc_free(data->pipe_crc);
		igt_display_commit2(&data->display, data->commit);
	}
	igt_assert(invalids == 0);
}


igt_main
{
	data_t data = {};

	igt_skip_on_simulation();

	igt_fixture {
		data.gfx_fd = drm_open_driver_master(DRIVER_INTEL);
		kmstest_set_vt_graphics_mode();
		igt_display_init(&data.display, data.gfx_fd);
		igt_require_pipe_crc(data.gfx_fd);
	}

	data.commit = data.display.is_atomic ? COMMIT_ATOMIC : COMMIT_LEGACY;

	igt_subtest("available_mode_test_crc") {
		test_available_modes(&data);
	}

	igt_fixture {
		kmstest_restore_vt_mode();
		igt_display_fini(&data.display);
	}
}
