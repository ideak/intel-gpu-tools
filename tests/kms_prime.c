/*
 * Copyright © 2019 Intel Corporation
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

#include "igt.h"
#include "igt_device.h"
#include "igt_debugfs.h"
#include "igt_sysfs.h"
#include <fcntl.h>

#include <sys/ioctl.h>
#include <sys/poll.h>
#include <time.h>

#define KMS_HELPER "/sys/module/drm_kms_helper/parameters/"
#define KMS_POLL_DISABLE 0

bool kms_poll_saved_state;
bool kms_poll_disabled;

struct dumb_bo {
	uint32_t handle;
	uint32_t width, height;
	uint32_t bpp, pitch;
	uint64_t size;
};

struct crc_info {
	igt_crc_t crc;
	char *str;
	const char *name;
};

static struct {
	double r, g, b;
	uint32_t color;
	struct crc_info prime_crc, direct_crc;
} colors[3] = {
	{ .r = 0.0, .g = 0.0, .b = 0.0, .color = 0xff000000 },
	{ .r = 1.0, .g = 1.0, .b = 1.0, .color = 0xffffffff },
	{ .r = 1.0, .g = 0.0, .b = 0.0, .color = 0xffff0000 },
};

IGT_TEST_DESCRIPTION("Prime tests, focusing on KMS side");

static bool has_prime_import(int fd)
{
	uint64_t value;

	if (drmGetCap(fd, DRM_CAP_PRIME, &value))
		return false;

	return value & DRM_PRIME_CAP_IMPORT;
}

static bool has_prime_export(int fd)
{
	uint64_t value;

	if (drmGetCap(fd, DRM_CAP_PRIME, &value))
		return false;

	return value & DRM_PRIME_CAP_EXPORT;
}

static igt_output_t *setup_display(int importer_fd, igt_display_t *display,
				   enum pipe *pipe)
{
	igt_output_t *output;
	bool found = false;

	for_each_pipe_with_valid_output(display, *pipe, output) {
		found = true;
		break;
	}

	igt_require_f(found, "No valid connector/pipe found\n");

	igt_display_reset(display);
	igt_output_set_pipe(output, *pipe);
	return output;
}

static void prepare_scratch(int exporter_fd, struct dumb_bo *scratch,
			    drmModeModeInfo *mode, uint32_t color)
{
	uint32_t *ptr;

	scratch->width = mode->hdisplay;
	scratch->height = mode->vdisplay;
	scratch->bpp = 32;

	if (!is_i915_device(exporter_fd)) {
		scratch->handle = kmstest_dumb_create(exporter_fd,
						      ALIGN(scratch->width, 256),
						      scratch->height, scratch->bpp,
						      &scratch->pitch, &scratch->size);

		ptr = kmstest_dumb_map_buffer(exporter_fd, scratch->handle,
					      scratch->size, PROT_WRITE);
	} else {
		igt_calc_fb_size(exporter_fd, mode->hdisplay, mode->vdisplay, DRM_FORMAT_XRGB8888,
				 DRM_FORMAT_MOD_NONE, &scratch->size, &scratch->pitch);
		if (gem_has_lmem(exporter_fd))
			scratch->handle = gem_create_in_memory_regions(exporter_fd, scratch->size,
								       REGION_LMEM(0), REGION_SMEM);
		else
			scratch->handle = gem_create_in_memory_regions(exporter_fd, scratch->size,
								       REGION_SMEM);

		ptr = gem_mmap__device_coherent(exporter_fd, scratch->handle, 0, scratch->size,
						PROT_WRITE | PROT_READ);
	}

	for (size_t idx = 0; idx < scratch->size / sizeof(*ptr); ++idx)
		ptr[idx] = color;

	munmap(ptr, scratch->size);
}

static void prepare_fb(int importer_fd, struct dumb_bo *scratch, struct igt_fb *fb)
{
	enum igt_color_encoding color_encoding = IGT_COLOR_YCBCR_BT709;
	enum igt_color_range color_range = IGT_COLOR_YCBCR_LIMITED_RANGE;

	igt_init_fb(fb, importer_fd, scratch->width, scratch->height,
		    DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR,
		    color_encoding, color_range);
}

static void import_fb(int importer_fd, struct igt_fb *fb,
		      int dmabuf_fd, uint32_t pitch)
{
	uint32_t offsets[4] = {}, pitches[4] = {}, handles[4] = {}, temp_buf_handle;
	int ret;

	if (is_i915_device(importer_fd)) {
		if (gem_has_lmem(importer_fd)) {
			uint64_t ahnd = get_reloc_ahnd(importer_fd, 0);
			uint64_t fb_size = 0;

			igt_info("Importer is dGPU\n");
			temp_buf_handle = prime_fd_to_handle(importer_fd, dmabuf_fd);
			igt_assert(temp_buf_handle > 0);
			fb->gem_handle = igt_create_bo_with_dimensions(importer_fd, fb->width, fb->height,
								       fb->drm_format, fb->modifier, pitch, &fb_size, NULL, NULL);
			igt_assert(fb->gem_handle > 0);

			igt_blitter_src_copy(importer_fd, ahnd, 0, NULL, temp_buf_handle,
					     0, pitch, fb->modifier, 0, 0, fb_size, fb->width,
					     fb->height, 32, fb->gem_handle, 0, pitch, fb->modifier,
					     0, 0, fb_size);

			gem_sync(importer_fd, fb->gem_handle);
			gem_close(importer_fd, temp_buf_handle);
			put_ahnd(ahnd);
		} else {
			fb->gem_handle = prime_fd_to_handle(importer_fd, dmabuf_fd);
		}
	} else {
		fb->gem_handle = prime_fd_to_handle(importer_fd, dmabuf_fd);
	}

	handles[0] = fb->gem_handle;
	pitches[0] = pitch;
	offsets[0] = 0;

	ret = drmModeAddFB2(importer_fd, fb->width, fb->height,
			    DRM_FORMAT_XRGB8888,
			    handles, pitches, offsets,
			    &fb->fb_id, 0);
	igt_assert(ret == 0);
}

static void set_fb(struct igt_fb *fb,
		   igt_display_t *display,
		   igt_output_t *output)
{
	igt_plane_t *primary;
	int ret;

	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
	igt_assert(primary);

	igt_plane_set_fb(primary, fb);
	ret = igt_display_commit(display);

	igt_assert(ret == 0);
}

static void collect_crc_for_fb(int importer_fd, struct igt_fb *fb, igt_display_t *display,
			       igt_output_t *output, igt_pipe_crc_t *pipe_crc,
			       uint32_t color, struct crc_info *info)
{
	set_fb(fb, display, output);
	igt_pipe_crc_collect_crc(pipe_crc, &info->crc);
	info->str = igt_crc_to_string(&info->crc);
	igt_debug("CRC through '%s' method for %#08x is %s\n",
		  info->name, color, info->str);
	igt_remove_fb(importer_fd, fb);
}

static void test_crc(int exporter_fd, int importer_fd)
{
	igt_display_t display;
	igt_output_t *output;
	igt_pipe_crc_t *pipe_crc;
	enum pipe pipe;
	struct igt_fb fb;
	int dmabuf_fd;
	struct dumb_bo scratch = {};
	bool crc_equal;
	int i, j;
	drmModeModeInfo *mode;

	igt_device_set_master(importer_fd);
	igt_require_pipe_crc(importer_fd);
	igt_display_require(&display, importer_fd);

	output = setup_display(importer_fd, &display, &pipe);

	mode = igt_output_get_mode(output);
	pipe_crc = igt_pipe_crc_new(importer_fd, pipe,
				    IGT_PIPE_CRC_SOURCE_AUTO);

	for (i = 0; i < ARRAY_SIZE(colors); i++) {
		prepare_scratch(exporter_fd, &scratch, mode, colors[i].color);
		dmabuf_fd = prime_handle_to_fd(exporter_fd, scratch.handle);
		gem_close(exporter_fd, scratch.handle);

		prepare_fb(importer_fd, &scratch, &fb);
		import_fb(importer_fd, &fb, dmabuf_fd, scratch.pitch);
		close(dmabuf_fd);

		colors[i].prime_crc.name = "prime";
		collect_crc_for_fb(importer_fd, &fb, &display, output,
				   pipe_crc, colors[i].color, &colors[i].prime_crc);
		igt_create_color_fb(importer_fd,
				    mode->hdisplay, mode->vdisplay,
				    DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR,
				    colors[i].r, colors[i].g, colors[i].b,
				    &fb);

		colors[i].direct_crc.name = "direct";
		collect_crc_for_fb(importer_fd, &fb, &display, output,
				   pipe_crc, colors[i].color, &colors[i].direct_crc);
	}
	igt_pipe_crc_free(pipe_crc);

	igt_debug("CRC table:\n");
	igt_debug("Color\t\tPrime\t\tDirect\n");
	for (i = 0; i < ARRAY_SIZE(colors); i++) {
		igt_debug("%#08x\t%.8s\t%.8s\n", colors[i].color,
			  colors[i].prime_crc.str, colors[i].direct_crc.str);
		free(colors[i].prime_crc.str);
		free(colors[i].direct_crc.str);
	}

	for (i = 0; i < ARRAY_SIZE(colors); i++) {
		for (j = 0; j < ARRAY_SIZE(colors); j++) {
			if (i == j) {
				igt_assert_crc_equal(&colors[i].prime_crc.crc,
						     &colors[j].direct_crc.crc);
				continue;
			}
			crc_equal = igt_check_crc_equal(&colors[i].prime_crc.crc,
							&colors[j].direct_crc.crc);
			igt_assert_f(!crc_equal, "CRC should be different");
		}
	}
	igt_display_fini(&display);
}

static void test_basic_modeset(int drm_fd)
{
	igt_display_t display;
	igt_output_t *output;
	enum pipe pipe;
	drmModeModeInfo *mode;
	struct igt_fb fb;

	igt_device_set_master(drm_fd);
	igt_display_require(&display, drm_fd);

	output = setup_display(drm_fd, &display, &pipe);
	mode = igt_output_get_mode(output);
	igt_assert(mode);

	igt_create_pattern_fb(drm_fd, mode->hdisplay, mode->vdisplay, DRM_FORMAT_XRGB8888,
			      DRM_FORMAT_MOD_LINEAR, &fb);

	set_fb(&fb, &display, output);
	igt_remove_fb(drm_fd, &fb);
	igt_display_fini(&display);
}

static bool has_connected_output(int drm_fd)
{
	igt_display_t display;
	igt_output_t *output;

	igt_device_set_master(drm_fd);
	igt_display_require(&display, drm_fd);

	for_each_connected_output(&display, output)
		return true;

	return false;
}

static void validate_d3_hot(int drm_fd)
{
	igt_assert(igt_debugfs_search(drm_fd, "i915_runtime_pm_status", "GPU idle: yes"));
	igt_assert(igt_debugfs_search(drm_fd, "i915_runtime_pm_status", "PCI device power state: D3hot [3]"));
}

static void kms_poll_state_restore(void)
{
	int sysfs_fd;

	igt_assert((sysfs_fd = open(KMS_HELPER, O_RDONLY)) >= 0);
	igt_sysfs_set_boolean(sysfs_fd, "poll", kms_poll_saved_state);
	close(sysfs_fd);

}

static void kms_poll_disable(void)
{
	int sysfs_fd;

	igt_require((sysfs_fd = open(KMS_HELPER, O_RDONLY)) >= 0);
	kms_poll_saved_state = igt_sysfs_get_boolean(sysfs_fd, "poll");
	igt_sysfs_set_boolean(sysfs_fd, "poll", KMS_POLL_DISABLE);
	kms_poll_disabled = true;
	close(sysfs_fd);
}

igt_main
{
	int first_fd = -1;
	int second_fd_vgem = -1;
	int second_fd_hybrid = -1;
	bool first_output, second_output;

	igt_fixture {
		kmstest_set_vt_graphics_mode();
		/* ANY = anything that is not VGEM */
		first_fd = __drm_open_driver_another(0, DRIVER_ANY);
		igt_require(first_fd >= 0);
		first_output = has_connected_output(first_fd);
	}

	igt_describe("Hybrid GPU subtests");
	igt_subtest_group {
		igt_fixture {
			second_fd_hybrid = __drm_open_driver_another(1, DRIVER_ANY);
			igt_require(second_fd_hybrid >= 0);
			second_output = has_connected_output(second_fd_hybrid);
		}

		igt_describe("Hybrid GPU: Make a dumb color buffer, export to another device and"
			     " compare the CRCs with a buffer native to that device");
		igt_subtest_with_dynamic("basic-crc-hybrid") {
			if (has_prime_export(first_fd) &&
			    has_prime_import(second_fd_hybrid) && second_output)
				igt_dynamic("first-to-second")
					test_crc(first_fd, second_fd_hybrid);

			if (has_prime_import(first_fd) &&
			    has_prime_export(second_fd_hybrid) && first_output)
				igt_dynamic("second-to-first")
					test_crc(second_fd_hybrid, first_fd);
		}

		igt_describe("Basic modeset on the one device when the other device is active");
		igt_subtest_with_dynamic("basic-modeset-hybrid") {
			igt_require(second_fd_hybrid >= 0);
			if (first_output) {
				igt_dynamic("first")
					test_basic_modeset(first_fd);
			}

			if (second_output) {
				igt_dynamic("second")
					test_basic_modeset(second_fd_hybrid);
			}
		}

		igt_describe("Validate pci state of dGPU when dGPU is idle and  scanout is on iGPU");
		igt_subtest("D3hot") {
			igt_require_f(is_i915_device(second_fd_hybrid), "i915 device required\n");
			igt_require_f(gem_has_lmem(second_fd_hybrid), "Second GPU is not dGPU\n");
			igt_require_f(first_output, "No display connected to iGPU\n");
			igt_require_f(!second_output, "Display connected to dGPU\n");

			kms_poll_disable();

			igt_set_timeout(10, "Wait for dGPU to enter D3hot before starting the subtest");
			while (!igt_debugfs_search(second_fd_hybrid,
			       "i915_runtime_pm_status",
			       "PCI device power state: D3hot [3]"));
			igt_reset_timeout();

			test_basic_modeset(first_fd);
			validate_d3_hot(second_fd_hybrid);
		}

		igt_fixture {
			if (kms_poll_disabled)
				kms_poll_state_restore();

			close(second_fd_hybrid);
		}
	}

	igt_describe("VGEM subtests");
	igt_subtest_group {
		igt_fixture {
			second_fd_vgem = __drm_open_driver_another(1, DRIVER_VGEM);
			igt_require(second_fd_vgem >= 0);
			if (is_i915_device(first_fd))
				igt_require(!gem_has_lmem(first_fd));
		}

		igt_describe("Make a dumb color buffer, export to another device and"
			     " compare the CRCs with a buffer native to that device");
		igt_subtest_with_dynamic("basic-crc-vgem") {
			if (has_prime_import(first_fd) &&
			    has_prime_export(second_fd_vgem) && first_output)
				igt_dynamic("second-to-first")
					test_crc(second_fd_vgem, first_fd);
		}

		igt_fixture
			close(second_fd_vgem);
	}

	igt_fixture
		close(first_fd);
}
