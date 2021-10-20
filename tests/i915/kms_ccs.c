/*
 * Copyright © 2016 Intel Corporation
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

#include "i915/gem_create.h"

#define SDR_PLANE_BASE	3

IGT_TEST_DESCRIPTION("Test render compression (RC), in which the main surface "
		     "is complemented by a color control surface (CCS) that "
		     "the display uses to interpret the compressed data.");

enum test_flags {
	TEST_CRC			= 1 << 1,
	TEST_ROTATE_180			= 1 << 2,
	TEST_BAD_PIXEL_FORMAT		= 1 << 3,
	TEST_BAD_ROTATION_90		= 1 << 4,
	TEST_NO_AUX_BUFFER		= 1 << 5,
	TEST_BAD_CCS_HANDLE		= 1 << 6,
	TEST_BAD_AUX_STRIDE		= 1 << 7,
	TEST_RANDOM			= 1 << 8,
	TEST_ALL_PLANES			= 1 << 9,
};

#define TEST_BAD_CCS_PLANE	(TEST_NO_AUX_BUFFER | TEST_BAD_CCS_HANDLE | \
				 TEST_BAD_AUX_STRIDE)
#define TEST_FAIL_ON_ADDFB2	(TEST_BAD_PIXEL_FORMAT | TEST_BAD_CCS_PLANE)

enum test_fb_flags {
	FB_COMPRESSED			= 1 << 0,
	FB_HAS_PLANE			= 1 << 1,
	FB_MISALIGN_AUX_STRIDE		= 1 << 2,
	FB_SMALL_AUX_STRIDE		= 1 << 3,
	FB_ZERO_AUX_STRIDE		= 1 << 4,
	FB_RANDOM			= 1 << 5,
};

typedef struct {
	int drm_fd;
	igt_display_t display;
	igt_output_t *output;
	enum pipe pipe;
	enum test_flags flags;
	igt_plane_t *plane;
	igt_pipe_crc_t *pipe_crc;
	uint32_t format;
	uint64_t ccs_modifier;
	unsigned int seed;
	bool user_seed;
} data_t;

static const struct {
	double r;
	double g;
	double b;
} colors[2] = {
	{1.0, 0.0, 0.0},
	{0.0, 1.0, 0.0}
};

static const uint32_t formats[] = {
	DRM_FORMAT_XYUV8888,
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_YUYV,
	DRM_FORMAT_NV12,
	DRM_FORMAT_P012,
	DRM_FORMAT_P016,
};

static const struct {
	uint64_t modifier;
	const char *str;
} ccs_modifiers[] = {
	{I915_FORMAT_MOD_Y_TILED_CCS, "y_tiled_ccs"},
	{I915_FORMAT_MOD_Yf_TILED_CCS, "yf_tiled_ccs"},
	{I915_FORMAT_MOD_Y_TILED_GEN12_RC_CCS, "y_tiled_gen12_rc_ccs"},
	{I915_FORMAT_MOD_Y_TILED_GEN12_RC_CCS_CC, "y_tiled_gen12_rc_ccs_cc"},
	{I915_FORMAT_MOD_Y_TILED_GEN12_MC_CCS, "y_tiled_gen12_mc_ccs"},
	{I915_FORMAT_MOD_4_TILED_DG2_RC_CCS, "4_tiled_dg2_rc_ccs"},
	{I915_FORMAT_MOD_4_TILED_DG2_MC_CCS, "4_tiled_dg2_mc_ccs"},
	{I915_FORMAT_MOD_4_TILED_DG2_RC_CCS_CC, "4_tiled_dg2_rc_ccs_cc"},
};

static bool check_ccs_planes;

static const struct {
	const enum test_flags	flags;
	const char		*testname;
	const char		*description;
} tests[] = {
	{TEST_BAD_PIXEL_FORMAT, "bad-pixel-format", "Test bad pixel format with given CCS modifier"},
	{TEST_BAD_ROTATION_90, "bad-rotation-90", "Test 90 degree rotation with given CCS modifier"},
	{TEST_CRC, "crc-primary-basic", "Test primary plane CRC compatibility with given CCS modifier"},
	{TEST_CRC | TEST_ROTATE_180, "crc-primary-rotation-180", "Test 180 degree rotation with given CCS modifier"},
	{TEST_RANDOM, "random-ccs-data", "Test random CCS data"},
	{TEST_NO_AUX_BUFFER, "missing-ccs-buffer", "Test missing CCS buffer with given CCS modifier"},
	{TEST_BAD_CCS_HANDLE, "ccs-on-another-bo", "Test CCS with different BO with given modifier"},
	{TEST_BAD_AUX_STRIDE, "bad-aux-stride", "Test with bad AUX stride with given CCS modifier"},
	{TEST_CRC | TEST_ALL_PLANES, "crc-sprite-planes-basic", "Test sprite plane CRC compatibility with given CCS modifier"},
};

/*
 * Limit maximum used sprite plane width so this test will not mistakenly
 * fail on hardware limitations which are not interesting to this test.
 * On this test too wide sprite plane may fail during creation with dmesg
 * comment saying:
 * "Requested display configuration exceeds system watermark limitations"
 */
