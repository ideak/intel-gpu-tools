/*
 * Copyright © 2013 Intel Corporation
 * Copyright © 2018 Collabora, Ltd.
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
 *    Daniel Vetter <daniel.vetter@ffwll.ch>
 *    Daniel Stone <daniels@collabora.com>
 *
 */

#include "igt.h"
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <inttypes.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/ioctl.h>

#include "drm.h"
#include "drm_fourcc.h"
#include "i915/gem_create.h"
#include "igt_device.h"

IGT_TEST_DESCRIPTION("Tests GETFB and GETFB2 ioctls.");

static bool has_getfb_iface(int fd)
{
	struct drm_mode_fb_cmd arg = { };
	int err;

	err = 0;
	if (drmIoctl(fd, DRM_IOCTL_MODE_GETFB, &arg))
		err = -errno;
	switch (err) {
	case -ENOTTY: /* ioctl unrecognised (kernel too old) */
	case -ENOTSUP: /* driver doesn't support KMS */
		return false;
	default:
		return true;
	}
}

static bool has_addfb2_iface(int fd)
{
	struct drm_mode_fb_cmd2 arg = { };
	int err;

	err = 0;
	if (drmIoctl(fd, DRM_IOCTL_MODE_ADDFB2, &arg))
		err = -errno;
	switch (err) {
	case -ENOTTY: /* ioctl unrecognised (kernel too old) */
	case -ENOTSUP: /* driver doesn't support KMS */
		return false;
	default:
		return true;
	}
}

static void get_ccs_fb(int fd, struct drm_mode_fb_cmd2 *ret)
{
	struct drm_mode_fb_cmd2 add = {
		.width = 1024,
		.height = 1024,
		.pixel_format = DRM_FORMAT_XRGB8888,
		.flags = DRM_MODE_FB_MODIFIERS,
	};
	int size;

	igt_require(has_addfb2_iface(fd));
	igt_require_intel(fd);

	if ((intel_display_ver(intel_get_drm_devid(fd))) >= 12) {
		add.modifier[0] = I915_FORMAT_MOD_Y_TILED_GEN12_RC_CCS;
		add.modifier[1] = I915_FORMAT_MOD_Y_TILED_GEN12_RC_CCS;

		/* The main surface for TGL is 4x4 tiles aligned
		 * For 32bpp the pitch is 4*4*32 bytes i.e. 512 bytes
		 */
		add.pitches[0] = ALIGN(add.width * 4, 4 * 128);

		/* The main surface height is 4 tile rows aligned */
		add.offsets[1] = add.pitches[0] * ALIGN(add.height, 128);

		/* CCS surface pitch is 64 bytes aligned which corresponds to
		 * 4 tiles on the main surface
		 */
		add.pitches[1] = DIV_ROUND_UP(add.width, 128) * 64;

		size = add.offsets[1];
		/* CCS surface height is 4 tile rows aligned */
		size += add.pitches[1] * DIV_ROUND_UP(add.height, 128) * 4;

		/* GEM object is page aligned */
		size = ALIGN(size, 4096);
	} else {
		add.modifier[0] = I915_FORMAT_MOD_Y_TILED_CCS;
		add.modifier[1] = I915_FORMAT_MOD_Y_TILED_CCS;

		/* An explanation of the magic numbers can be found in kms_ccs.c. */
		add.pitches[0] = ALIGN(add.width * 4, 128);
		add.offsets[1] = add.pitches[0] * ALIGN(add.height, 32);
		add.pitches[1] = ALIGN(ALIGN(add.width * 4, 32) / 32, 128);

		size = add.offsets[1];
		size += add.pitches[1] * ALIGN(ALIGN(add.height, 16) / 16, 32);
	}

	add.handles[0] = gem_create(fd, size);
	igt_require(add.handles[0] != 0);
	add.handles[1] = add.handles[0];

	if (drmIoctl(fd, DRM_IOCTL_MODE_ADDFB2, &add) == 0)
		*ret = add;
	else
		gem_close(fd, add.handles[0]);
}

/**
 * Find and return an arbitrary valid property ID.
 */
