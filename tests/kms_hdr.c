/*
 * Copyright 2019 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "igt.h"
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include "igt_edid.h"

IGT_TEST_DESCRIPTION("Test HDR metadata interfaces and bpc switch");

/* HDR EDID parsing. */
#define CTA_EXTENSION_VERSION		0x03
#define HDR_STATIC_METADATA_BLOCK       0x06
#define USE_EXTENDED_TAG		0x07

/* DRM HDR definitions. Not in the UAPI header, unfortunately. */
enum hdmi_metadata_type {
	HDMI_STATIC_METADATA_TYPE1 = 1,
};

enum hdmi_eotf {
	HDMI_EOTF_TRADITIONAL_GAMMA_SDR,
	HDMI_EOTF_TRADITIONAL_GAMMA_HDR,
	HDMI_EOTF_SMPTE_ST2084,
};

/* Test flags. */
enum {
	TEST_NONE = 1 << 0,
	TEST_DPMS = 1 << 1,
	TEST_SUSPEND = 1 << 2,
	TEST_SWAP = 1 << 3,
};

/* BPC connector state. */
typedef struct output_bpc {
	unsigned int current;
	unsigned int maximum;
} output_bpc_t;

/* Common test data. */
typedef struct data {
	igt_display_t display;
	igt_plane_t *primary;
	igt_output_t *output;
	igt_pipe_t *pipe;
	igt_pipe_crc_t *pipe_crc;
	drmModeModeInfo *mode;
	enum pipe pipe_id;
	int fd;
	int w;
	int h;
} data_t;

/* Common test cleanup. */
static void test_fini(data_t *data)
{
	igt_pipe_crc_free(data->pipe_crc);
	igt_display_reset(&data->display);
}

static void test_cycle_flags(data_t *data, uint32_t test_flags)
{
	if (test_flags & TEST_DPMS) {
		kmstest_set_connector_dpms(data->fd,
					   data->output->config.connector,
					   DRM_MODE_DPMS_OFF);
		kmstest_set_connector_dpms(data->fd,
					   data->output->config.connector,
					   DRM_MODE_DPMS_ON);
	}

	if (test_flags & TEST_SUSPEND)
		igt_system_suspend_autoresume(SUSPEND_STATE_MEM,
					      SUSPEND_TEST_NONE);
}

/* Returns the current and maximum bpc from the connector debugfs. */
static output_bpc_t get_output_bpc(data_t *data)
{
	char buf[256];
	char *start_loc;
	int fd, res;
	output_bpc_t info;

	fd = igt_debugfs_connector_dir(data->fd, data->output->name, O_RDONLY);
	igt_assert(fd >= 0);

	res = igt_debugfs_simple_read(fd, "output_bpc", buf, sizeof(buf));

	igt_require(res > 0);

	close(fd);

	igt_assert(start_loc = strstr(buf, "Current: "));
	igt_assert_eq(sscanf(start_loc, "Current: %u", &info.current), 1);

	igt_assert(start_loc = strstr(buf, "Maximum: "));
	igt_assert_eq(sscanf(start_loc, "Maximum: %u", &info.maximum), 1);

	return info;
}

/* Verifies that connector has the correct output bpc. */
static void assert_output_bpc(data_t *data, unsigned int bpc)
{
	output_bpc_t info = get_output_bpc(data);

	igt_require_f(info.maximum >= bpc,
		      "Monitor doesn't support %u bpc, max is %u\n", bpc,
		      info.maximum);

	igt_assert_eq(info.current, bpc);
}

/* Fills the FB with a test HDR pattern. */
static void draw_hdr_pattern(igt_fb_t *fb)
{
	cairo_t *cr = igt_get_cairo_ctx(fb->fd, fb);

	igt_paint_color(cr, 0, 0, fb->width, fb->height, 1.0, 1.0, 1.0);
	igt_paint_test_pattern(cr, fb->width, fb->height);

	igt_put_cairo_ctx(fb->fd, fb, cr);
}