#define MAX_SPRITE_PLANE_WIDTH 2000

static void addfb_init(struct igt_fb *fb, struct drm_mode_fb_cmd2 *f)
{
	int i;

	f->width = fb->width;
	f->height = fb->height;
	f->pixel_format = fb->drm_format;
	f->flags = DRM_MODE_FB_MODIFIERS;

	for (i = 0; i < fb->num_planes; i++) {
		f->handles[i] = fb->gem_handle;
		f->modifier[i] = fb->modifier;
		f->pitches[i] = fb->strides[i];
		f->offsets[i] = fb->offsets[i];
	}
}

static void
create_fb_prepare_add(int drm_fd, int width, int height,
		      uint32_t format, uint64_t modifier,
		      igt_fb_t *fb, struct drm_mode_fb_cmd2 *f)
{
	igt_create_bo_for_fb(drm_fd, width, height, format, modifier, fb);
	igt_assert(fb->gem_handle > 0);

	addfb_init(fb, f);
}

static bool is_ccs_cc_modifier(uint64_t modifier)
{
	return modifier == I915_FORMAT_MOD_Y_TILED_GEN12_RC_CCS_CC ||
		modifier == I915_FORMAT_MOD_4_TILED_DG2_RC_CCS_CC;
}

/*
 * The CCS planes of compressed framebuffers contain non-zero bytes if the
 * engine compressed effectively the framebuffer. The actual encoding of these
 * bytes is not specified, but we know that seeing an all-zero CCS plane means
 * that the engine left the FB uncompressed, which is not what we expect in
 * the test. Look for the first non-zero byte in the given CCS plane to get a
 * minimal assurance that compression took place.
 */
static void check_ccs_plane(int drm_fd, igt_fb_t *fb, int plane)
{
	void *map;
	void *ccs_p;
	size_t ccs_size;
	int i;

	ccs_size = fb->strides[plane] * fb->plane_height[plane];
	igt_assert(ccs_size);

	gem_set_domain(drm_fd, fb->gem_handle, I915_GEM_DOMAIN_CPU, 0);

	map = gem_mmap__cpu(drm_fd, fb->gem_handle, 0, fb->size, PROT_READ);

	ccs_size = fb->strides[plane] * fb->plane_height[plane];
	ccs_p = map + fb->offsets[plane];
	for (i = 0; i < ccs_size; i += sizeof(uint32_t))
		if (*(uint32_t *)(ccs_p + i))
			break;

	munmap(map, fb->size);

	igt_assert_f(i < ccs_size,
		     "CCS plane %d (for main plane %d) lacks compression meta-data\n",
		     plane, igt_fb_ccs_to_main_plane(fb, plane));
}

