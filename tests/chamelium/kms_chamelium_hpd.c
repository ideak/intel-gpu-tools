/*
 * Copyright © 2016 Red Hat Inc.
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
 *    Lyude Paul <lyude@redhat.com>
 */

/**
 * TEST: Tests behaviour of hpd using chamelium
 * Category: Display
 */

#include "kms_chamelium_helper.h"

#define HPD_STORM_PULSE_INTERVAL_DP 100 /* ms */
#define HPD_STORM_PULSE_INTERVAL_HDMI 200 /* ms */

#define HPD_TOGGLE_COUNT_VGA 5
#define HPD_TOGGLE_COUNT_DP_HDMI 15
#define HPD_TOGGLE_COUNT_FAST 3

enum test_modeset_mode {
	TEST_MODESET_ON,
	TEST_MODESET_ON_OFF,
	TEST_MODESET_OFF,
};

static void try_suspend_resume_hpd(chamelium_data_t *data,
				   struct chamelium_port *port,
				   enum igt_suspend_state state,
				   enum igt_suspend_test test,
				   struct udev_monitor *mon, bool connected)
{
	drmModeConnection target_state = connected ? DRM_MODE_DISCONNECTED :
						     DRM_MODE_CONNECTED;
	int timeout = CHAMELIUM_HOTPLUG_TIMEOUT;
	int delay;
	int p;

	igt_flush_uevents(mon);

	delay = igt_get_autoresume_delay(state) * 1000 / 2;

	if (port) {
		chamelium_schedule_hpd_toggle(data->chamelium, port, delay,
					      !connected);
	} else {
		for (p = 0; p < data->port_count; p++) {
			port = data->ports[p];
			chamelium_schedule_hpd_toggle(data->chamelium, port,
						      delay, !connected);
		}

		port = NULL;
	}

	igt_system_suspend_autoresume(state, test);
	igt_assert(chamelium_wait_for_hotplug(mon, &timeout));
	chamelium_assert_reachable(data->chamelium, ONLINE_TIMEOUT);

	if (port) {
		igt_assert_eq(chamelium_reprobe_connector(
				      &data->display, data->chamelium, port),
			      target_state);
	} else {
		for (p = 0; p < data->port_count; p++) {
			drmModeConnection current_state;

			port = data->ports[p];
			/*
			 * There could be as many hotplug events sent by
			 * driver as connectors we scheduled an HPD toggle on
			 * above, depending on timing. So if we're not seeing
			 * the expected connector state try to wait for an HPD
			 * event for each connector/port.
			 */
			current_state = chamelium_reprobe_connector(
				&data->display, data->chamelium, port);
			if (p > 0 && current_state != target_state) {
				igt_assert(chamelium_wait_for_hotplug(
					mon, &timeout));
				current_state = chamelium_reprobe_connector(
					&data->display, data->chamelium, port);
			}

			igt_assert_eq(current_state, target_state);
		}

		port = NULL;
	}
}

/**
 * SUBTEST: dp-hpd-fast
 * Description: Check that we get uevents and updated connector status on
 * 		hotplug and unplug
 * Test category: functionality test
 * Run type: BAT
 * Functionality: dp_hotplug
 * Mega feature: DP
 *
 * SUBTEST: hdmi-hpd-fast
 * Description: Check that we get uevents and updated connector status on
 * 		hotplug and unplug
 * Test category: functionality test
 * Run type: BAT
 * Functionality: hdmi_hotplug
 * Mega feature: HDMI
 *
 * SUBTEST: vga-hpd-fast
 * Description: Check that we get uevents and updated connector status on
 * 		hotplug and unplug
 * Test category: functionality test
 * Run type: BAT
 * Functionality: vga_hotplug
 * Mega feature: VGA
 */
static const char test_basic_hotplug_desc[] =
	"Check that we get uevents and updated connector status on "
	"hotplug and unplug";
