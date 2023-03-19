/*
 * Copyright (c) 2020, The Linux Foundation. All rights reserved.
 * Copyright 2017 Intel Corporation
 *   Jesse Barnes <jesse.barnes@intel.com>
 *   Manasi Navare <manasi.d.navare@intel.com>
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
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "igt.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <sys/stat.h>

#include "igt_dp_compliance.h"
#include <libudev.h>

struct igt_hotplug_handler_ctx {
	struct udev_monitor *uevent_monitor;
	struct udev *udev;
	GIOChannel *udevchannel;
};

static struct igt_hotplug_handler_ctx hotplug_handler_ctx;

static gboolean hotplug_event(GIOChannel *source, GIOCondition condition,
			      gpointer data)
{
	struct udev_device *dev;
	dev_t udev_devnum;
	struct stat s;
	const char *hotplug;
	const struct igt_hotplug_handler_ctx *ctx = data;

	dev = udev_monitor_receive_device(ctx->uevent_monitor);
	if (!dev)
		goto out;

	udev_devnum = udev_device_get_devnum(dev);
	fstat(drm_fd, &s);

	hotplug = udev_device_get_property_value(dev, "HOTPLUG");

	if (memcmp(&s.st_rdev, &udev_devnum, sizeof(dev_t)) == 0 &&
	    hotplug && atoi(hotplug) == 1)
		update_display(0, false);

	udev_device_unref(dev);
out:
	return TRUE;
}


gboolean igt_dp_compliance_setup_hotplug(void)
{
	struct igt_hotplug_handler_ctx *ctx = &hotplug_handler_ctx;
	int ret;

	ctx->udev = udev_new();
	if (!ctx->udev) {
		igt_warn("Failed to create udev object\n");
		goto out;
	}

	ctx->uevent_monitor = udev_monitor_new_from_netlink(ctx->udev, "udev");
	if (!ctx->uevent_monitor) {
		igt_warn("Failed to create udev event monitor\n");
		goto out;
	}

	ret = udev_monitor_filter_add_match_subsystem_devtype(ctx->uevent_monitor,
							      "drm",
							      "drm_minor");
	if (ret < 0) {
		igt_warn("Failed to filter for drm events\n");
		goto out;
	}

	ret = udev_monitor_enable_receiving(ctx->uevent_monitor);
	if (ret < 0) {
		igt_warn("Failed to enable udev event reception\n");
		goto out;
	}

	ctx->udevchannel =
		g_io_channel_unix_new(udev_monitor_get_fd(ctx->uevent_monitor));
	if (!ctx->udevchannel) {
		igt_warn("Failed to create udev GIOChannel\n");
		goto out;
	}

	ret = g_io_add_watch(ctx->udevchannel, G_IO_IN | G_IO_ERR, hotplug_event,
			     ctx);
	if (ret < 0) {
		igt_warn("Failed to add watch on udev GIOChannel\n");
		goto out;
	}

	return TRUE;

out:
	igt_dp_compliance_cleanup_hotplug();
	return FALSE;
}

void igt_dp_compliance_cleanup_hotplug(void)
{
	struct igt_hotplug_handler_ctx *ctx = &hotplug_handler_ctx;

	if (ctx->udevchannel) {
		g_io_channel_flush(ctx->udevchannel, NULL);
		g_io_channel_unref(ctx->udevchannel);
	}
	if (ctx->uevent_monitor)
		udev_monitor_unref(ctx->uevent_monitor);
	if (ctx->udev)
		udev_unref(ctx->udev);
}