static uint32_t get_any_prop_id(int fd)
{
	igt_display_t display;

	igt_display_require(&display, fd);
	for (int i = 0; i < display.n_outputs; i++) {
		igt_output_t *output = &display.outputs[i];
		if (output->props[IGT_CONNECTOR_DPMS] != 0)
			return output->props[IGT_CONNECTOR_DPMS];
	}

	return 0;
}

static void test_handle_input(int fd)
{
	struct drm_mode_fb_cmd2 add = {};

	igt_fixture {
		add.width = 1024;
		add.height = 1024;
		add.pixel_format = DRM_FORMAT_XRGB8888;
		add.pitches[0] = 1024*4;
		add.handles[0] = igt_create_bo_with_dimensions(fd, 1024, 1024,
			DRM_FORMAT_XRGB8888, 0, 0, NULL, NULL, NULL);
		igt_require(add.handles[0] != 0);
		do_ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &add);
	}

	igt_describe("Tests error handling for a zero'd input.");
	igt_subtest("getfb-handle-zero") {
		struct drm_mode_fb_cmd get = { .fb_id = 0 };
		do_ioctl_err(fd, DRM_IOCTL_MODE_GETFB, &get, ENOENT);
	}

	igt_describe("Tests error handling when passing an valid "
		     "handle.");
	igt_subtest("getfb-handle-valid") {
		struct drm_mode_fb_cmd get = { .fb_id = add.fb_id };
		do_ioctl(fd, DRM_IOCTL_MODE_GETFB, &get);
		igt_assert_neq_u32(get.handle, 0);
		igt_assert_eq_u32(get.width, add.width);
		igt_assert_eq_u32(get.height, add.height);
		igt_assert_eq_u32(get.pitch, add.pitches[0]);
		igt_assert_eq_u32(get.depth, 24);
		igt_assert_eq_u32(get.bpp, 32);
		gem_close(fd, get.handle);
	}

	igt_describe("Tests error handling when passing a handle that "
		     "has been closed.");
	igt_subtest("getfb-handle-closed") {
		struct drm_mode_fb_cmd get = { .fb_id = add.fb_id };
		do_ioctl(fd, DRM_IOCTL_MODE_RMFB, &add.fb_id);
		do_ioctl_err(fd, DRM_IOCTL_MODE_GETFB, &get, ENOENT);
	}

	igt_describe("Tests error handling when passing an invalid "
		     "handle.");
	igt_subtest("getfb-handle-not-fb") {
		struct drm_mode_fb_cmd get = { .fb_id = get_any_prop_id(fd) };
		igt_require(get.fb_id > 0);
		do_ioctl_err(fd, DRM_IOCTL_MODE_GETFB, &get, ENOENT);
	}

	igt_fixture
		gem_close(fd, add.handles[0]);
}

static void test_duplicate_handles(int fd)
{
	struct drm_mode_fb_cmd2 add = {};

	igt_fixture {
		add.width = 1024;
		add.height = 1024;
		add.pixel_format = DRM_FORMAT_XRGB8888;
		add.pitches[0] = 1024*4;
		add.handles[0] = igt_create_bo_with_dimensions(fd, 1024, 1024,
			DRM_FORMAT_XRGB8888, 0, 0, NULL, NULL, NULL);
		igt_assert(add.handles[0]);
		do_ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &add);
	}

	igt_describe("Tests error handling while requesting for two different "
		     "handles from same fd.");
	igt_subtest("getfb-addfb-different-handles") {
		struct drm_mode_fb_cmd get = { .fb_id = add.fb_id };

		do_ioctl(fd, DRM_IOCTL_MODE_GETFB, &get);
		igt_assert_neq_u32(get.handle, add.handles[0]);
		gem_close(fd, get.handle);
	}

	igt_describe("Tests error handling while requesting for two different "
		     "handles from different fd.");
	igt_subtest("getfb-repeated-different-handles") {
		struct drm_mode_fb_cmd get1 = { .fb_id = add.fb_id };
		struct drm_mode_fb_cmd get2 = { .fb_id = add.fb_id };

		do_ioctl(fd, DRM_IOCTL_MODE_GETFB, &get1);
		do_ioctl(fd, DRM_IOCTL_MODE_GETFB, &get2);
		igt_assert_neq_u32(get1.handle, get2.handle);

		gem_close(fd, get1.handle);
		gem_close(fd, get2.handle);
	}

	igt_describe("Tests error handling while requesting CCS buffers "
		     "it should refuse because getfb supports returning "
		     "a single buffer handle.");
	igt_subtest("getfb-reject-ccs") {
		struct drm_mode_fb_cmd2 add_ccs = { };
		struct drm_mode_fb_cmd get = { };

		get_ccs_fb(fd, &add_ccs);
		igt_require(add_ccs.handles[0] != 0);
		get.fb_id = add_ccs.fb_id;
		do_ioctl_err(fd, DRM_IOCTL_MODE_GETFB, &get, EINVAL);

		do_ioctl(fd, DRM_IOCTL_MODE_RMFB, &add_ccs.fb_id);
		gem_close(fd, add_ccs.handles[0]);
	}

	igt_fixture {
		do_ioctl(fd, DRM_IOCTL_MODE_RMFB, &add.fb_id);
		gem_close(fd, add.handles[0]);
	}
}