static void test_hotplug(chamelium_data_t *data, struct chamelium_port *port,
			 int toggle_count, enum test_modeset_mode modeset_mode)
{
	int i;
	enum pipe pipe;
	struct igt_fb fb = { 0 };
	drmModeModeInfo mode;
	struct udev_monitor *mon = igt_watch_uevents();
	igt_output_t *output = chamelium_get_output_for_port(data, port);

	igt_modeset_disable_all_outputs(&data->display);
	chamelium_reset_state(&data->display, data->chamelium, NULL,
			      data->ports, data->port_count);

	igt_hpd_storm_set_threshold(data->drm_fd, 0);

	for (i = 0; i < toggle_count; i++) {
		igt_flush_uevents(mon);

		/* Check if we get a sysfs hotplug event */
		chamelium_plug(data->chamelium, port);

		chamelium_wait_for_connector_after_hotplug(data, mon, port,
							   DRM_MODE_CONNECTED);
		igt_flush_uevents(mon);

		if (modeset_mode == TEST_MODESET_ON_OFF ||
		    (modeset_mode == TEST_MODESET_ON && i == 0)) {
			if (i == 0) {
				/* We can only get mode and pipe once we are
				 * connected */
				output = chamelium_get_output_for_port(data,
								       port);
				pipe = chamelium_get_pipe_for_output(
					&data->display, output);
				mode = chamelium_get_mode_for_port(
					data->chamelium, port);
				chamelium_create_fb_for_mode(data, &fb, &mode);
			}

			igt_output_set_pipe(output, pipe);
			chamelium_enable_output(data, port, output, &mode, &fb);
		}

		/* Now check if we get a hotplug from disconnection */
		chamelium_unplug(data->chamelium, port);

		chamelium_wait_for_connector_after_hotplug(
			data, mon, port, DRM_MODE_DISCONNECTED);

		igt_flush_uevents(mon);

		if (modeset_mode == TEST_MODESET_ON_OFF) {
			igt_output_set_pipe(output, PIPE_NONE);
			igt_display_commit2(&data->display, COMMIT_ATOMIC);
		}
	}

	igt_cleanup_uevents(mon);
	igt_hpd_storm_reset(data->drm_fd);
	igt_remove_fb(data->drm_fd, &fb);
}

static const char test_hotplug_for_each_pipe_desc[] =
	"Check that we get uevents and updated connector status on "
	"hotplug and unplug for each pipe with valid output";
static void test_hotplug_for_each_pipe(chamelium_data_t *data,
				       struct chamelium_port *port)
{
	igt_output_t *output;
	enum pipe pipe;
	struct udev_monitor *mon = igt_watch_uevents();

	chamelium_reset_state(&data->display, data->chamelium, port,
			      data->ports, data->port_count);

	igt_hpd_storm_set_threshold(data->drm_fd, 0);
	/* Disconnect if any port got connected */
	chamelium_unplug(data->chamelium, port);
	chamelium_wait_for_connector_after_hotplug(data, mon, port,
						   DRM_MODE_DISCONNECTED);

	for_each_pipe(&data->display, pipe) {
		igt_modeset_disable_all_outputs(&data->display);
		igt_flush_uevents(mon);
		/* Check if we get a sysfs hotplug event */
		chamelium_plug(data->chamelium, port);
		chamelium_wait_for_connector_after_hotplug(data, mon, port,
							   DRM_MODE_CONNECTED);
		igt_flush_uevents(mon);
		output = chamelium_get_output_for_port(data, port);

		/* If pipe is valid for output then set it */
		if (igt_pipe_connector_valid(pipe, output)) {
			igt_output_set_pipe(output, pipe);
			igt_display_commit2(&data->display, COMMIT_ATOMIC);
		}

		chamelium_unplug(data->chamelium, port);
		chamelium_wait_for_connector_after_hotplug(
			data, mon, port, DRM_MODE_DISCONNECTED);
		igt_flush_uevents(mon);
	}

	igt_cleanup_uevents(mon);
	igt_hpd_storm_reset(data->drm_fd);
}

static const char test_suspend_resume_hpd_desc[] =
	"Toggle HPD during suspend, check that uevents are sent and connector "
	"status is updated";
static void test_suspend_resume_hpd(chamelium_data_t *data,
				    struct chamelium_port *port,
				    enum igt_suspend_state state,
				    enum igt_suspend_test test)
{
	struct udev_monitor *mon = igt_watch_uevents();

	igt_modeset_disable_all_outputs(&data->display);
	chamelium_reset_state(&data->display, data->chamelium, port,
			      data->ports, data->port_count);

