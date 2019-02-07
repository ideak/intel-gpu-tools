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

#include <poll.h>
#include <fcntl.h>
#include "igt.h"
#include "igt_sysfs.h"
#include "igt_kms.h"

IGT_TEST_DESCRIPTION("Test content protection (HDCP)");

struct data {
	int drm_fd;
	igt_display_t display;
	struct igt_fb red, green;
} data;


static void flip_handler(int fd, unsigned int sequence, unsigned int tv_sec,
			 unsigned int tv_usec, void *_data)
{
	igt_debug("Flip event received.\n");
}

static int wait_flip_event(void)
{
	int rc;
	drmEventContext evctx;
	struct pollfd pfd;

	evctx.version = 2;
	evctx.vblank_handler = NULL;
	evctx.page_flip_handler = flip_handler;

	pfd.fd = data.drm_fd;
	pfd.events = POLLIN;
	pfd.revents = 0;

	rc = poll(&pfd, 1, 1000);
	switch (rc) {
	case 0:
		igt_info("Poll timeout. 1Sec.\n");
		rc = -ETIMEDOUT;
		break;
	case 1:
		rc = drmHandleEvent(data.drm_fd, &evctx);
		igt_assert_eq(rc, 0);
		rc = 0;
		break;
	default:
		igt_info("Unexpected poll rc %d\n", rc);
		rc = -1;
		break;
	}

	return rc;
}

static bool
wait_for_prop_value(igt_output_t *output, uint64_t expected,
		    uint32_t timeout_mSec)
{
	uint64_t val;
	int i;

	for (i = 0; i < timeout_mSec; i++) {
		val = igt_output_get_prop(output,
					  IGT_CONNECTOR_CONTENT_PROTECTION);
		if (val == expected)
			return true;
		usleep(1000);
	}
	igt_info("prop_value mismatch %" PRId64 " != %" PRId64 "\n",
		 val, expected);

	return false;
}

static void
commit_display_and_wait_for_flip(enum igt_commit_style s)
{
	int ret;
	uint32_t flag;

	if (s == COMMIT_ATOMIC) {
		flag = DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_ALLOW_MODESET;
		igt_display_commit_atomic(&data.display, flag, NULL);

		ret = wait_flip_event();
		igt_assert_f(!ret, "wait_flip_event failed. %d\n", ret);
	} else {
		igt_display_commit2(&data.display, s);

		/* Wait for 50mSec */
		usleep(50 * 1000);
	}
}

static void modeset_with_fb(const enum pipe pipe, igt_output_t *output,
			    enum igt_commit_style s)
{
	igt_display_t *display = &data.display;
	drmModeModeInfo mode;
	igt_plane_t *primary;

	igt_assert(kmstest_get_connector_default_mode(
			display->drm_fd, output->config.connector, &mode));

	igt_output_override_mode(output, &mode);
	igt_output_set_pipe(output, pipe);

	igt_create_color_fb(display->drm_fd, mode.hdisplay, mode.vdisplay,
			    DRM_FORMAT_XRGB8888, LOCAL_DRM_FORMAT_MOD_NONE,
			    1.f, 0.f, 0.f, &data.red);
	igt_create_color_fb(display->drm_fd, mode.hdisplay, mode.vdisplay,
			    DRM_FORMAT_XRGB8888, LOCAL_DRM_FORMAT_MOD_NONE,
			    0.f, 1.f, 0.f, &data.green);

	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
	igt_display_commit2(display, s);
	igt_plane_set_fb(primary, &data.red);

	/* Wait for Flip completion before starting the HDCP authentication */
	commit_display_and_wait_for_flip(s);
}

static bool test_cp_enable(igt_output_t *output, enum igt_commit_style s)
{
	igt_display_t *display = &data.display;
	igt_plane_t *primary;
	bool ret;

	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);

	igt_output_set_prop_value(output,
				  IGT_CONNECTOR_CONTENT_PROTECTION, 1);
	igt_display_commit2(display, s);

	/* Wait for 18000mSec (3 authentication * 6Sec) */
	ret = wait_for_prop_value(output, 2, 18000);
	if (ret) {
		igt_plane_set_fb(primary, &data.green);
		igt_display_commit2(display, s);
	}

	return ret;
}

static void test_cp_disable(igt_output_t *output, enum igt_commit_style s)
{
	igt_display_t *display = &data.display;
	igt_plane_t *primary;
	bool ret;

	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);

	/*
	 * Even on HDCP enable failed scenario, IGT should exit leaving the
	 * "content protection" at "UNDESIRED".
	 */
	igt_output_set_prop_value(output, IGT_CONNECTOR_CONTENT_PROTECTION, 0);
	igt_plane_set_fb(primary, &data.red);
	igt_display_commit2(display, s);

	/* Wait for HDCP to be disabled, before crtc off */
	ret = wait_for_prop_value(output, 0, 1000);
	igt_assert_f(ret, "Content Protection not cleared\n");
}