/* Prepare test data. */
static void prepare_test(data_t *data, igt_output_t *output, enum pipe pipe)
{
	igt_display_t *display = &data->display;

	data->pipe_id = pipe;
	data->pipe = &data->display.pipes[data->pipe_id];
	igt_assert(data->pipe);

	igt_display_reset(display);

	data->output = output;
	igt_assert(data->output);

	data->mode = igt_output_get_mode(data->output);
	igt_assert(data->mode);

	data->primary =
		igt_pipe_get_plane_type(data->pipe, DRM_PLANE_TYPE_PRIMARY);

	data->pipe_crc = igt_pipe_crc_new(data->fd, data->pipe_id,
					  INTEL_PIPE_CRC_SOURCE_AUTO);

	igt_output_set_pipe(data->output, data->pipe_id);

	data->w = data->mode->hdisplay;
	data->h = data->mode->vdisplay;
}

static bool igt_pipe_is_free(igt_display_t *display, enum pipe pipe)
{
	int i;

	for (i = 0; i < display->n_outputs; i++)
		if (display->outputs[i].pending_pipe == pipe)
			return false;

	return true;
}

static void test_bpc_switch_on_output(data_t *data, igt_output_t *output,
				      uint32_t flags)
{
	igt_display_t *display = &data->display;
	igt_crc_t ref_crc, new_crc;
	enum pipe pipe;
	igt_fb_t afb;
	int afb_id, ret;

	for_each_pipe(display, pipe) {
		if (!igt_pipe_connector_valid(pipe, output))
			continue;
		/*
		 * If previous subtest of connector failed, pipe
		 * attached to that connector is not released.
		 * Because of that we have to choose the non
		 * attached pipe for this subtest.
		 */
		if (!igt_pipe_is_free(display, pipe))
			continue;

		prepare_test(data, output, pipe);

		/* 10-bit formats are slow, so limit the size. */
		afb_id = igt_create_fb(data->fd, 512, 512, DRM_FORMAT_XRGB2101010, 0, &afb);
		igt_assert(afb_id);

		draw_hdr_pattern(&afb);

		/* Start in 8bpc. */
		igt_plane_set_fb(data->primary, &afb);
		igt_plane_set_size(data->primary, data->w, data->h);
		igt_output_set_prop_value(data->output, IGT_CONNECTOR_MAX_BPC, 8);
		ret = igt_display_try_commit_atomic(display, DRM_MODE_ATOMIC_TEST_ONLY, NULL);
		if (!ret) {
			data->w = afb.width;
			data->h = afb.height;
		}

		igt_plane_set_fb(data->primary, NULL);

		/*
		 * i915 driver doesn't expose max bpc as debugfs entry,
		 * so limiting assert only for amd driver.
		 */
		if (is_amdgpu_device(data->fd))
			assert_output_bpc(data, 8);

		/* Switch to 10bpc. */
		igt_output_set_prop_value(data->output, IGT_CONNECTOR_MAX_BPC, 10);
		igt_display_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
		if (is_amdgpu_device(data->fd))
			assert_output_bpc(data, 10);

		/* Verify that the CRC are equal after DPMS or suspend. */
		igt_pipe_crc_collect_crc(data->pipe_crc, &ref_crc);
		test_cycle_flags(data, flags);
		igt_pipe_crc_collect_crc(data->pipe_crc, &new_crc);

		/* Drop back to 8bpc. */
		igt_output_set_prop_value(data->output, IGT_CONNECTOR_MAX_BPC, 8);
		igt_display_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
		if (is_amdgpu_device(data->fd))
			assert_output_bpc(data, 8);

		/* CRC capture is clamped to 8bpc, so capture should match. */
		igt_assert_crc_equal(&ref_crc, &new_crc);

		test_fini(data);
		igt_remove_fb(data->fd, &afb);

		/*
		 * Testing a output with a pipe is enough for HDR
		 * testing. No ROI in testing the connector with other
		 * pipes. So break the loop on pipe.
		 */
		break;
	}
}