	/* Make sure we notice new connectors after resuming */
	try_suspend_resume_hpd(data, port, state, test, mon, false);

	/* Now make sure we notice disconnected connectors after resuming */
	try_suspend_resume_hpd(data, port, state, test, mon, true);

	igt_cleanup_uevents(mon);
}

/**
 * SUBTEST: common-hpd-after-suspend
 * Description: Toggle HPD during suspend on all connectors, check that uevents
 * 		are sent and connector status is updated
 * Test category: functionality test
 * Run type: BAT
 * Functionality: hotplug
 * Mega feature: General Display Features
 */
static const char test_suspend_resume_hpd_common_desc[] =
	"Toggle HPD during suspend on all connectors, check that uevents are "
	"sent and connector status is updated";
static void test_suspend_resume_hpd_common(chamelium_data_t *data,
					   enum igt_suspend_state state,
					   enum igt_suspend_test test)
{
	struct udev_monitor *mon = igt_watch_uevents();
	struct chamelium_port *port;
	int p;

	for (p = 0; p < data->port_count; p++) {
		port = data->ports[p];
		igt_debug("Testing port %s\n", chamelium_port_get_name(port));
	}

	igt_modeset_disable_all_outputs(&data->display);
	chamelium_reset_state(&data->display, data->chamelium, NULL,
			      data->ports, data->port_count);

	/* Make sure we notice new connectors after resuming */
	try_suspend_resume_hpd(data, NULL, state, test, mon, false);

	/* Now make sure we notice disconnected connectors after resuming */
	try_suspend_resume_hpd(data, NULL, state, test, mon, true);

	igt_cleanup_uevents(mon);
}

static const char test_hpd_without_ddc_desc[] =
	"Disable DDC on a VGA connector, check we still get a uevent on hotplug";
static void test_hpd_without_ddc(chamelium_data_t *data,
				 struct chamelium_port *port)
{
	struct udev_monitor *mon = igt_watch_uevents();

	igt_modeset_disable_all_outputs(&data->display);
	chamelium_reset_state(&data->display, data->chamelium, port,
			      data->ports, data->port_count);
	igt_flush_uevents(mon);

	/* Disable the DDC on the connector and make sure we still get a
	 * hotplug
	 */
	chamelium_port_set_ddc_state(data->chamelium, port, false);
	chamelium_plug(data->chamelium, port);

	igt_assert(igt_hotplug_detected(mon, CHAMELIUM_HOTPLUG_TIMEOUT));
	igt_assert_eq(chamelium_reprobe_connector(&data->display,
						  data->chamelium, port),
		      DRM_MODE_CONNECTED);

	igt_cleanup_uevents(mon);
}

static const char test_hpd_storm_detect_desc[] =
	"Trigger a series of hotplugs in a very small timeframe to simulate a"
	"bad cable, check the kernel falls back to polling to avoid a hotplug "
	"storm";
static void test_hpd_storm_detect(chamelium_data_t *data,
				  struct chamelium_port *port, int width)
{
	struct udev_monitor *mon;
	int count = 0;

	igt_require_hpd_storm_ctl(data->drm_fd);
	igt_modeset_disable_all_outputs(&data->display);
	chamelium_reset_state(&data->display, data->chamelium, port,
			      data->ports, data->port_count);

	igt_hpd_storm_set_threshold(data->drm_fd, 1);
	chamelium_fire_hpd_pulses(data->chamelium, port, width, 10);
	igt_assert(igt_hpd_storm_detected(data->drm_fd));

	mon = igt_watch_uevents();
	chamelium_fire_hpd_pulses(data->chamelium, port, width, 10);

	/*
	 * Polling should have been enabled by the HPD storm at this point,
	 * so we should only get at most 1 hotplug event
	 */
	igt_until_timeout(5)
		count += igt_hotplug_detected(mon, 1);
	igt_assert_lt(count, 2);

	igt_cleanup_uevents(mon);
	igt_hpd_storm_reset(data->drm_fd);
}

static const char test_hpd_storm_disable_desc[] =
	"Disable HPD storm detection, trigger a storm and check the kernel "
	"doesn't detect one";
static void test_hpd_storm_disable(chamelium_data_t *data,
				   struct chamelium_port *port, int width)
{
	igt_require_hpd_storm_ctl(data->drm_fd);
	igt_modeset_disable_all_outputs(&data->display);
	chamelium_reset_state(&data->display, data->chamelium, port,
			      data->ports, data->port_count);

