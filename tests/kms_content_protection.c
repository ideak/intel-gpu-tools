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
#include <sys/epoll.h>
#include <sys/stat.h>
#include <libudev.h>
#include "igt.h"
#include "igt_sysfs.h"
#include "igt_kms.h"
#include "igt_kmod.h"

IGT_TEST_DESCRIPTION("Test content protection (HDCP)");

struct data {
	int drm_fd;
	igt_display_t display;
	struct igt_fb red, green;
	unsigned int cp_tests;
	struct udev_monitor *uevent_monitor;
} data;

/* Test flags */
#define CP_DPMS					(1 << 0)
#define CP_LIC					(1 << 1)
#define CP_MEI_RELOAD				(1 << 2)
#define CP_TYPE_CHANGE				(1 << 3)
#define CP_UEVENT				(1 << 4)

#define CP_UNDESIRED				0
#define CP_DESIRED				1
#define CP_ENABLED				2

/*
 * HDCP_CONTENT_TYPE_0 can be handled on both HDCP1.4 and HDCP2.2. Where as
 * HDCP_CONTENT_TYPE_1 can be handled only through HDCP2.2.
 */
#define HDCP_CONTENT_TYPE_0				0
#define HDCP_CONTENT_TYPE_1				1

#define LIC_PERIOD_MSEC				(4 * 1000)
/* Kernel retry count=3, Max time per authentication allowed = 6Sec */
#define KERNEL_AUTH_TIME_ALLOWED_MSEC		(3 *  6 * 1000)
#define KERNEL_DISABLE_TIME_ALLOWED_MSEC	(1 * 1000)
#define FLIP_EVENT_POLLING_TIMEOUT_MSEC		1000

__u8 facsimile_srm[] = {
	0x80, 0x0, 0x0, 0x05, 0x01, 0x0, 0x0, 0x36, 0x02, 0x51, 0x1E, 0xF2,
	0x1A, 0xCD, 0xE7, 0x26, 0x97, 0xF4, 0x01, 0x97, 0x10, 0x19, 0x92, 0x53,
	0xE9, 0xF0, 0x59, 0x95, 0xA3, 0x7A, 0x3B, 0xFE, 0xE0, 0x9C, 0x76, 0xDD,
	0x83, 0xAA, 0xC2, 0x5B, 0x24, 0xB3, 0x36, 0x84, 0x94, 0x75, 0x34, 0xDB,
	0x10, 0x9E, 0x3B, 0x23, 0x13, 0xD8, 0x7A, 0xC2, 0x30, 0x79, 0x84};

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

	rc = poll(&pfd, 1, FLIP_EVENT_POLLING_TIMEOUT_MSEC);
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

	if (data.cp_tests & CP_UEVENT && expected != CP_UNDESIRED) {
		igt_assert_f(igt_connector_event_detected(data.uevent_monitor,
							  output->id,
			     output->props[IGT_CONNECTOR_CONTENT_PROTECTION],
			     timeout_mSec / 1000), "uevent is not received");

		val = igt_output_get_prop(output,
					  IGT_CONNECTOR_CONTENT_PROTECTION);
		if (val == expected)
			return true;
	} else {
		for (i = 0; i < timeout_mSec; i++) {
			val = igt_output_get_prop(output,
						  IGT_CONNECTOR_CONTENT_PROTECTION);
			if (val == expected)
				return true;
			usleep(1000);
		}
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

	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
	igt_display_commit2(display, s);
	igt_plane_set_fb(primary, &data.red);
	igt_fb_set_size(&data.red, primary, mode.hdisplay, mode.vdisplay);

	/* Wait for Flip completion before starting the HDCP authentication */
	commit_display_and_wait_for_flip(s);
}