/* Returns true if an output supports max bpc property. */
static bool has_max_bpc(igt_output_t *output)
{
	return igt_output_has_prop(output, IGT_CONNECTOR_MAX_BPC) &&
	       igt_output_get_prop(output, IGT_CONNECTOR_MAX_BPC);
}

static void test_bpc_switch(data_t *data, uint32_t flags)
{
	igt_output_t *output;
	int valid_tests = 0;

	for_each_connected_output(&data->display, output) {
		if (!has_max_bpc(output))
			continue;

		igt_info("BPC switch test execution on %s\n", output->name);
		test_bpc_switch_on_output(data, output, flags);
		valid_tests++;
	}

	igt_require_f(valid_tests, "No connector found with MAX BPC connector property\n");
}

static bool cta_block(const char *edid_ext)
{
	/*
	 * Byte 1: 0x07 indicates Extended Tag
	 * Byte 2: 0x06 indicates HDMI Static Metadata Block
	 * Byte 3: bits 0 to 5 identify EOTF functions supported by sink
	 *	       where ET_0: Traditional Gamma - SDR Luminance Range
	 *	             ET_1: Traditional Gamma - HDR Luminance Range
	 *	             ET_2: SMPTE ST 2084
	 *	             ET_3: Hybrid Log-Gamma (HLG)
	 *	             ET_4 to ET_5: Reserved for future use
	 */

	if ((((edid_ext[0] & 0xe0) >> 5 == USE_EXTENDED_TAG) &&
	      (edid_ext[1] == HDR_STATIC_METADATA_BLOCK)) &&
	     ((edid_ext[2] & HDMI_EOTF_TRADITIONAL_GAMMA_HDR) ||
	      (edid_ext[2] & HDMI_EOTF_SMPTE_ST2084)))
			return true;

	return false;
}

/* Returns true if panel supports HDR. */
static bool is_panel_hdr(data_t *data, igt_output_t *output)
{
	bool ok;
	int i, j, offset;
	uint64_t edid_blob_id;
	drmModePropertyBlobRes *edid_blob;
	const struct edid_ext *edid_ext;
	const struct edid *edid;
	const struct edid_cea *edid_cea;
	const char *cea_data;
	bool ret = false;

	ok = kmstest_get_property(data->fd, output->id,
			DRM_MODE_OBJECT_CONNECTOR, "EDID",
			NULL, &edid_blob_id, NULL);

	if (!ok || !edid_blob_id)
		return ret;

	edid_blob = drmModeGetPropertyBlob(data->fd, edid_blob_id);
	igt_assert(edid_blob);

	edid = (const struct edid *) edid_blob->data;
	igt_assert(edid);

	drmModeFreePropertyBlob(edid_blob);

	for (i = 0; i < edid->extensions_len; i++) {
		edid_ext = &edid->extensions[i];
		edid_cea = &edid_ext->data.cea;

		/* HDR not defined in CTA Extension Version < 3. */
		if ((edid_ext->tag != EDID_EXT_CEA) ||
		    (edid_cea->revision != CTA_EXTENSION_VERSION))
				continue;
		else {
			offset = edid_cea->dtd_start;
			cea_data = edid_cea->data;

			for (j = 0; j < offset; j += (cea_data[j] & 0x1f) + 1) {
				ret = cta_block(cea_data + j);

				if (ret)
					break;
			}
		}
	}

	return ret;
}

/* Sets the HDR output metadata prop. */
static void set_hdr_output_metadata(data_t *data,
				    struct hdr_output_metadata const *meta)
{
	igt_output_replace_prop_blob(data->output,
				     IGT_CONNECTOR_HDR_OUTPUT_METADATA, meta,
				     meta ? sizeof(*meta) : 0);
}

/* Converts a double to 861-G spec FP format. */
static uint16_t calc_hdr_float(double val)
{
	return (uint16_t)(val * 50000.0);
}