static void test_getfb2(int fd)
{
	struct drm_mode_fb_cmd2 add_basic = {};

	igt_fixture {
		struct drm_mode_fb_cmd2 get = {};

		add_basic.width = 1024;
		add_basic.height = 1024;
		add_basic.pixel_format = DRM_FORMAT_XRGB8888;
		add_basic.pitches[0] = 1024*4;
		add_basic.handles[0] = igt_create_bo_with_dimensions(fd, 1024, 1024,
			DRM_FORMAT_XRGB8888, 0, 0, NULL, NULL, NULL);
		igt_assert(add_basic.handles[0]);
		do_ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &add_basic);

		get.fb_id = add_basic.fb_id;
		do_ioctl(fd, DRM_IOCTL_MODE_GETFB2, &get);
		igt_assert_neq_u32(get.handles[0], 0);
		gem_close(fd, get.handles[0]);
	}

	igt_describe("Tests error handling for a zero'd input.");
	igt_subtest("getfb2-handle-zero") {
		struct drm_mode_fb_cmd2 get = {};
		do_ioctl_err(fd, DRM_IOCTL_MODE_GETFB2, &get, ENOENT);
	}

	igt_describe("Tests error handling when passing a handle that "
		     "has been closed.");
	igt_subtest("getfb2-handle-closed") {
		struct drm_mode_fb_cmd2 add = add_basic;
		struct drm_mode_fb_cmd2 get = { };

		do_ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &add);
		do_ioctl(fd, DRM_IOCTL_MODE_RMFB, &add.fb_id);

		get.fb_id = add.fb_id;
		do_ioctl_err(fd, DRM_IOCTL_MODE_GETFB2, &get, ENOENT);
	}

	igt_describe("Tests error handling when passing an invalid "
		     "handle.");
	igt_subtest("getfb2-handle-not-fb") {
		struct drm_mode_fb_cmd2 get = { .fb_id = get_any_prop_id(fd) };
		igt_require(get.fb_id > 0);
		do_ioctl_err(fd, DRM_IOCTL_MODE_GETFB2, &get, ENOENT);
	}

	igt_describe("Tests outputs are correct when retrieving a "
		     "CCS framebuffer.");
	igt_subtest("getfb2-accept-ccs") {
		struct drm_mode_fb_cmd2 add_ccs = { };
		struct drm_mode_fb_cmd2 get = { };
		int i;

		get_ccs_fb(fd, &add_ccs);
		igt_require(add_ccs.fb_id != 0);
		get.fb_id = add_ccs.fb_id;
		do_ioctl(fd, DRM_IOCTL_MODE_GETFB2, &get);

		igt_assert_eq_u32(get.width, add_ccs.width);
		igt_assert_eq_u32(get.height, add_ccs.height);
		igt_assert(get.flags & DRM_MODE_FB_MODIFIERS);

		for (i = 0; i < ARRAY_SIZE(get.handles); i++) {
			igt_assert_eq_u32(get.pitches[i], add_ccs.pitches[i]);
			igt_assert_eq_u32(get.offsets[i], add_ccs.offsets[i]);
			if (add_ccs.handles[i] != 0) {
				igt_assert_neq_u32(get.handles[i], 0);
				igt_assert_neq_u32(get.handles[i],
						   add_ccs.handles[i]);
				igt_assert_eq_u64(get.modifier[i],
						  add_ccs.modifier[i]);
			} else {
				igt_assert_eq_u32(get.handles[i], 0);
				igt_assert_eq_u64(get.modifier[i], 0);
			}
		}
		igt_assert_eq_u32(get.handles[0], get.handles[1]);

		do_ioctl(fd, DRM_IOCTL_MODE_RMFB, &get.fb_id);
		gem_close(fd, add_ccs.handles[0]);
		gem_close(fd, get.handles[0]);
	}

	igt_describe("Output check by passing the output of GETFB2 "
		     "into ADDFB2.");
	igt_subtest("getfb2-into-addfb2") {
		struct drm_mode_fb_cmd2 cmd = { };

		cmd.fb_id = add_basic.fb_id;
		do_ioctl(fd, DRM_IOCTL_MODE_GETFB2, &cmd);
		do_ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &cmd);

		do_ioctl(fd, DRM_IOCTL_MODE_RMFB, &cmd.fb_id);
		gem_close(fd, cmd.handles[0]);
	}

	igt_fixture {
		do_ioctl(fd, DRM_IOCTL_MODE_RMFB, &add_basic.fb_id);
		gem_close(fd, add_basic.handles[0]);
	}
}