static void test_cp_enable_with_retry(igt_output_t *output,
				      enum igt_commit_style s, int retry)
{
	bool ret;

	do {
		test_cp_disable(output, s);
		ret = test_cp_enable(output, s);

		if (!ret && --retry)
			igt_debug("Retry (%d/2) ...\n", 3 - retry);
	} while (retry && !ret);
	igt_assert_f(ret, "Content Protection not enabled\n");
}

static bool igt_pipe_is_free(igt_display_t *display, enum pipe pipe)
{
	int i;

	for (i = 0; i < display->n_outputs; i++)
		if (display->outputs[i].pending_pipe == pipe)
			return false;

	return true;
}

static void test_cp_lic(igt_output_t *output)
{
	bool ret;

	/* Wait for 4Secs (min 2 cycles of Link Integrity Check) */
	ret = wait_for_prop_value(output, 1, 4 * 1000);
	igt_assert_f(!ret, "Content Protection LIC Failed\n");
}

static void test_content_protection_on_output(igt_output_t *output,
					      enum igt_commit_style s,
					      bool dpms_test)
{
	igt_display_t *display = &data.display;
	igt_plane_t *primary;
	enum pipe pipe;
	bool ret;

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

		modeset_with_fb(pipe, output, s);
		test_cp_enable_with_retry(output, s, 3);
		test_cp_lic(output);

		if (dpms_test) {
			igt_pipe_set_prop_value(display, pipe,
						IGT_CRTC_ACTIVE, 0);
			igt_display_commit2(display, s);

			igt_pipe_set_prop_value(display, pipe,
						IGT_CRTC_ACTIVE, 1);
			igt_display_commit2(display, s);

			ret = wait_for_prop_value(output, 2, 18000);
			if (!ret)
				test_cp_enable_with_retry(output, s, 2);
		}

		test_cp_disable(output, s);
		primary = igt_output_get_plane_type(output,
						    DRM_PLANE_TYPE_PRIMARY);
		igt_plane_set_fb(primary, NULL);
		igt_output_set_pipe(output, PIPE_NONE);

		/*
		 * Testing a output with a pipe is enough for HDCP
		 * testing. No ROI in testing the connector with other
		 * pipes. So Break the loop on pipe.
		 */
		break;
	}
}

static void __debugfs_read(int fd, const char *param, char *buf, int len)
{
	len = igt_debugfs_simple_read(fd, param, buf, len);
	if (len < 0)
		igt_assert_eq(len, -ENODEV);
}

#define debugfs_read(fd, p, arr) __debugfs_read(fd, p, arr, sizeof(arr))

#define MAX_SINK_HDCP_CAP_BUF_LEN	500
static bool sink_hdcp_capable(igt_output_t *output)
{
	char buf[MAX_SINK_HDCP_CAP_BUF_LEN];
	int fd;

	fd = igt_debugfs_connector_dir(data.drm_fd, output->name, O_RDONLY);
	if (fd < 0)
		return false;

	debugfs_read(fd, "i915_hdcp_sink_capability", buf);
	close(fd);

	igt_debug("Sink capability: %s\n", buf);

	return strstr(buf, "HDCP1.4");
}


static void
test_content_protection(enum igt_commit_style s, bool dpms_test)
{
	igt_display_t *display = &data.display;
	igt_output_t *output;
	int valid_tests = 0;

	for_each_connected_output(display, output) {
		if (!output->props[IGT_CONNECTOR_CONTENT_PROTECTION])
			continue;

		igt_info("CP Test execution on %s\n", output->name);
		if (!sink_hdcp_capable(output)) {
			igt_info("\tSkip %s (Sink has no HDCP support)\n",
				 output->name);
			continue;
		}

		test_content_protection_on_output(output, s, dpms_test);
		valid_tests++;
	}

	igt_require_f(valid_tests, "No connector found with HDCP capability\n");
}

igt_main
{
	igt_fixture {
		igt_skip_on_simulation();

		data.drm_fd = drm_open_driver(DRIVER_ANY);

		igt_display_require(&data.display, data.drm_fd);
	}

	igt_subtest("legacy")
		test_content_protection(COMMIT_LEGACY, false);

	igt_subtest("atomic") {
		igt_require(data.display.is_atomic);
		test_content_protection(COMMIT_ATOMIC, false);
	}

	igt_subtest("atomic-dpms") {
		igt_require(data.display.is_atomic);
		test_content_protection(COMMIT_ATOMIC, true);
	}

	igt_fixture
		igt_display_fini(&data.display);
}