static void check_ccs_cc_plane(int drm_fd, igt_fb_t *fb, int plane, const float *cc_color)
{
	union cc {
		float f;
		uint32_t d;
	} *cc_p;
	void *map;
	uint32_t native_color;

	gem_set_domain(drm_fd, fb->gem_handle, I915_GEM_DOMAIN_CPU, 0);

	map = gem_mmap__cpu(drm_fd, fb->gem_handle, 0, fb->size, PROT_READ);
	cc_p = map + fb->offsets[plane];

	igt_assert(cc_color[0] == cc_p[0].f &&
		   cc_color[1] == cc_p[1].f &&
		   cc_color[2] == cc_p[2].f &&
		   cc_color[3] == cc_p[3].f);

	native_color = (uint8_t)(cc_color[3] * 0xff) << 24 |
		       (uint8_t)(cc_color[0] * 0xff) << 16 |
		       (uint8_t)(cc_color[1] * 0xff) << 8 |
		       (uint8_t)(cc_color[2] * 0xff);

	igt_assert(native_color == cc_p[4].d);

	munmap(map, fb->size);
};

static void check_all_ccs_planes(int drm_fd, igt_fb_t *fb, const float *cc_color, bool check_cc_plane)
{
	int i;

	for (i = 0; i < fb->num_planes; i++) {
		if (igt_fb_is_ccs_plane(fb, i) &&
		    !igt_fb_is_gen12_ccs_cc_plane(fb, i))
			check_ccs_plane(drm_fd, fb, i);
		else if (igt_fb_is_gen12_ccs_cc_plane(fb, i) && check_cc_plane)
			check_ccs_cc_plane(drm_fd, fb, i, cc_color);
	}
}

static void fill_fb_random(int drm_fd, igt_fb_t *fb)
{
	void *map;
	uint8_t *p;
	int i;

	gem_set_domain(drm_fd, fb->gem_handle, I915_GEM_DOMAIN_CPU, I915_GEM_DOMAIN_CPU);

	p = map = gem_mmap__cpu(drm_fd, fb->gem_handle, 0, fb->size, PROT_WRITE);

	for (i = 0; i < fb->size; i++)
		p[i] = rand();

	munmap(map, fb->size);
}

static void test_bad_ccs_plane(data_t *data, int width, int height, int ccs_plane,
			       enum test_fb_flags fb_flags)
{
	struct igt_fb fb = {};
	struct drm_mode_fb_cmd2 f = {};
	uint32_t bad_ccs_bo = 0;
	int addfb_errno;
	int ret;

	igt_assert(fb_flags & FB_COMPRESSED);
	create_fb_prepare_add(data->drm_fd, width, height,
			      data->format, data->ccs_modifier,
			      &fb, &f);

	/*
	 * The stride of CCS planes on GEN12+ is fixed, so we can check for
	 * an incorrect stride with the same delta as on earlier platforms.
	 */
	if (fb_flags & FB_MISALIGN_AUX_STRIDE) {
		igt_skip_on_f(HAS_FLATCCS(intel_get_drm_devid(data->drm_fd)), "No aux plane on flat ccs.\n");
		igt_skip_on_f(width <= 1024,
			      "FB already has the smallest possible stride\n");
		f.pitches[ccs_plane] -= 64;
	}

	if (fb_flags & FB_SMALL_AUX_STRIDE) {
		igt_skip_on_f(HAS_FLATCCS(intel_get_drm_devid(data->drm_fd)), "No aux plane on flat ccs.\n");
		igt_skip_on_f(width <= 1024,
			      "FB already has the smallest possible stride\n");
		f.pitches[ccs_plane] = ALIGN(f.pitches[ccs_plane] / 2, 128);
	}

	if (fb_flags & FB_ZERO_AUX_STRIDE)
		f.pitches[ccs_plane] = 0;

