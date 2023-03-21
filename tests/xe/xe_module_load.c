// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023 Intel Corporation
 */

/**
 * TEST: Tests the xe module loading
 * Category: Sofware building block
 * Sub-category: driver
 * Test category: functionality test
 */

#include <dirent.h>
#include <fcntl.h>
#include <libgen.h>

#ifdef __linux__
#include <linux/limits.h>
#endif

#include <signal.h>

#include <sys/ioctl.h>
#include <sys/utsname.h>

#include "igt.h"

#include "igt_aux.h"
#include "igt_core.h"
#include "igt_debugfs.h"
#include "igt_kmod.h"
#include "igt_sysfs.h"

#define BAR_SIZE_SHIFT 20
#define MIN_BAR_SIZE 256

static void file_write(const char *ptr, size_t size, size_t nmemb,
                       FILE *fp)
{
	int count;

	count = fwrite(ptr, size, nmemb, fp);
	if (count == size * nmemb)
		return;

	igt_debug("Can't update hda dynamic debug with : %s\n", ptr);
}

static void hda_dynamic_debug(bool enable)
{
	FILE *fp;
	static const char snd_hda_intel_on[] = "module snd_hda_intel +pf";
	static const char snd_hda_core_on[] = "module snd_hda_core +pf";

	static const char snd_hda_intel_off[] = "module snd_hda_intel =_";
	static const char snd_hda_core_off[] = "module snd_hda_core =_";

	fp = fopen("/sys/kernel/debug/dynamic_debug/control", "w");
	if (!fp) {
		igt_debug("hda dynamic debug not available\n");
		return;
	}

	if (enable) {
		file_write(snd_hda_intel_on, 1, sizeof(snd_hda_intel_on), fp);
		file_write(snd_hda_core_on, 1, sizeof(snd_hda_core_on), fp);
	} else {
		file_write(snd_hda_intel_off, 1, sizeof(snd_hda_intel_off), fp);
		file_write(snd_hda_core_off, 1, sizeof(snd_hda_core_off), fp);
	}

	fclose(fp);
}

static void load_and_check_xe(const char *opts)
{
	int error;
	int drm_fd;

	hda_dynamic_debug(true);
	error = igt_xe_driver_load(opts);
	hda_dynamic_debug(false);

	igt_assert_eq(error, 0);

	/* driver is ready, check if it's bound */
	drm_fd = __drm_open_driver(DRIVER_XE);
	igt_fail_on_f(drm_fd < 0, "Cannot open the xe DRM driver after modprobing xe.\n");
	close(drm_fd);
}

static const char * const unwanted_drivers[] = {
	"xe",
	"i915",
	NULL
};

/**
 * SUBTEST: force-load
 * Description: Load the Xe driver passing ``force_probe=*`` parameter
 * Run type: BAT
 *
 * SUBTEST: load
 * Description: Load the Xe driver
 * Run type: FULL
 *
 * SUBTEST: unload
 * Description: Unload the Xe driver
 * Run type: FULL
 *
 * SUBTEST: reload
 * Description: Reload the Xe driver
 * Run type: FULL
 *
 * SUBTEST: reload-no-display
 * Description: Reload the Xe driver passing ``enable_display=0`` parameter
 * Run type: FULL
 *
 * SUBTEST: many-reload
 * Description: Reload the Xe driver many times
 * Run type: FULL
 */
igt_main
{
	igt_describe("Check if xe and friends are not yet loaded, then load them.");
	igt_subtest("load") {
		for (int i = 0; unwanted_drivers[i] != NULL; i++) {
			igt_skip_on_f(igt_kmod_is_loaded(unwanted_drivers[i]),
				      "%s is already loaded\n", unwanted_drivers[i]);
		}

		load_and_check_xe(NULL);
	}

	igt_subtest("unload") {
		igt_xe_driver_unload();
	}

	igt_subtest("force-load") {
		for (int i = 0; unwanted_drivers[i] != NULL; i++) {
			igt_skip_on_f(igt_kmod_is_loaded(unwanted_drivers[i]),
				      "%s is already loaded\n", unwanted_drivers[i]);
		}

		load_and_check_xe("force_probe=*");
	}

	igt_subtest("reload-no-display") {
		igt_xe_driver_unload();
		load_and_check_xe("enable_display=0");
	}

	igt_subtest("many-reload") {
		int i;

		for (i = 0; i < 10; i++) {
			igt_debug("reload cycle: %d\n", i);
			igt_xe_driver_unload();
			load_and_check_xe(NULL);
			sleep(1);
		}
	}

	igt_subtest("reload") {
		igt_xe_driver_unload();
		load_and_check_xe(NULL);

		/* only default modparams, can leave module loaded */
	}

	/* Subtests should unload the module themselves if they use modparams */
}