/* Fills some test values for ST2048 HDR output metadata.
 *
 * Note: there isn't really a standard for what the metadata is supposed
 * to do on the display side of things. The display is free to ignore it
 * and clip the output, use it to help tonemap to the content range,
 * or do anything they want, really.
 */
static void fill_hdr_output_metadata_st2048(struct hdr_output_metadata *meta)
{
	memset(meta, 0, sizeof(*meta));

	meta->metadata_type = HDMI_STATIC_METADATA_TYPE1;
	meta->hdmi_metadata_type1.eotf = HDMI_EOTF_SMPTE_ST2084;

	/* Rec. 2020 */
	meta->hdmi_metadata_type1.display_primaries[0].x =
		calc_hdr_float(0.708); /* Red */
	meta->hdmi_metadata_type1.display_primaries[0].y =
		calc_hdr_float(0.292);
	meta->hdmi_metadata_type1.display_primaries[1].x =
		calc_hdr_float(0.170); /* Green */
	meta->hdmi_metadata_type1.display_primaries[1].y =
		calc_hdr_float(0.797);
	meta->hdmi_metadata_type1.display_primaries[2].x =
		calc_hdr_float(0.131); /* Blue */
	meta->hdmi_metadata_type1.display_primaries[2].y =
		calc_hdr_float(0.046);
	meta->hdmi_metadata_type1.white_point.x = calc_hdr_float(0.3127);
	meta->hdmi_metadata_type1.white_point.y = calc_hdr_float(0.3290);

	meta->hdmi_metadata_type1.max_display_mastering_luminance =
		1000; /* 1000 nits */
	meta->hdmi_metadata_type1.min_display_mastering_luminance =
		500;				   /* 0.05 nits */
	meta->hdmi_metadata_type1.max_fall = 1000; /* 1000 nits */
	meta->hdmi_metadata_type1.max_cll = 500;   /* 500 nits */
}

static void test_static_toggle(data_t *data, igt_output_t *output,
			       uint32_t flags)
{
	igt_display_t *display = &data->display;
	struct hdr_output_metadata hdr;
	igt_crc_t ref_crc, new_crc;
	enum pipe pipe;
	igt_fb_t afb;
	int afb_id;

	for_each_pipe(display, pipe) {
		if (!igt_pipe_connector_valid(pipe, output))
			continue;

		if (!igt_pipe_is_free(display, pipe))
			continue;

		prepare_test(data, output, pipe);

		/* 10-bit formats are slow, so limit the size. */
		afb_id = igt_create_fb(data->fd, 512, 512, DRM_FORMAT_XRGB2101010, 0, &afb);
		igt_assert(afb_id);

		draw_hdr_pattern(&afb);

		fill_hdr_output_metadata_st2048(&hdr);

		/* Start with no metadata. */
		igt_plane_set_fb(data->primary, &afb);
		igt_plane_set_size(data->primary, data->w, data->h);
		set_hdr_output_metadata(data, NULL);
		igt_output_set_prop_value(data->output, IGT_CONNECTOR_MAX_BPC, 8);
		igt_display_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
		if (is_amdgpu_device(data->fd))
			assert_output_bpc(data, 8);

		/* Apply HDR metadata and 10bpc. We expect a modeset for entering. */
		set_hdr_output_metadata(data, &hdr);
		igt_output_set_prop_value(data->output, IGT_CONNECTOR_MAX_BPC, 10);
		igt_display_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
		if (is_amdgpu_device(data->fd))
			assert_output_bpc(data, 10);

		/* Verify that the CRC are equal after DPMS or suspend. */
		igt_pipe_crc_collect_crc(data->pipe_crc, &ref_crc);
		test_cycle_flags(data, flags);
		igt_pipe_crc_collect_crc(data->pipe_crc, &new_crc);

		/* Disable HDR metadata and drop back to 8bpc. We expect a modeset for exiting. */
		set_hdr_output_metadata(data, NULL);
		igt_output_set_prop_value(data->output, IGT_CONNECTOR_MAX_BPC, 8);
		igt_display_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
		if (is_amdgpu_device(data->fd))
			assert_output_bpc(data, 8);

		igt_assert_crc_equal(&ref_crc, &new_crc);

		test_fini(data);
		igt_remove_fb(data->fd, &afb);

		break;
	}
}