	igt_hpd_storm_set_threshold(data->drm_fd, 0);
	chamelium_fire_hpd_pulses(data->chamelium, port, width, 10);
	igt_assert(!igt_hpd_storm_detected(data->drm_fd));

	igt_hpd_storm_reset(data->drm_fd);
}

IGT_TEST_DESCRIPTION("Testing HPD with a Chamelium board");
igt_main
{
	chamelium_data_t data;
	struct chamelium_port *port;
	int p;

	igt_fixture {
		chamelium_init_test(&data);
	}

	igt_describe("DisplayPort tests");
	igt_subtest_group {
		igt_fixture {
			chamelium_require_connector_present(
				data.ports, DRM_MODE_CONNECTOR_DisplayPort,
				data.port_count, 1);
		}

		igt_describe(test_basic_hotplug_desc);
		connector_subtest("dp-hpd", DisplayPort)
			test_hotplug(&data, port, HPD_TOGGLE_COUNT_DP_HDMI,
				     TEST_MODESET_OFF);

		igt_describe(test_basic_hotplug_desc);
		connector_subtest("dp-hpd-fast", DisplayPort) test_hotplug(
			&data, port, HPD_TOGGLE_COUNT_FAST, TEST_MODESET_OFF);

		igt_describe(test_basic_hotplug_desc);
		connector_subtest("dp-hpd-enable-disable-mode", DisplayPort)
			test_hotplug(&data, port, HPD_TOGGLE_COUNT_FAST,
				     TEST_MODESET_ON_OFF);

		igt_describe(test_basic_hotplug_desc);
		connector_subtest("dp-hpd-with-enabled-mode", DisplayPort)
			test_hotplug(&data, port, HPD_TOGGLE_COUNT_FAST,
				     TEST_MODESET_ON);

		igt_describe(test_hotplug_for_each_pipe_desc);
		connector_subtest("dp-hpd-for-each-pipe", DisplayPort)
			test_hotplug_for_each_pipe(&data, port);

		igt_describe(test_suspend_resume_hpd_desc);
		connector_subtest("dp-hpd-after-suspend", DisplayPort)
			test_suspend_resume_hpd(&data, port, SUSPEND_STATE_MEM,
						SUSPEND_TEST_NONE);

		igt_describe(test_suspend_resume_hpd_desc);
		connector_subtest("dp-hpd-after-hibernate", DisplayPort)
			test_suspend_resume_hpd(&data, port, SUSPEND_STATE_DISK,
						SUSPEND_TEST_DEVICES);

		igt_describe(test_hpd_storm_detect_desc);
		connector_subtest("dp-hpd-storm", DisplayPort)
			test_hpd_storm_detect(&data, port,
					      HPD_STORM_PULSE_INTERVAL_DP);

		igt_describe(test_hpd_storm_disable_desc);
		connector_subtest("dp-hpd-storm-disable", DisplayPort)
			test_hpd_storm_disable(&data, port,
					       HPD_STORM_PULSE_INTERVAL_DP);
	}

	igt_describe("HDMI tests");
	igt_subtest_group {
		igt_fixture {
			chamelium_require_connector_present(
				data.ports, DRM_MODE_CONNECTOR_HDMIA,
				data.port_count, 1);
		}

		igt_describe(test_basic_hotplug_desc);
		connector_subtest("hdmi-hpd", HDMIA)
			test_hotplug(&data, port, HPD_TOGGLE_COUNT_DP_HDMI,
				     TEST_MODESET_OFF);

		igt_describe(test_basic_hotplug_desc);
		connector_subtest("hdmi-hpd-fast", HDMIA) test_hotplug(
			&data, port, HPD_TOGGLE_COUNT_FAST, TEST_MODESET_OFF);

		igt_describe(test_basic_hotplug_desc);
		connector_subtest("hdmi-hpd-enable-disable-mode", HDMIA)
			test_hotplug(&data, port, HPD_TOGGLE_COUNT_FAST,
				     TEST_MODESET_ON_OFF);

		igt_describe(test_basic_hotplug_desc);
		connector_subtest("hdmi-hpd-with-enabled-mode", HDMIA)
			test_hotplug(&data, port, HPD_TOGGLE_COUNT_FAST,
				     TEST_MODESET_ON);

		igt_describe(test_hotplug_for_each_pipe_desc);
		connector_subtest("hdmi-hpd-for-each-pipe", HDMIA)
			test_hotplug_for_each_pipe(&data, port);

		igt_describe(test_suspend_resume_hpd_desc);
		connector_subtest("hdmi-hpd-after-suspend", HDMIA)
			test_suspend_resume_hpd(&data, port, SUSPEND_STATE_MEM,
						SUSPEND_TEST_NONE);

		igt_describe(test_suspend_resume_hpd_desc);
		connector_subtest("hdmi-hpd-after-hibernate", HDMIA)
			test_suspend_resume_hpd(&data, port, SUSPEND_STATE_DISK,
						SUSPEND_TEST_DEVICES);

		igt_describe(test_hpd_storm_detect_desc);
		connector_subtest("hdmi-hpd-storm", HDMIA)
			test_hpd_storm_detect(&data, port,
					      HPD_STORM_PULSE_INTERVAL_HDMI);

		igt_describe(test_hpd_storm_disable_desc);
		connector_subtest("hdmi-hpd-storm-disable", HDMIA)
			test_hpd_storm_disable(&data, port,
					       HPD_STORM_PULSE_INTERVAL_HDMI);
	}

	igt_describe("VGA tests");
	igt_subtest_group {
		igt_fixture {
			chamelium_require_connector_present(
				data.ports, DRM_MODE_CONNECTOR_VGA,
				data.port_count, 1);
		}

		igt_describe(test_basic_hotplug_desc);
		connector_subtest("vga-hpd", VGA) test_hotplug(
			&data, port, HPD_TOGGLE_COUNT_VGA, TEST_MODESET_OFF);

		igt_describe(test_basic_hotplug_desc);
		connector_subtest("vga-hpd-fast", VGA) test_hotplug(
			&data, port, HPD_TOGGLE_COUNT_FAST, TEST_MODESET_OFF);

		igt_describe(test_basic_hotplug_desc);
		connector_subtest("vga-hpd-enable-disable-mode", VGA)
			test_hotplug(&data, port, HPD_TOGGLE_COUNT_FAST,
				     TEST_MODESET_ON_OFF);

		igt_describe(test_basic_hotplug_desc);
		connector_subtest("vga-hpd-with-enabled-mode", VGA)
			test_hotplug(&data, port, HPD_TOGGLE_COUNT_FAST,
				     TEST_MODESET_ON);

		igt_describe(test_suspend_resume_hpd_desc);
		connector_subtest("vga-hpd-after-suspend", VGA)
			test_suspend_resume_hpd(&data, port, SUSPEND_STATE_MEM,
						SUSPEND_TEST_NONE);

		igt_describe(test_suspend_resume_hpd_desc);
		connector_subtest("vga-hpd-after-hibernate", VGA)
			test_suspend_resume_hpd(&data, port, SUSPEND_STATE_DISK,
						SUSPEND_TEST_DEVICES);

		igt_describe(test_hpd_without_ddc_desc);
		connector_subtest("vga-hpd-without-ddc", VGA)
			test_hpd_without_ddc(&data, port);
	}

	igt_describe("Tests that operate on all connectors");
	igt_subtest_group {
		igt_fixture {
			igt_require(data.port_count);
		}

		igt_describe(test_suspend_resume_hpd_common_desc);
		igt_subtest("common-hpd-after-suspend")
			test_suspend_resume_hpd_common(&data, SUSPEND_STATE_MEM,
						       SUSPEND_TEST_NONE);

		igt_describe(test_suspend_resume_hpd_common_desc);
		igt_subtest("common-hpd-after-hibernate")
			test_suspend_resume_hpd_common(&data,
						       SUSPEND_STATE_DISK,
						       SUSPEND_TEST_DEVICES);
	}

	igt_describe(test_hotplug_for_each_pipe_desc);
	connector_subtest("vga-hpd-for-each-pipe", VGA)
		test_hotplug_for_each_pipe(&data, port);

	igt_fixture {
		igt_display_fini(&data.display);
		close(data.drm_fd);
	}
}
