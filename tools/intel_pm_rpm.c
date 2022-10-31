/*
 * Copyright © 2022 Intel Corporation
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

#include <errno.h>
#include <getopt.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "igt.h"
#include "igt_device.h"
#include "igt_device_scan.h"
#include "igt_pm.h"

#define DONT_SET_AUTOSUSPEND_DELAY (1 << 0)
#define SET_I915_AUTOSUSPEND_DELAY (1 << 1)

typedef struct {
	int drm_fd;
	int debugfs_fd;
	drmModeResPtr res;
	igt_display_t display;
} data_t;

const char *help_str =
	"  --disable-display-wait\t\tDisable all screens and try to go into runtime pm.\n"
	"  --force-d3cold-wait\t\tForce dgfx gfx card to enter runtime D3Cold.\n"
	"  --setup-d3cold\t\tEnable gfx card runtime pm and optionally set autosupend delay to"
	"  i915 autosuspend delay. Use --setup-d3cold=i915-auto-delay as optional argument.\n"
	"  --help\t\tProvide help. Provide card name with IGT_DEVICE=drm:/dev/dri/card*.";
static struct option long_options[] = {
	{"disable-display-wait", 0, 0, 'd'},
	{"force-d3cold-wait", 0, 0, 'f'},
	{"setup-d3cold", 2, 0, 's'},
	{"help", 0, 0, 'h'},
	{ 0, 0, 0, 0 }
};

const char *optstr = "dfs::h";

static void usage(const char *name)
{
	igt_info("Usage: %s [options]\n", name);
	igt_info("%s\n", help_str);
}

static void disable_all_displays(data_t *data)
{
	igt_output_t *output;

	for (int i = 0; i < data->display.n_outputs; i++) {
		output = &data->display.outputs[i];
		igt_output_set_pipe(output, PIPE_NONE);
		igt_display_commit(&data->display);
	}
}

static void
setup_gfx_card_d3cold(data_t *data, unsigned char setup_d3cold)
{
	struct pci_device *root, *i915;

	root = igt_device_get_pci_root_port(data->drm_fd);
	if (setup_d3cold == DONT_SET_AUTOSUSPEND_DELAY) {
		igt_pm_enable_pci_card_runtime_pm(root, NULL);
	} else if (setup_d3cold == SET_I915_AUTOSUSPEND_DELAY) {
		i915 = igt_device_get_pci_device(data->drm_fd);
		igt_pm_enable_pci_card_runtime_pm(root, i915);
	}

	igt_info("Enabled pci devs runtime pm under Root port %04x:%02x:%02x.%01x\n",
		 root->domain, root->bus, root->dev, root->func);
}

static void force_gfx_card_d3cold(data_t *data)
{
	struct pci_device *root;
	int d_state;

	root = igt_device_get_pci_root_port(data->drm_fd);

	if (!igt_pm_acpi_d3cold_supported(root)) {
		igt_info("D3Cold isn't supported on Root port %04x:%02x:%02x.%01x\n",
			 root->domain, root->bus, root->dev, root->func);
		return;
	}

	disable_all_displays(data);
	igt_pm_setup_pci_card_runtime_pm(root);
	sleep(1);
	d_state = igt_pm_get_acpi_real_d_state(root);

	if (d_state == IGT_ACPI_D3Cold) {
		igt_info("D3Cold achieved for root port %04x:%02x:%02x.%01x\n",
			 root->domain, root->bus, root->dev, root->func);
	} else {
		igt_pm_print_pci_card_runtime_status();
		igt_info("D3Cold not achieved yet. Please monitor %04x:%02x:%02x.%01x real_power_state\n",
			 root->domain, root->bus, root->dev, root->func);
	}

	igt_info("Hit CTRL-C to exit\n");
	while (1)
		sleep(600);
}

int main(int argc, char *argv[])
{
	bool disable_display = false, force_d3cold = false;
	unsigned char setup_d3cold = 0;
	struct igt_device_card card;
	char *env_device = NULL;
	int c, option_index = 0;
	data_t data = {};
	int ret = 0;

	if (argc <= 1) {
		usage(argv[0]);
		return EXIT_SUCCESS;
	}

	env_device = getenv("IGT_DEVICE");
	igt_devices_scan(false);

	if (env_device) {
		if (!igt_device_card_match(env_device, &card)) {
			igt_warn("No device found for the env_device\n");
			ret = EXIT_FAILURE;
			goto exit;
		}
	} else {
		if (!igt_device_find_first_i915_discrete_card(&card)) {
			igt_warn("No discrete gpu found\n");
			ret = EXIT_FAILURE;
			goto exit;
		}
	}

	while ((c = getopt_long(argc, argv, optstr,
				long_options, &option_index)) != -1) {
		switch (c) {
		case 'd':
			disable_display = true;
			break;
		case 'f':
			force_d3cold = true;
			break;
		case 's':
			if (optarg) {
				if (!strcmp(optarg, "i915-auto-delay")) {
					setup_d3cold = SET_I915_AUTOSUSPEND_DELAY;
				} else	{
					usage(argv[0]);
					ret = EXIT_SUCCESS;
					goto exit;
				}
			} else {
				setup_d3cold = DONT_SET_AUTOSUSPEND_DELAY;
			}
			break;
		default:
		case 'h':
			usage(argv[0]);
			ret = EXIT_SUCCESS;
			goto exit;
		}
	}

	data.drm_fd = igt_open_card(&card);
	if (data.drm_fd  >= 0) {
		igt_info("Device %s successfully opened\n", card.card);
	} else {
		igt_warn("Cannot open card %s device\n", card.card);
		ret = EXIT_FAILURE;
		goto exit;
	}

	data.res = drmModeGetResources(data.drm_fd);
	if (data.res) {
		data.debugfs_fd = igt_debugfs_dir(data.drm_fd);
		kmstest_set_vt_graphics_mode();
		igt_display_require(&data.display, data.drm_fd);

		/* i915 disables RPM in case DMC is not loaded on kms supported cards */
		if (!igt_pm_dmc_loaded(data.debugfs_fd)) {
			igt_warn("dmc fw is not loaded, no runtime pm\n");
			ret = EXIT_FAILURE;
			goto exit;
		}
	}

	if (disable_display) {
		igt_setup_runtime_pm(data.drm_fd);
		disable_all_displays(&data);
		if (!igt_wait_for_pm_status(IGT_RUNTIME_PM_STATUS_SUSPENDED)) {
			__igt_debugfs_dump(data.drm_fd, "i915_runtime_pm_status", IGT_LOG_INFO);
			ret = EXIT_FAILURE;
			goto exit;
		}

		igt_info("Device runtime suspended, Useful for debugging.\n"
			 "Hit CTRL-C to exit\n");
		/* Don't return useful for debugging */
		while (1)
			sleep(600);
	}

	if (force_d3cold)
		force_gfx_card_d3cold(&data);

	if (setup_d3cold)
		setup_gfx_card_d3cold(&data, setup_d3cold);

exit:
	igt_restore_runtime_pm();

	if (data.res)
		igt_display_fini(&data.display);

	if (data.debugfs_fd)
		close(data.debugfs_fd);
	close(data.drm_fd);
	igt_devices_free();

	return ret;
}
