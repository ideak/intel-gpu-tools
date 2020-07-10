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

#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "i915/gem.h"
#include "igt.h"
#include "igt_device_scan.h"
#include "igt_kmod.h"
#include "igt_sysfs.h"

IGT_TEST_DESCRIPTION("Examine behavior of a driver on device hot unplug");

struct hotunplug {
	struct {
		int drm;
		int sysfs_dev;
		int sysfs_bus;
		int sysfs_drv;
	} fd;
	char *dev_bus_addr;
};

/* Helpers */

static void prepare_for_unbind(struct hotunplug *priv, char *buf, int buflen)
{
	int len;

	igt_assert(buflen);

	priv->fd.sysfs_drv = openat(priv->fd.sysfs_dev, "device/driver",
				    O_DIRECTORY);
	igt_assert_fd(priv->fd.sysfs_drv);

	len = readlinkat(priv->fd.sysfs_dev, "device", buf, buflen - 1);
	buf[len] = '\0';
	priv->dev_bus_addr = strrchr(buf, '/');
	igt_assert(priv->dev_bus_addr++);

	/* sysfs_dev no longer needed */
	close(priv->fd.sysfs_dev);
}

static void prepare(struct hotunplug *priv, char *buf, int buflen)
{
	igt_debug("opening device\n");
	priv->fd.drm = __drm_open_driver(DRIVER_ANY);
	igt_assert_fd(priv->fd.drm);

	priv->fd.sysfs_dev = igt_sysfs_open(priv->fd.drm);
	igt_assert_fd(priv->fd.sysfs_dev);

	if (buf) {
		prepare_for_unbind(priv, buf, buflen);
	} else {
		/* prepare for bus rescan */
		priv->fd.sysfs_bus = openat(priv->fd.sysfs_dev,
					    "device/subsystem", O_DIRECTORY);
		igt_assert_fd(priv->fd.sysfs_bus);
	}
}

static const char *failure;

/* Unbind the driver from the device */
static void driver_unbind(int fd_sysfs_drv, const char *dev_bus_addr)
{
	failure = "Driver unbind timeout!";
	igt_set_timeout(60, failure);
	igt_sysfs_set(fd_sysfs_drv, "unbind", dev_bus_addr);
	igt_reset_timeout();
	failure = NULL;

	/* don't close fd_sysfs_drv, it will be used for driver rebinding */
}

/* Re-bind the driver to the device */
static void driver_bind(int fd_sysfs_drv, const char *dev_bus_addr)
{
	failure = "Driver re-bind timeout!";
	igt_set_timeout(60, failure);
	igt_sysfs_set(fd_sysfs_drv, "bind", dev_bus_addr);
	igt_reset_timeout();
	failure = NULL;

	close(fd_sysfs_drv);
}

/* Remove (virtually unplug) the device from its bus */
static void device_unplug(int fd_sysfs_dev)
{
	failure = "Device unplug timeout!";
	igt_set_timeout(60, failure);
	igt_sysfs_set(fd_sysfs_dev, "device/remove", "1");
	igt_reset_timeout();
	failure = NULL;

	close(fd_sysfs_dev);
}

/* Re-discover the device by rescanning its bus */
static void bus_rescan(int fd_sysfs_bus)
{
	failure = "Bus rescan timeout!";
	igt_set_timeout(60, failure);
	igt_sysfs_set(fd_sysfs_bus, "rescan", "1");
	igt_reset_timeout();
	failure = NULL;

	close(fd_sysfs_bus);
}

static void healthcheck(void)
{
	int fd_drm;

	/* device name may have changed, rebuild IGT device list */
	igt_devices_scan(true);

	igt_debug("reopening the device\n");
	fd_drm = __drm_open_driver(DRIVER_ANY);
	igt_abort_on_f(fd_drm < 0, "Device reopen failure");

	if (is_i915_device(fd_drm)) {
		failure = "GEM failure";
		igt_require_gem(fd_drm);
		failure = NULL;
	}

	close(fd_drm);
}