/* Fills some test values for HDR metadata targeting SDR. */
static void fill_hdr_output_metadata_sdr(struct hdr_output_metadata *meta)
{
	memset(meta, 0, sizeof(*meta));

	meta->metadata_type = HDMI_STATIC_METADATA_TYPE1;
	meta->hdmi_metadata_type1.eotf = HDMI_EOTF_TRADITIONAL_GAMMA_SDR;

	/* Rec. 709 */
	meta->hdmi_metadata_type1.display_primaries[0].x =
		calc_hdr_float(0.640); /* Red */
	meta->hdmi_metadata_type1.display_primaries[0].y =
		calc_hdr_float(0.330);
	meta->hdmi_metadata_type1.display_primaries[1].x =
		calc_hdr_float(0.300); /* Green */
	meta->hdmi_metadata_type1.display_primaries[1].y =
		calc_hdr_float(0.600);
	meta->hdmi_metadata_type1.display_primaries[2].x =
		calc_hdr_float(0.150); /* Blue */
	meta->hdmi_metadata_type1.display_primaries[2].y =
		calc_hdr_float(0.006);
	meta->hdmi_metadata_type1.white_point.x = calc_hdr_float(0.3127);
	meta->hdmi_metadata_type1.white_point.y = calc_hdr_float(0.3290);

	meta->hdmi_metadata_type1.max_display_mastering_luminance = 0;
	meta->hdmi_metadata_type1.min_display_mastering_luminance = 0;
	meta->hdmi_metadata_type1.max_fall = 0;
	meta->hdmi_metadata_type1.max_cll = 0;
}

static void test_static_swap(data_t *data, igt_output_t *output)
{
	igt_display_t *display = &data->display;
	igt_crc_t ref_crc, new_crc;
	enum pipe pipe;
	igt_fb_t afb;
	int afb_id;
	struct hdr_output_metadata hdr;

	for_each_pipe(display, pipe) {
		if (!igt_pipe_connector_valid(pipe, output))
			continue;

		if (!igt_pipe_is_free(display, pipe))
			continue;

		prepare_test(data, output, pipe);

		/* 10-bit formats are slow, so limit the size. */
		afb_id = igt_create_fb(data->fd, 512, 512, DRM_FORMAT_XRGB2101010, 0, &afb);
		igt_assert(afb_id);

		draw_hdr_pattern(&afb);

		/* Start in SDR. */
		igt_plane_set_fb(data->primary, &afb);
		igt_plane_set_size(data->primary, data->w, data->h);
		igt_output_set_prop_value(data->output, IGT_CONNECTOR_MAX_BPC, 8);
		igt_display_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
		if (is_amdgpu_device(data->fd))
			assert_output_bpc(data, 8);

		/* Enter HDR, a modeset is allowed here. */
		fill_hdr_output_metadata_st2048(&hdr);
		set_hdr_output_metadata(data, &hdr);
		igt_output_set_prop_value(data->output, IGT_CONNECTOR_MAX_BPC, 10);
		igt_display_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
		if (is_amdgpu_device(data->fd))
			assert_output_bpc(data, 10);

		igt_pipe_crc_collect_crc(data->pipe_crc, &ref_crc);

		/* Change the mastering information, no modeset allowed
		 * for amd driver, whereas a modeset is required for i915
		 * driver. */
		hdr.hdmi_metadata_type1.max_display_mastering_luminance = 200;
		hdr.hdmi_metadata_type1.max_fall = 200;
		hdr.hdmi_metadata_type1.max_cll = 100;

		set_hdr_output_metadata(data, &hdr);
		if (is_amdgpu_device(data->fd))
			igt_display_commit_atomic(display, 0, NULL);
		else
			igt_display_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);

		/* Enter SDR via metadata, no modeset allowed for
		 * amd driver, whereas a modeset is required for
		 * i915 driver. */
		fill_hdr_output_metadata_sdr(&hdr);
		set_hdr_output_metadata(data, &hdr);
		if (is_amdgpu_device(data->fd))
			igt_display_commit_atomic(display, 0, NULL);
		else
			igt_display_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);

		igt_pipe_crc_collect_crc(data->pipe_crc, &new_crc);

		/* Exit SDR and enter 8bpc, cleanup. */
		set_hdr_output_metadata(data, NULL);
		igt_output_set_prop_value(data->output, IGT_CONNECTOR_MAX_BPC, 8);
		igt_display_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
		if (is_amdgpu_device(data->fd))
			assert_output_bpc(data, 8);

		/* Verify that the CRC didn't change while cycling metadata. */
		igt_assert_crc_equal(&ref_crc, &new_crc);

		test_fini(data);
		igt_remove_fb(data->fd, &afb);

		break;
	}
}