static void test_handle_protection(void) {
	int non_master_fd;
	struct drm_mode_fb_cmd2 non_master_add = {};

	igt_fixture {
		non_master_fd = drm_open_driver(DRIVER_ANY);

		non_master_add.width = 1024;
		non_master_add.height = 1024;
		non_master_add.pixel_format = DRM_FORMAT_XRGB8888;
		non_master_add.pitches[0] = 1024*4;
		non_master_add.handles[0] = igt_create_bo_with_dimensions(non_master_fd, 1024, 1024,
			DRM_FORMAT_XRGB8888, 0, 0, NULL, NULL, NULL);
		igt_require(non_master_add.handles[0] != 0);
		do_ioctl(non_master_fd, DRM_IOCTL_MODE_ADDFB2, &non_master_add);
	}

	igt_describe("Make sure GETFB doesn't return handles if caller "
		     "is non-root or non-master.");
	igt_subtest("getfb-handle-protection") {
		struct drm_mode_fb_cmd get = { .fb_id = non_master_add.fb_id};

		igt_fork(child, 1) {
			igt_drop_root();

			do_ioctl(non_master_fd, DRM_IOCTL_MODE_GETFB, &get);
			/* ioctl succeeds but handle should be 0 */
			igt_assert_eq_u32(get.handle, 0);
		}
		igt_waitchildren();
	}

	igt_describe("Make sure GETFB2 doesn't return handles if caller "
		     "is non-root or non-master.");
	igt_subtest("getfb2-handle-protection") {
		struct drm_mode_fb_cmd2 get = { .fb_id = non_master_add.fb_id};
		int i;

		igt_fork(child, 1) {
			igt_drop_root();

			do_ioctl(non_master_fd, DRM_IOCTL_MODE_GETFB2, &get);
			/* ioctl succeeds but handles should be 0 */
			for (i = 0; i < ARRAY_SIZE(get.handles); i++) {
				igt_assert_eq_u32(get.handles[i], 0);
			}
		}
		igt_waitchildren();
	}

	igt_fixture {
		do_ioctl(non_master_fd, DRM_IOCTL_MODE_RMFB, &non_master_add.fb_id);
		gem_close(non_master_fd, non_master_add.handles[0]);
	}
}

igt_main
{
	int fd;

	igt_fixture {
		fd = drm_open_driver_master(DRIVER_ANY);
		igt_require(has_getfb_iface(fd));
	}

	igt_subtest_group
		test_handle_input(fd);

	igt_subtest_group
		test_duplicate_handles(fd);

	igt_subtest_group
		test_getfb2(fd);

	igt_subtest_group
		test_handle_protection();

	igt_fixture
		close(fd);
}
