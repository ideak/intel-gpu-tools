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

/* Common test data */
typedef struct data {
	struct igt_fb pattern_fb_info;
	int fd;
	igt_display_t display;
	igt_plane_t *primary;
	igt_output_t *output;
	igt_pipe_t *pipe;
	enum pipe pipe_id;
	bool use_virtual_connector;
} data_t;

/* Video modes indexed by VIC */
static drmModeModeInfo test_modes[] = {
	[1] = { 25175,
		640, 656, 752, 800, 0,
		480, 489, 492, 525, 0,
		60, 0xa, 0x40,
		"640x480",	/* VIC 1 */
	},
	[96] = { 594000,
		 3840, 4896, 4984, 5280, 0,
		 2160, 2168, 2178, 2250, 0,
		 50, 0x5|DRM_MODE_FLAG_PIC_AR_16_9, 0x40,
		 "3840x2160",	/* VIC 96 */
	},
	[97] = { 594000,
		 3840, 4016, 4104, 4400, 0,
		 2160, 2168, 2178, 2250, 0,
		 60, 0x5|DRM_MODE_FLAG_PIC_AR_16_9, 0x40,
		 "3840x2160",	/* VIC 97 */
	},
	[101] = { 594000,
		  4096, 5064, 5152, 5280, 0,
		  2160, 2168, 2178, 2250, 0,
		  50, 0x5|DRM_MODE_FLAG_PIC_AR_256_135, 0x40,
		  "4096x2160",	/* VIC 101 */
	},
	[102] = { 594000,
		  4096, 4184, 4272, 4400, 0,
		  2160, 2168, 2178, 2250, 0,
		  60, 0x5|DRM_MODE_FLAG_PIC_AR_256_135, 0x40,
		  "4096x2160",	/* VIC 102 */
	},
	[106] = { 594000,
		  3840, 4896, 4984, 5280, 0,
		  2160, 2168, 2178, 2250, 0,
		  50, 0x5|DRM_MODE_FLAG_PIC_AR_64_27, 0x40,
		  "3840x2160",	/* VIC 106 */
	},
	[107] = { 594000,
		  3840, 4016, 4104, 4400, 0,
		  2160, 2168, 2178, 2250, 0,
		  60, 0x5|DRM_MODE_FLAG_PIC_AR_64_27, 0x40,
		  "3840x2160",	/* VIC 107 */
	},
};

/* Common test setup. */
static void test_init(data_t *data)
{
	igt_display_t *display = &data->display;

	data->pipe_id = PIPE_A;
	data->pipe = &data->display.pipes[data->pipe_id];

	igt_display_reset(display);

	/* find a connected HDMI output */
	data->output = NULL;
	for (int i=0; i < data->display.n_outputs; ++i) {
		drmModeConnector *connector = data->display.outputs[i].config.connector;
		if (connector->connection == DRM_MODE_CONNECTED &&
				(connector->connector_type == DRM_MODE_CONNECTOR_HDMIA ||
				 (data->use_virtual_connector &&
				  connector->connector_type == DRM_MODE_CONNECTOR_VIRTUAL))) {
			data->output = &data->display.outputs[i];
		}
	}

	igt_require(data->output);

	data->primary =
		igt_pipe_get_plane_type(data->pipe, DRM_PLANE_TYPE_PRIMARY);

	igt_output_set_pipe(data->output, data->pipe_id);

}

/* Common test cleanup. */
static void test_fini(data_t *data)
{
	igt_display_reset(&data->display);
}

static void wait_for_keypress(void)
{
	while (getchar() != '\n')
		;
}

static void test_vic_mode(data_t *data, int vic)
{
	igt_display_t *display = &data->display;
	drmModeModeInfo *mode;
	igt_fb_t afb;

	test_init(data);

	mode = &test_modes[vic];

	igt_output_override_mode(data->output, mode);

	igt_create_pattern_fb(data->fd, mode->hdisplay, mode->vdisplay, DRM_FORMAT_XRGB8888, 0, &afb);

	igt_plane_set_fb(data->primary, &afb);

	igt_display_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);

	igt_info("Press [Enter] to finish\n");
	wait_for_keypress();

	test_fini(data);
}

const char *optstr = "hvt:";
static void usage(const char *name)
{
	igt_info("Usage: %s [options]\n", name);
	igt_info("-h      Show help\n");
	igt_info("-t vic  Select video mode based on VIC\n");
	igt_info("-v      Test on 'Virtual' connector as well, for debugging.\n");
}

int main(int argc, char **argv)
{
	data_t data;
	int c;
	int vic = 1; /* default to VIC 1 (640x480) */

	memset(&data, 0, sizeof(data));

	while((c = getopt(argc, argv, optstr)) != -1) {
		switch(c) {
		case 't':
			vic = atoi(optarg);
			break;
		case 'v':
			data.use_virtual_connector = true;
			break;
		default:
		case 'h':
			usage(argv[0]);
			exit(EXIT_SUCCESS);
		}
	}

	if (vic < 1 ||
		vic > ARRAY_SIZE(test_modes) ||
		!test_modes[vic].name[0]) {
		igt_warn("VIC %d is not supported\n", vic);
		exit(EXIT_FAILURE);
	}

	data.fd = drm_open_driver_master(DRIVER_ANY);
	kmstest_set_vt_graphics_mode();

	igt_display_require(&data.display, data.fd);
	igt_require(data.display.is_atomic);
	igt_display_require_output(&data.display);

	test_vic_mode(&data, vic);

	igt_display_fini(&data.display);
}