static bool test_cp_enable(igt_output_t *output, enum igt_commit_style s,
			   int content_type, bool type_change)
{
	igt_display_t *display = &data.display;
	igt_plane_t *primary;
	bool ret;

	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);

	if (!type_change)
		igt_output_set_prop_value(output,
					  IGT_CONNECTOR_CONTENT_PROTECTION,
					  CP_DESIRED);

	if (output->props[IGT_CONNECTOR_HDCP_CONTENT_TYPE])
		igt_output_set_prop_value(output,
					  IGT_CONNECTOR_HDCP_CONTENT_TYPE,
					  content_type);
	igt_display_commit2(display, s);

	ret = wait_for_prop_value(output, CP_ENABLED,
				  KERNEL_AUTH_TIME_ALLOWED_MSEC);
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
	igt_output_set_prop_value(output, IGT_CONNECTOR_CONTENT_PROTECTION,
				  CP_UNDESIRED);
	igt_plane_set_fb(primary, &data.red);
	igt_display_commit2(display, s);

	/* Wait for HDCP to be disabled, before crtc off */
	ret = wait_for_prop_value(output, CP_UNDESIRED,
				  KERNEL_DISABLE_TIME_ALLOWED_MSEC);
	igt_assert_f(ret, "Content Protection not cleared\n");
}

static void test_cp_enable_with_retry(igt_output_t *output,
				      enum igt_commit_style s, int retry,
				      int content_type, bool expect_failure,
				      bool type_change)
{
	int retry_orig = retry;
	bool ret;

	do {
		if (!type_change || retry_orig != retry)
			test_cp_disable(output, s);

		ret = test_cp_enable(output, s, content_type, type_change);

		if (!ret && --retry)
			igt_debug("Retry (%d/2) ...\n", 3 - retry);
	} while (retry && !ret);

	if (!ret)
		test_cp_disable(output, s);

	if (expect_failure)
		igt_assert_f(!ret,
			     "CP Enabled. Though it is expected to fail\n");
	else
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
	ret = wait_for_prop_value(output, CP_DESIRED, LIC_PERIOD_MSEC);
	igt_assert_f(!ret, "Content Protection LIC Failed\n");
}

static bool write_srm_as_fw(const __u8 *srm, int len)
{
	int fd, ret, total = 0;

	fd = open("/lib/firmware/display_hdcp_srm.bin",
		  O_WRONLY | O_CREAT, S_IRWXU);
	do {
		ret = write(fd, srm + total, len - total);
		if (ret < 0)
			ret = -errno;
		if (ret == -EINTR || ret == -EAGAIN)
			continue;
		if (ret <= 0)
			break;
		total += ret;
	} while (total != len);
	close(fd);

	return total < len ? false : true;
}

