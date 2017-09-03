/*
 * Copyright © 2017 Intel Corporation
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

#include <dirent.h>
#include "igt.h"

#define HDISPLAY_4K	3840
#define VDISPLAY_4K	2160

IGT_TEST_DESCRIPTION("Tests 4K and audio HDMI injection.");

static drmModeConnector *
get_connector(int drm_fd, drmModeRes *res)
{
	int i;
	drmModeConnector *connector;

	for (i = 0; i < res->count_connectors; i++) {

		connector =
			drmModeGetConnectorCurrent(drm_fd, res->connectors[i]);

		if (connector->connector_type == DRM_MODE_CONNECTOR_HDMIA &&
		    connector->connection == DRM_MODE_DISCONNECTED)
			break;

		drmModeFreeConnector(connector);
		connector = NULL;
	}

	return connector;
}

static void
hdmi_inject_4k(int drm_fd, drmModeConnector *connector)
{
	unsigned char *edid;
	size_t length;
	struct kmstest_connector_config config;
	int ret, cid, i, crtc_mask = -1;
	int fb_id;
	struct igt_fb fb;
	uint8_t found_4k_mode = 0;
	uint32_t devid;

	devid = intel_get_drm_devid(drm_fd);

	/* 4K requires at least HSW */
	igt_require(IS_HASWELL(devid) || intel_gen(devid) >= 8);

	kmstest_edid_add_4k(igt_kms_get_base_edid(), EDID_LENGTH, &edid,
			    &length);

	kmstest_force_edid(drm_fd, connector, edid, length);

	if (!kmstest_force_connector(drm_fd, connector, FORCE_CONNECTOR_ON))
		igt_skip("Could not force connector on\n");

	cid = connector->connector_id;

	connector = drmModeGetConnectorCurrent(drm_fd, cid);

	for (i = 0; i < connector->count_modes; i++) {
		if (connector->modes[i].hdisplay == HDISPLAY_4K &&
		    connector->modes[i].vdisplay == VDISPLAY_4K) {
			found_4k_mode++;
			break;
		}
	}

	igt_assert(found_4k_mode);

	/* create a configuration */
	ret = kmstest_get_connector_config(drm_fd, cid, crtc_mask, &config);
	igt_assert(ret);

	igt_info("  ");
	kmstest_dump_mode(&connector->modes[i]);

	/* create framebuffer */
	fb_id = igt_create_fb(drm_fd, connector->modes[i].hdisplay,
			      connector->modes[i].vdisplay,
			      DRM_FORMAT_XRGB8888,
			      LOCAL_DRM_FORMAT_MOD_NONE, &fb);

	ret = drmModeSetCrtc(drm_fd, config.crtc->crtc_id, fb_id, 0, 0,
			     &connector->connector_id, 1,
			     &connector->modes[i]);

	igt_assert(ret == 0);

	igt_remove_fb(drm_fd, &fb);

	kmstest_force_connector(drm_fd, connector, FORCE_CONNECTOR_UNSPECIFIED);
	kmstest_force_edid(drm_fd, connector, NULL, 0);

	free(edid);
}

static bool
eld_entry_is_igt(const char* path)
{
	FILE *in;
	char buf[1024];
	uint8_t eld_valid = 0;
	uint8_t mon_valid = 0;

	in = fopen(path, "r");
	if (!in)
		return false;

	memset(buf, 0, 1024);

	while ((fgets(buf, 1024, in)) != NULL) {

		char *line = buf;

		if (!strncasecmp(line, "eld_valid", 9) &&
				strstr(line, "1")) {
			eld_valid++;
		}

		if (!strncasecmp(line, "monitor_name", 12) &&
				strstr(line, "IGT")) {
			mon_valid++;
		}
	}

	fclose(in);
	if (mon_valid && eld_valid)
		return true;

	return false;
}

static bool
eld_is_valid(void)
{
	DIR *dir;
	struct dirent *snd_hda;
	int i;

	for (i = 0; i < 8; i++) {
		char cards[128];

		snprintf(cards, sizeof(cards), "/proc/asound/card%d", i);
		dir = opendir(cards);
		if (!dir)
			continue;

		while ((snd_hda = readdir(dir))) {
			char fpath[PATH_MAX];

			if (*snd_hda->d_name == '.' ||
			    strstr(snd_hda->d_name, "eld") == 0)
				continue;

			snprintf(fpath, sizeof(fpath), "%s/%s", cards,
				 snd_hda->d_name);
			if (eld_entry_is_igt(fpath)) {
				closedir(dir);
				return true;
			}
		}
		closedir(dir);
	}

	return false;
}

static void
hdmi_inject_audio(int drm_fd, drmModeConnector *connector)
{
	unsigned char *edid;
	size_t length;
	int fb_id, cid, ret, crtc_mask = -1;
	struct igt_fb fb;
	struct kmstest_connector_config config;

	kmstest_edid_add_audio(igt_kms_get_base_edid(), EDID_LENGTH, &edid,
			       &length);

	kmstest_force_edid(drm_fd, connector, edid, length);

	if (!kmstest_force_connector(drm_fd, connector, FORCE_CONNECTOR_ON))
		igt_skip("Could not force connector on\n");

	cid = connector->connector_id;
	connector = drmModeGetConnectorCurrent(drm_fd, cid);

	/* create a configuration */
	ret = kmstest_get_connector_config(drm_fd, cid, crtc_mask, &config);
	igt_assert(ret);

	/*
	 * Create a framebuffer as to allow the kernel to enable the pipe and
	 * enable the audio encoder.
	 */
	fb_id = igt_create_fb(drm_fd, connector->modes[0].hdisplay,
			      connector->modes[0].vdisplay,
			      DRM_FORMAT_XRGB8888,
			      LOCAL_DRM_FORMAT_MOD_NONE, &fb);

	ret = drmModeSetCrtc(drm_fd, config.crtc->crtc_id, fb_id, 0, 0,
			     &connector->connector_id, 1,
			     &connector->modes[0]);


	igt_assert(ret == 0);

	/*
	 * Test if we have /proc/asound/HDMI/eld#0.0 and is its contents are
	 * valid.
	 */
	igt_assert(eld_is_valid());

	igt_remove_fb(drm_fd, &fb);

	igt_info("  ");
	kmstest_dump_mode(&connector->modes[0]);

	kmstest_force_connector(drm_fd, connector, FORCE_CONNECTOR_UNSPECIFIED);
	kmstest_force_edid(drm_fd, connector, NULL, 0);

	free(edid);
}

igt_main
{
	int drm_fd;
	drmModeRes *res;
	drmModeConnector *connector;

	igt_fixture {
		drm_fd = drm_open_driver_master(DRIVER_INTEL);
		res = drmModeGetResources(drm_fd);

		connector = get_connector(drm_fd, res);
		igt_require(connector);
	}

	igt_subtest("inject-4k")
		hdmi_inject_4k(drm_fd, connector);

	igt_subtest("inject-audio")
		hdmi_inject_audio(drm_fd, connector);

	igt_fixture {
		drmModeFreeConnector(connector);
	}
}