	/* Put the CCS buffer on a different BO. */
	if (data->flags & TEST_BAD_CCS_HANDLE) {
		bad_ccs_bo = gem_create(data->drm_fd, fb.size);
		f.handles[ccs_plane] = bad_ccs_bo;
	}

	if (data->flags & TEST_NO_AUX_BUFFER) {
		igt_skip_on_f(HAS_FLATCCS(intel_get_drm_devid(data->drm_fd)), "No aux plane on flat ccs.\n");
		f.handles[ccs_plane] = 0;
		f.modifier[ccs_plane] = 0;
		f.pitches[ccs_plane] = 0;
		f.offsets[ccs_plane] = 0;
	}

	ret = drmIoctl(data->drm_fd, DRM_IOCTL_MODE_ADDFB2, &f);
	addfb_errno = errno;

	if (bad_ccs_bo)
		gem_close(data->drm_fd, bad_ccs_bo);

	igt_assert_eq(ret, -1);
	igt_assert_eq(addfb_errno, EINVAL);

	gem_close(data->drm_fd, fb.gem_handle);
}

static void test_bad_ccs_plane_params(data_t *data, int width, int height,
				      enum test_fb_flags fb_flags)
{
	for (int ccs_plane = 1;
	     ccs_plane <= (igt_format_is_yuv_semiplanar(data->format) ? 2 : 1);
	     ccs_plane++)
		test_bad_ccs_plane(data, width, height, ccs_plane, fb_flags);
}

static void test_bad_pixel_format(data_t *data, int width, int height,
				  enum test_fb_flags fb_flags)
{
	struct igt_fb fb = {};
	struct drm_mode_fb_cmd2 f = {};
	int ret;

	igt_assert(fb_flags & FB_COMPRESSED);
	create_fb_prepare_add(data->drm_fd, width, height,
			      DRM_FORMAT_RGB565, data->ccs_modifier,
			      &fb, &f);

	ret = drmIoctl(data->drm_fd, DRM_IOCTL_MODE_ADDFB2, &f);
	igt_assert_eq(ret, -1);
	igt_assert_eq(errno, EINVAL);

	gem_close(data->drm_fd, fb.gem_handle);
}

static void test_bad_fb_params(data_t *data, int width, int height, enum test_fb_flags fb_flags)
{
	if (data->flags & TEST_BAD_PIXEL_FORMAT)
		test_bad_pixel_format(data, width, height, fb_flags);

	if (data->flags & TEST_BAD_CCS_PLANE)
		test_bad_ccs_plane_params(data, width, height, fb_flags);
}

static void fast_clear_fb(int drm_fd, struct igt_fb *fb, const float *cc_color)
{
	igt_render_clearfunc_t fast_clear = igt_get_render_clearfunc(intel_get_drm_devid(drm_fd));
	struct intel_bb *ibb = intel_bb_create(drm_fd, 4096);
	struct buf_ops *bops = buf_ops_create(drm_fd);
	struct intel_buf *dst = igt_fb_create_intel_buf(drm_fd, bops, fb, "fast clear dst");

	gem_set_domain(drm_fd, fb->gem_handle,
		       I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);

	fast_clear(ibb, dst, 0, 0, fb->width, fb->height, cc_color);

	intel_bb_sync(ibb);
	intel_bb_destroy(ibb);
	intel_buf_destroy(dst);
	buf_ops_destroy(bops);
}

static void generate_fb(data_t *data, struct igt_fb *fb,
			int width, int height,
			enum test_fb_flags fb_flags)
{
	struct drm_mode_fb_cmd2 f = {0};
	uint64_t modifier;
	cairo_t *cr;
	int ret;
	const float cc_color[4] = {colors[!!data->plane].r,
				   colors[!!data->plane].g,
				   colors[!!data->plane].b,
				   1.0};