static void test_content_protection_on_output(igt_output_t *output,
					      enum igt_commit_style s,
					      int content_type)
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
		test_cp_enable_with_retry(output, s, 3, content_type, false,
					  false);

		if (data.cp_tests & CP_TYPE_CHANGE) {
			/* Type 1 -> Type 0 */
			test_cp_enable_with_retry(output, s, 3,
						  HDCP_CONTENT_TYPE_0, false,
						  true);
			/* Type 0 -> Type 1 */
			test_cp_enable_with_retry(output, s, 3,
						  content_type, false,
						  true);
		}

		if (data.cp_tests & CP_MEI_RELOAD) {
			igt_assert_f(!igt_kmod_unload("mei_hdcp", 0),
				     "mei_hdcp unload failed");

			/* Expected to fail */
			test_cp_enable_with_retry(output, s, 3,
						  content_type, true, false);

			igt_assert_f(!igt_kmod_load("mei_hdcp", NULL),
				     "mei_hdcp load failed");

			/* Expected to pass */
			test_cp_enable_with_retry(output, s, 3,
						  content_type, false, false);
		}

		if (data.cp_tests & CP_LIC)
			test_cp_lic(output);

		if (data.cp_tests & CP_DPMS) {
			igt_pipe_set_prop_value(display, pipe,
						IGT_CRTC_ACTIVE, 0);
			igt_display_commit2(display, s);

			igt_pipe_set_prop_value(display, pipe,
						IGT_CRTC_ACTIVE, 1);
			igt_display_commit2(display, s);

			ret = wait_for_prop_value(output, CP_ENABLED,
						  KERNEL_AUTH_TIME_ALLOWED_MSEC);
			if (!ret)
				test_cp_enable_with_retry(output, s, 2,
							  content_type, false,
							  false);
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

#define MAX_SINK_HDCP_CAP_BUF_LEN	5000

static bool sink_hdcp_capable(igt_output_t *output)
{
	char buf[MAX_SINK_HDCP_CAP_BUF_LEN];
	int fd;

	fd = igt_debugfs_connector_dir(data.drm_fd, output->name, O_RDONLY);
	if (fd < 0)
		return false;

	if (is_i915_device(data.drm_fd))
		debugfs_read(fd, "i915_hdcp_sink_capability", buf);
	else
		debugfs_read(fd, "hdcp_sink_capability", buf);

	close(fd);

	igt_debug("Sink capability: %s\n", buf);

	return strstr(buf, "HDCP1.4");
}

static bool sink_hdcp2_capable(igt_output_t *output)
{
	char buf[MAX_SINK_HDCP_CAP_BUF_LEN];
	int fd;

	fd = igt_debugfs_connector_dir(data.drm_fd, output->name, O_RDONLY);
	if (fd < 0)
		return false;

	if (is_i915_device(data.drm_fd))
		debugfs_read(fd, "i915_hdcp_sink_capability", buf);
	else
		debugfs_read(fd, "hdcp_sink_capability", buf);

	close(fd);

	igt_debug("Sink capability: %s\n", buf);

	return strstr(buf, "HDCP2.2");
}

static void prepare_modeset_on_mst_output(igt_output_t *output, enum pipe pipe)
{
	drmModeConnectorPtr c = output->config.connector;
	drmModeModeInfo *mode;
	igt_plane_t *primary;
	int i, width, height;

	mode = igt_output_get_mode(output);

	/*
	 * TODO: Add logic to use the highest possible modes on each output.
	 * Currently using 2k modes by default on all the outputs.
	 */
	igt_debug("Before mode override: Output %s Mode hdisplay %d Mode vdisplay %d\n",
		   output->name, mode->hdisplay, mode->vdisplay);

	if (mode->hdisplay > 1920 && mode->vdisplay > 1080) {
		for (i = 0; i < c->count_modes; i++) {
			if (c->modes[i].hdisplay <= 1920 && c->modes[i].vdisplay <= 1080) {
				mode = &c->modes[i];
				igt_output_override_mode(output, mode);
				break;
			}
		}
	}

	igt_debug("After mode overide: Output %s Mode hdisplay %d Mode vdisplay %d\n",
		   output->name, mode->hdisplay, mode->vdisplay);

	width = mode->hdisplay;
	height = mode->vdisplay;

	igt_output_set_pipe(output, pipe);
	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
	igt_plane_set_fb(primary, NULL);
	igt_plane_set_fb(primary, pipe % 2 ? &data.red : &data.green);
	igt_fb_set_size(pipe % 2 ? &data.red : &data.green, primary, width, height);
	igt_plane_set_size(primary, width, height);
}

static bool output_hdcp_capable(igt_output_t *output, int content_type)
{
		if (!output->props[IGT_CONNECTOR_CONTENT_PROTECTION])
			return false;

		if (!output->props[IGT_CONNECTOR_HDCP_CONTENT_TYPE] &&
		    content_type)
			return false;

		if (content_type && !sink_hdcp2_capable(output)) {
			igt_info("\tSkip %s (Sink has no HDCP2.2 support)\n",
				 output->name);
			return false;
		} else if (!sink_hdcp_capable(output)) {
			igt_info("\tSkip %s (Sink has no HDCP support)\n",
				 output->name);
			return false;
		}

		return true;
}

static void
test_content_protection(enum igt_commit_style s, int content_type)
{
	igt_display_t *display = &data.display;
	igt_output_t *output;
	int valid_tests = 0;

	if (data.cp_tests & CP_MEI_RELOAD)
		igt_require_f(igt_kmod_is_loaded("mei_hdcp"),
			      "mei_hdcp module is not loaded\n");

	for_each_connected_output(display, output) {
		if (!output_hdcp_capable(output, content_type))
			continue;

		test_content_protection_on_output(output, s, content_type);
		valid_tests++;
	}

	igt_require_f(valid_tests, "No connector found with HDCP capability\n");
}

static int parse_path_blob(char *blob_data)
{
	int connector_id;
	char *encoder;

	encoder = strtok(blob_data, ":");
	igt_assert_f(!strcmp(encoder, "mst"), "PATH connector property expected to have 'mst'\n");

	connector_id = atoi(strtok(NULL, "-"));

	return connector_id;
}

static bool output_is_dp_mst(igt_output_t *output, int i)
{
	drmModePropertyBlobPtr path_blob = NULL;
	uint64_t path_blob_id;
	drmModeConnector *connector = output->config.connector;
	struct kmstest_connector_config config;
	const char *encoder;
	int connector_id;
	static int prev_connector_id;

	kmstest_get_connector_config(data.drm_fd, output->config.connector->connector_id, -1, &config);
	encoder = kmstest_encoder_type_str(config.encoder->encoder_type);

	if (strcmp(encoder, "DP MST"))
		return false;

	igt_assert(kmstest_get_property(data.drm_fd, connector->connector_id,
		   DRM_MODE_OBJECT_CONNECTOR, "PATH", NULL,
		   &path_blob_id, NULL));

	igt_assert(path_blob = drmModeGetPropertyBlob(data.drm_fd, path_blob_id));

	connector_id = parse_path_blob((char *) path_blob->data);

	/*
	 * Discarding outputs of other DP MST topology.
	 * Testing only on outputs on the topology we got previously
	 */
	if (i == 0) {
		prev_connector_id = connector_id;
	} else {
		if (connector_id != prev_connector_id)
			return false;
	}

	drmModeFreePropertyBlob(path_blob);

	return true;
}

static void test_cp_lic_on_mst(igt_output_t *mst_outputs[], int valid_outputs, bool first_output)
{
	int ret, count;
	uint64_t val;

	/* Only wait for the first output, this optimizes the test execution time */
	ret = wait_for_prop_value(mst_outputs[first_output], CP_DESIRED, LIC_PERIOD_MSEC);
	igt_assert_f(!ret, "Content Protection LIC Failed on %s\n", mst_outputs[0]->name);

	for (count = first_output + 1; count < valid_outputs; count++) {
		val = igt_output_get_prop(mst_outputs[count], IGT_CONNECTOR_CONTENT_PROTECTION);
		igt_assert_f(val != CP_DESIRED, "Content Protection LIC Failed on %s\n", mst_outputs[count]->name);
	}
}

static void
test_content_protection_mst(int content_type)
{
	igt_display_t *display = &data.display;
	igt_output_t *output;
	int valid_outputs = 0, dp_mst_outputs = 0, ret, count, max_pipe = 0, i;
	enum pipe pipe;
	igt_output_t *mst_output[IGT_MAX_PIPES], *hdcp_mst_output[IGT_MAX_PIPES];

	for_each_pipe(display, pipe)
		max_pipe++;

	pipe = PIPE_A;

	for_each_connected_output(display, output) {
		if (!output_is_dp_mst(output, dp_mst_outputs))
			continue;

		igt_assert_f(igt_pipe_connector_valid(pipe, output), "Output-pipe combination invalid\n");

		prepare_modeset_on_mst_output(output, pipe);
		mst_output[dp_mst_outputs++] = output;

		pipe++;

		if (pipe > max_pipe)
			break;
	}

	igt_require_f(dp_mst_outputs > 1, "No DP MST set up with >= 2 outputs found in a single topology\n");

	ret = igt_display_try_commit2(display, COMMIT_ATOMIC);
	igt_require_f(ret == 0, "Commit failure during MST modeset\n");

	for (count = 0; count < dp_mst_outputs; count++) {
		if (!output_hdcp_capable(mst_output[count], content_type))
			continue;

		hdcp_mst_output[valid_outputs++] = mst_output[count];
	}

	igt_require_f(valid_outputs > 1, "DP MST outputs do not have the required HDCP support\n");

	for (count = 0; count < valid_outputs; count++) {
		igt_output_set_prop_value(hdcp_mst_output[count], IGT_CONNECTOR_CONTENT_PROTECTION, CP_DESIRED);

		if (output->props[IGT_CONNECTOR_HDCP_CONTENT_TYPE])
			igt_output_set_prop_value(hdcp_mst_output[count], IGT_CONNECTOR_HDCP_CONTENT_TYPE, content_type);
	}

	igt_display_commit2(display, COMMIT_ATOMIC);

	for (count = 0; count < valid_outputs; count++) {
		ret = wait_for_prop_value(hdcp_mst_output[count], CP_ENABLED, KERNEL_AUTH_TIME_ALLOWED_MSEC);
		igt_assert_f(ret, "Content Protection not enabled on %s\n", hdcp_mst_output[count]->name);
	}

	if (data.cp_tests & CP_LIC)
		test_cp_lic_on_mst(hdcp_mst_output, valid_outputs, 0);

	/*
	 * Verify if CP is still enabled on other outputs by disabling CP on the first output.
	 */
	igt_debug("CP Prop being UNDESIRED on %s\n", hdcp_mst_output[0]->name);
	test_cp_disable(hdcp_mst_output[0], COMMIT_ATOMIC);

	/* CP is expected to be still enabled on other outputs*/
	for (i = 1; i < valid_outputs; i++) {
		/* Wait for the timeout to verify CP is not disabled */
		ret = wait_for_prop_value(hdcp_mst_output[i], CP_UNDESIRED, KERNEL_DISABLE_TIME_ALLOWED_MSEC);
		igt_assert_f(!ret, "Content Protection not enabled on %s\n", hdcp_mst_output[i]->name);
	}

	if (data.cp_tests & CP_LIC)
		test_cp_lic_on_mst(hdcp_mst_output, valid_outputs, 1);
}

static void test_content_protection_cleanup(void)
{
	igt_display_t *display = &data.display;
	igt_output_t *output;
	uint64_t val;

	for_each_connected_output(display, output) {
		if (!output->props[IGT_CONNECTOR_CONTENT_PROTECTION])
			continue;

		val = igt_output_get_prop(output,
					  IGT_CONNECTOR_CONTENT_PROTECTION);
		if (val == CP_UNDESIRED)
			continue;

		igt_info("CP Prop being UNDESIRED on %s\n", output->name);
		test_cp_disable(output, COMMIT_ATOMIC);
	}

	igt_remove_fb(data.drm_fd, &data.red);
	igt_remove_fb(data.drm_fd, &data.green);
}

static void create_fbs(void)
{
	igt_output_t *output;
	int width = 0, height = 0;
	drmModeModeInfo *mode;

	for_each_connected_output(&data.display, output) {
		mode = igt_output_get_mode(output);
		igt_assert(mode);

		width = max(width, mode->hdisplay);
		height = max(height, mode->vdisplay);
	}

	igt_create_color_fb(data.drm_fd, width, height,
			    DRM_FORMAT_XRGB8888, LOCAL_DRM_FORMAT_MOD_NONE,
			    1.f, 0.f, 0.f, &data.red);
	igt_create_color_fb(data.drm_fd, width, height,
			    DRM_FORMAT_XRGB8888, LOCAL_DRM_FORMAT_MOD_NONE,
			    0.f, 1.f, 0.f, &data.green);
}

igt_main
{
	igt_fixture {
		data.drm_fd = drm_open_driver_master(DRIVER_ANY);
		igt_display_require(&data.display, data.drm_fd);
		igt_display_require_output(&data.display);
		create_fbs();
	}

	igt_subtest("legacy") {
		data.cp_tests = 0;
		test_content_protection(COMMIT_LEGACY, HDCP_CONTENT_TYPE_0);
	}

	igt_subtest("atomic") {
		igt_require(data.display.is_atomic);
		data.cp_tests = 0;
		test_content_protection(COMMIT_ATOMIC, HDCP_CONTENT_TYPE_0);
	}

	igt_subtest("atomic-dpms") {
		igt_require(data.display.is_atomic);
		data.cp_tests = CP_DPMS;
		test_content_protection(COMMIT_ATOMIC, HDCP_CONTENT_TYPE_0);
	}

	igt_subtest("LIC") {
		igt_require(data.display.is_atomic);
		data.cp_tests = CP_LIC;
		test_content_protection(COMMIT_ATOMIC, HDCP_CONTENT_TYPE_0);
	}

	igt_subtest("type1") {
		igt_require(data.display.is_atomic);
		test_content_protection(COMMIT_ATOMIC, HDCP_CONTENT_TYPE_1);
	}

	igt_subtest("mei_interface") {
		igt_require(data.display.is_atomic);
		data.cp_tests = CP_MEI_RELOAD;
		test_content_protection(COMMIT_ATOMIC, HDCP_CONTENT_TYPE_1);
	}

	igt_subtest("content_type_change") {
		igt_require(data.display.is_atomic);
		data.cp_tests = CP_TYPE_CHANGE;
		test_content_protection(COMMIT_ATOMIC, HDCP_CONTENT_TYPE_1);
	}

	igt_subtest("uevent") {
		igt_require(data.display.is_atomic);
		data.cp_tests = CP_UEVENT;
		data.uevent_monitor = igt_watch_uevents();
		igt_flush_uevents(data.uevent_monitor);
		test_content_protection(COMMIT_ATOMIC, HDCP_CONTENT_TYPE_0);
		igt_cleanup_uevents(data.uevent_monitor);
	}

	/*
	 *  Testing the revocation check through SRM needs a HDCP sink with
	 *  programmable Ksvs or we need a uAPI from kernel to read the
	 *  connected HDCP sink's Ksv. With that we would be able to add that
	 *  Ksv into a SRM and send in for revocation check. Since we dont have
	 *  either of these options, we test SRM writing from userspace and
	 *  validation of the same at kernel. Something is better than nothing.
	 */
	igt_subtest("srm") {
		bool ret;

		igt_require(data.display.is_atomic);
		data.cp_tests = 0;
		ret = write_srm_as_fw((const __u8 *)facsimile_srm,
				      sizeof(facsimile_srm));
		igt_assert_f(ret, "SRM update failed");
		test_content_protection(COMMIT_ATOMIC, HDCP_CONTENT_TYPE_0);
	}

	igt_describe("Test Content protection over DP MST");
	igt_subtest("dp-mst-type-0") {
		test_content_protection_mst(HDCP_CONTENT_TYPE_0);
	}

	igt_describe("Test Content protection over DP MST with LIC");
	igt_subtest("dp-mst-lic-type-0") {
		data.cp_tests = CP_LIC;
		test_content_protection_mst(HDCP_CONTENT_TYPE_0);
	}

	igt_describe("Test Content protection over DP MST");
	igt_subtest("dp-mst-type-1") {
		test_content_protection_mst(HDCP_CONTENT_TYPE_1);
	}

	igt_describe("Test Content protection over DP MST with LIC");
	igt_subtest("dp-mst-lic-type-1") {
		data.cp_tests = CP_LIC;
		test_content_protection_mst(HDCP_CONTENT_TYPE_1);
	}

	igt_fixture {
		test_content_protection_cleanup();
		igt_display_fini(&data.display);
	}
}