static void set_filter_from_device(int fd)
{
	const char *filter_type = "sys:";
	char filter[strlen(filter_type) + PATH_MAX + 1];
	char *dst = stpcpy(filter, filter_type);
	char path[PATH_MAX + 1];

	igt_assert(igt_sysfs_path(fd, path, PATH_MAX));
	strncat(path, "/device", PATH_MAX - strlen(path));
	igt_assert(realpath(path, dst));

	igt_device_filter_free_all();
	igt_device_filter_add(filter);
}

/* Subtests */

static void unbind_rebind(void)
{
	struct hotunplug priv;
	char buf[PATH_MAX];

	prepare(&priv, buf, sizeof(buf));

	igt_debug("closing the device\n");
	close(priv.fd.drm);

	igt_debug("unbinding the driver from the device\n");
	driver_unbind(priv.fd.sysfs_drv, priv.dev_bus_addr);

	igt_debug("rebinding the driver to the device\n");
	driver_bind(priv.fd.sysfs_drv, priv.dev_bus_addr);

	healthcheck();
}

static void unplug_rescan(void)
{
	struct hotunplug priv;

	prepare(&priv, NULL, 0);

	igt_debug("closing the device\n");
	close(priv.fd.drm);

	igt_debug("unplugging the device\n");
	device_unplug(priv.fd.sysfs_dev);

	igt_debug("recovering the device\n");
	bus_rescan(priv.fd.sysfs_bus);

	healthcheck();
}

static void hotunbind_lateclose(void)
{
	struct hotunplug priv;
	char buf[PATH_MAX];

	prepare(&priv, buf, sizeof(buf));

	igt_debug("hot unbinding the driver from the device\n");
	driver_unbind(priv.fd.sysfs_drv, priv.dev_bus_addr);

	igt_debug("rebinding the driver to the device\n");
	driver_bind(priv.fd.sysfs_drv, priv.dev_bus_addr);

	igt_debug("late closing the unbound device instance\n");
	close(priv.fd.drm);

	healthcheck();
}

static void hotunplug_lateclose(void)
{
	struct hotunplug priv;

	prepare(&priv, NULL, 0);

	igt_debug("hot unplugging the device\n");
	device_unplug(priv.fd.sysfs_dev);

	igt_debug("recovering the device\n");
	bus_rescan(priv.fd.sysfs_bus);

	igt_debug("late closing the removed device instance\n");
	close(priv.fd.drm);

	healthcheck();
}

/* Main */

igt_main
{
	igt_fixture {
		int fd_drm;

		/**
		 * As subtests must be able to close examined devices
		 * completely, don't use drm_open_driver() as it keeps
		 * a device file descriptor open for exit handler use.
		 */
		fd_drm = __drm_open_driver(DRIVER_ANY);
		igt_assert_fd(fd_drm);

		if (is_i915_device(fd_drm))
			igt_require_gem(fd_drm);

		/* Make sure subtests always reopen the same device */
		set_filter_from_device(fd_drm);

		close(fd_drm);
	}

	igt_describe("Check if the driver can be cleanly unbound from a device believed to be closed");
	igt_subtest("unbind-rebind")
		unbind_rebind();

	igt_fixture
		igt_abort_on_f(failure, "%s\n", failure);

	igt_describe("Check if a device believed to be closed can be cleanly unplugged");
	igt_subtest("unplug-rescan")
		unplug_rescan();

	igt_fixture
		igt_abort_on_f(failure, "%s\n", failure);

	igt_describe("Check if the driver can be cleanly unbound from a still open device, then released");
	igt_subtest("hotunbind-lateclose")
		hotunbind_lateclose();

	igt_fixture
		igt_abort_on_f(failure, "%s\n", failure);

	igt_describe("Check if a still open device can be cleanly unplugged, then released");
	igt_subtest("hotunplug-lateclose")
		hotunplug_lateclose();

	igt_fixture
		igt_abort_on_f(failure, "%s\n", failure);
}