	/* Use either compressed or linear to test. However, given the lack of
	 * available bandwidth, we use linear for the primary plane when
	 * testing sprites, since we cannot fit two CCS planes into the
	 * available FIFO configurations.
	 */
	if (fb_flags & FB_COMPRESSED)
		modifier = data->ccs_modifier;
	else if (!(fb_flags & FB_HAS_PLANE))
		modifier = DRM_FORMAT_MOD_LINEAR;
	else
		modifier = 0;

	create_fb_prepare_add(data->drm_fd, width, height,
			      data->format, modifier,
			      fb, &f);

	if (data->flags & TEST_RANDOM) {
		srand(data->seed);
		fill_fb_random(data->drm_fd, fb);
	} else {
		bool do_fast_clear = is_ccs_cc_modifier(data->ccs_modifier);
		bool do_solid_fill = do_fast_clear || data->plane;
		int c = !!data->plane;

		if (do_fast_clear && (fb_flags & FB_COMPRESSED)) {
			fast_clear_fb(data->drm_fd, fb, cc_color);
		} else {
			cr = igt_get_cairo_ctx(data->drm_fd, fb);

			if (do_solid_fill)
				igt_paint_color(cr, 0, 0, width, height,
						colors[c].r, colors[c].g, colors[c].b);
			else
				igt_paint_test_pattern(cr, width, height);

			igt_put_cairo_ctx(cr);
		}
	}

	ret = drmIoctl(data->drm_fd, DRM_IOCTL_MODE_ADDFB2, &f);
	igt_assert_eq(ret, 0);

	if (check_ccs_planes)
		check_all_ccs_planes(data->drm_fd, fb, cc_color, !(data->flags & TEST_RANDOM));

	fb->fb_id = f.fb_id;
}

static igt_plane_t *first_sdr_plane(data_t *data)
{
	return igt_output_get_plane(data->output, SDR_PLANE_BASE);
}

static bool is_sdr_plane(const igt_plane_t *plane)
{
	return plane->index >= SDR_PLANE_BASE;
}

/*
 * Mixing SDR and HDR planes results in a CRC mismatch, so use the first
 * SDR/HDR plane as the main plane matching the SDR/HDR type of the sprite
 * plane under test.
 */
static igt_plane_t *compatible_main_plane(data_t *data)
{
	if (data->plane && is_sdr_plane(data->plane) &&
	    igt_format_is_yuv(data->format))
		return first_sdr_plane(data);

	return igt_output_get_plane_type(data->output, DRM_PLANE_TYPE_PRIMARY);
}