/* Returns true if an output supports HDR metadata property. */
static bool has_hdr(igt_output_t *output)
{
	return igt_output_has_prop(output, IGT_CONNECTOR_HDR_OUTPUT_METADATA);
}

static void test_hdr(data_t *data, const char *test_name, uint32_t flags)
{
	igt_output_t *output;
	int valid_tests = 0;

	for_each_connected_output(&data->display, output) {
		/* To test HDR, 10 bpc is required, so we need to
		 * set MAX_BPC property to 10bpc prior to setting
		 * HDR metadata property. Therefore, checking.
		 */
		if (!has_max_bpc(output))
			continue;

		if (!has_hdr(output))
			continue;

		if (!is_panel_hdr(data, output))
			continue;

		igt_info("HDR %s test execution on %s\n", test_name, output->name);
		if (flags & TEST_NONE || flags & TEST_DPMS || flags & TEST_SUSPEND)
			test_static_toggle(data, output, flags);
		if (flags & TEST_SWAP)
			test_static_swap(data, output);
		valid_tests++;
	}

	igt_require_f(valid_tests, "No connector found with HDR metadata/MAX BPC connector property (or) panel is Non-HDR\n");
}

igt_main
{
	data_t data = { 0 };

	igt_fixture {
		data.fd = drm_open_driver_master(DRIVER_AMDGPU | DRIVER_INTEL);

		kmstest_set_vt_graphics_mode();

		igt_display_require(&data.display, data.fd);
		igt_require(data.display.is_atomic);

		igt_display_require_output(&data.display);
	}

	igt_describe("Tests switching between different display output bpc modes");
	igt_subtest("bpc-switch") test_bpc_switch(&data, TEST_NONE);
	igt_describe("Tests bpc switch with dpms");
	igt_subtest("bpc-switch-dpms") test_bpc_switch(&data, TEST_DPMS);
	igt_describe("Tests bpc switch with suspend");
	igt_subtest("bpc-switch-suspend") test_bpc_switch(&data, TEST_SUSPEND);

	igt_describe("Tests entering and exiting HDR mode");
	igt_subtest("static-toggle") test_hdr(&data, "static-toggle", TEST_NONE);
	igt_describe("Tests static toggle with dpms");
	igt_subtest("static-toggle-dpms") test_hdr(&data, "static-toggle-dpms", TEST_DPMS);
	igt_describe("Tests static toggle with suspend");
	igt_subtest("static-toggle-suspend") test_hdr(&data, "static-toggle-suspend", TEST_SUSPEND);

	igt_describe("Tests swapping static HDR metadata");
	igt_subtest("static-swap") test_hdr(&data, "static-swap", TEST_SWAP);

	igt_fixture {
		igt_display_fini(&data.display);
	}
}