static bool try_config(data_t *data, enum test_fb_flags fb_flags,
		       igt_crc_t *crc)
{
	igt_display_t *display = &data->display;
	igt_plane_t *primary = compatible_main_plane(data);
	drmModeModeInfo *drm_mode = igt_output_get_mode(data->output);
	int fb_width = drm_mode->hdisplay;
	enum igt_commit_style commit;
	struct igt_fb fb = {};
	struct igt_fb fb_sprite = {};
	int ret;

	if (data->display.is_atomic)
		commit = COMMIT_ATOMIC;
	else
		commit = COMMIT_UNIVERSAL;

	if (primary == data->plane)
		return false;

	if (!igt_plane_has_format_mod(primary, data->format,
				      data->ccs_modifier))
		return false;

	if (is_ccs_cc_modifier(data->ccs_modifier) &&
	    data->format != DRM_FORMAT_XRGB8888)
		return false;

	if ((fb_flags & FB_MISALIGN_AUX_STRIDE) ||
	    (fb_flags & FB_SMALL_AUX_STRIDE))
		fb_width = max(fb_width, 1536);

	fb_width = min(MAX_SPRITE_PLANE_WIDTH, fb_width);

	if (data->flags & TEST_FAIL_ON_ADDFB2) {
		test_bad_fb_params(data, fb_width, drm_mode->vdisplay, fb_flags);
		return true;
	}

	if (data->plane && fb_flags & FB_COMPRESSED) {
		if (!igt_plane_has_format_mod(data->plane, data->format,
					      data->ccs_modifier))
			return false;

		generate_fb(data, &fb, fb_width, drm_mode->vdisplay,
			    (fb_flags & ~FB_COMPRESSED) | FB_HAS_PLANE);
		generate_fb(data, &fb_sprite, 256, 256, fb_flags);
	} else {
		generate_fb(data, &fb, fb_width, drm_mode->vdisplay, fb_flags);
	}

	igt_plane_set_position(primary, 0, 0);
	igt_plane_set_size(primary, drm_mode->hdisplay, drm_mode->vdisplay);
	igt_plane_set_fb(primary, &fb);

	if (data->plane && fb_flags & FB_COMPRESSED) {
		igt_plane_set_position(data->plane, 0, 0);
		igt_plane_set_size(data->plane, 256, 256);
		igt_plane_set_fb(data->plane, &fb_sprite);
	}

	if (data->flags & TEST_ROTATE_180)
		igt_plane_set_rotation(primary, IGT_ROTATION_180);
	if (data->flags & TEST_BAD_ROTATION_90)
		igt_plane_set_rotation(primary, IGT_ROTATION_90);

	ret = igt_display_try_commit2(display, commit);

	if (ret == 0 && !(fb_flags & TEST_BAD_ROTATION_90) && crc)
		igt_pipe_crc_collect_crc(data->pipe_crc, crc);

	igt_debug_wait_for_keypress("ccs");

	if (data->plane && fb_flags & FB_COMPRESSED) {
		igt_plane_set_position(data->plane, 0, 0);
		igt_plane_set_size(data->plane, 0, 0);
		igt_plane_set_fb(data->plane, NULL);
	}

	igt_plane_set_fb(primary, NULL);
	igt_plane_set_rotation(primary, IGT_ROTATION_0);
	igt_display_commit2(display, commit);

	igt_remove_fb(data->drm_fd, &fb_sprite);
	igt_remove_fb(data->drm_fd, &fb);

	igt_assert_eq(ret, data->flags & TEST_BAD_ROTATION_90 ? -EINVAL : 0);

	return true;
}

static int test_ccs(data_t *data)
{	int valid_tests = 0;
	igt_crc_t crc, ref_crc;
	enum test_fb_flags fb_flags = 0;

	if (data->flags & TEST_CRC) {
		data->pipe_crc = igt_pipe_crc_new(data->drm_fd, data->pipe,
						  IGT_PIPE_CRC_SOURCE_AUTO);

		if (try_config(data, fb_flags | FB_COMPRESSED, &ref_crc) &&
		    try_config(data, fb_flags, &crc)) {
			igt_assert_crc_equal(&crc, &ref_crc);
			valid_tests++;
		}

		igt_pipe_crc_free(data->pipe_crc);
		data->pipe_crc = NULL;
	}

	if (data->flags & TEST_RANDOM)
		valid_tests += try_config(data, fb_flags | FB_COMPRESSED | FB_RANDOM, NULL);

	if (data->flags & TEST_BAD_PIXEL_FORMAT ||
	    data->flags & TEST_BAD_ROTATION_90 ||
	    data->flags & TEST_NO_AUX_BUFFER ||
	    data->flags & TEST_BAD_CCS_HANDLE) {
		valid_tests += try_config(data, fb_flags | FB_COMPRESSED, NULL);
	}

	if (data->flags & TEST_BAD_AUX_STRIDE) {
		valid_tests += try_config(data, fb_flags | FB_COMPRESSED | FB_MISALIGN_AUX_STRIDE , NULL);
		valid_tests += try_config(data, fb_flags | FB_COMPRESSED | FB_SMALL_AUX_STRIDE , NULL);
		valid_tests += try_config(data, fb_flags | FB_COMPRESSED | FB_ZERO_AUX_STRIDE , NULL);
	}

	return valid_tests;
}

static void test_output(data_t *data, const int testnum)
{
	igt_fixture {
		data->flags = tests[testnum].flags;

		data->output = igt_get_single_output_for_pipe(&data->display,
							      data->pipe);
		igt_require(data->output);
		igt_output_set_pipe(data->output, data->pipe);
	}

	for (int i = 0; i < ARRAY_SIZE(ccs_modifiers); i++) {
		if ((ccs_modifiers[i].modifier == I915_FORMAT_MOD_4_TILED_DG2_RC_CCS ||
		    ccs_modifiers[i].modifier == I915_FORMAT_MOD_4_TILED_DG2_MC_CCS ||
		    ccs_modifiers[i].modifier == I915_FORMAT_MOD_4_TILED_DG2_RC_CCS_CC) &&
		    tests[testnum].flags & TEST_BAD_CCS_PLANE)
		    continue;

		data->ccs_modifier = ccs_modifiers[i].modifier;

		igt_describe(tests[testnum].description);
		igt_subtest_f("pipe-%s-%s-%s", kmstest_pipe_name(data->pipe),
			      tests[testnum].testname, ccs_modifiers[i].str) {
			int valid_tests = 0;
			igt_require(data->output);

			if (data->flags == TEST_RANDOM)
				igt_info("Testing with seed %d\n", data->seed);

			if (data->flags & TEST_ALL_PLANES) {
				igt_display_require_output_on_pipe(&data->display, data->pipe);

				for_each_plane_on_pipe(&data->display, data->pipe, data->plane) {
					for (int j = 0; j < ARRAY_SIZE(formats); j++) {
						data->format = formats[j];
						valid_tests += test_ccs(data);
					}
				}
			} else {
				for (int j = 0; j < ARRAY_SIZE(formats); j++) {
					data->format = formats[j];
					valid_tests += test_ccs(data);
				}
			}
			igt_require_f(valid_tests > 0,
				      "no valid tests for %s on pipe %s\n",
				      ccs_modifiers[i].str,
				      kmstest_pipe_name(data->pipe));
		}
	}

	igt_fixture {
		igt_output_set_pipe(data->output, PIPE_NONE);
		igt_display_commit2(&data->display, data->display.is_atomic ?
				    COMMIT_ATOMIC : COMMIT_LEGACY);
		data->plane = NULL;
	}
}

static int opt_handler(int opt, int opt_index, void *opt_data)
{
	data_t *data = opt_data;

	switch (opt) {
	case 'c':
		check_ccs_planes = true;
		break;
	case 's':
		data->user_seed = true;
		data->seed = strtoul(optarg, NULL, 0);
		break;
	default:
		return IGT_OPT_HANDLER_ERROR;
	}

	return IGT_OPT_HANDLER_SUCCESS;
}

static data_t data;

static const char *help_str =
"  -c\t\tCheck the presence of compression meta-data\n"
"  -s <seed>\tSeed for random number generator\n"
;

igt_main_args("cs:", NULL, help_str, opt_handler, &data)
{
	enum pipe pipe;

	igt_fixture {
		data.drm_fd = drm_open_driver_master(DRIVER_INTEL);

		igt_require(intel_display_ver(intel_get_drm_devid(data.drm_fd)) >= 9);
		kmstest_set_vt_graphics_mode();
		igt_require_pipe_crc(data.drm_fd);

		igt_display_require(&data.display, data.drm_fd);
		igt_display_require_output(&data.display);

		if (!data.user_seed)
			data.seed = time(NULL);
	}

	for_each_pipe_static(pipe) {
		data.pipe = pipe;

		igt_subtest_group {
			for (int c = 0; c < ARRAY_SIZE(tests); c++) {
				test_output(&data, c);
			}
		}
	}

	igt_fixture {
		igt_display_fini(&data.display);
		close(data.drm_fd);
	}
}
