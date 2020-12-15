// SPDX-License-Identifier: MIT
/*
 * Copyright(c) 2020 Intel Corporation. All rights reserved.
 */
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

#include "i915/gem.h"
#include "igt.h"
#include "igt_device_scan.h"
#include "igt_sysfs.h"

IGT_TEST_DESCRIPTION("Examine behavior of a driver on device sysfs reset");


#define DEV_PATH_LEN 80
#define DEV_BUS_ADDR_LEN 13 /* addr has form 0000:00:00.0 */

/**
 * Helper structure containing file descriptors
 * and bus address related to tested device
 */
struct device_fds {
	struct {
		int dev;
		int dev_dir;
		int drv_dir;
	} fds;
	char dev_bus_addr[DEV_BUS_ADDR_LEN];
};

static int __open_sysfs_dir(int fd, const char* path)
{
	int sysfs;

	sysfs = igt_sysfs_open(fd);
	if (sysfs < 0) {
		return -1;
	}

	fd = openat(sysfs, path, O_DIRECTORY);
	close(sysfs);
	return fd;
}

static int open_device_sysfs_dir(int fd)
{
	return __open_sysfs_dir(fd, "device");
}

static int open_driver_sysfs_dir(int fd)
{
	return __open_sysfs_dir(fd, "device/driver");
}

/**
 * device_sysfs_path:
 * @fd: opened device file descriptor
 * @path: buffer to store sysfs path to device directory
 *
 * Returns:
 * On successfull path resolution sysfs path to device directory,
 * NULL otherwise
 */
static char *device_sysfs_path(int fd, char *path)
{
	char sysfs[DEV_PATH_LEN];

	if (!igt_sysfs_path(fd, sysfs, sizeof(sysfs)))
		return NULL;

	if (DEV_PATH_LEN <= (strlen(sysfs) + strlen("/device")))
		return NULL;

	strcat(sysfs, "/device");

	return realpath(sysfs, path);
}

static void init_device_fds(struct device_fds *dev)
{
	char dev_path[PATH_MAX];
	char *addr_pos;

	igt_debug("open device\n");
	/**
	 * As subtests must be able to close examined devices
	 * completely, don't use drm_open_driver() as it keeps
	 * a device file descriptor open for exit handler use.
	 */
	dev->fds.dev = __drm_open_driver(DRIVER_ANY);
	igt_assert_fd(dev->fds.dev);
	if (is_i915_device(dev->fds.dev))
		igt_require_gem(dev->fds.dev);

	igt_assert(device_sysfs_path(dev->fds.dev, dev_path));
	addr_pos = strrchr(dev_path, '/');
	igt_assert(addr_pos);
	igt_assert_eq(sizeof(dev->dev_bus_addr) - 1,
		      snprintf(dev->dev_bus_addr, sizeof(dev->dev_bus_addr),
			       "%s", addr_pos + 1));

	dev->fds.dev_dir = open_device_sysfs_dir(dev->fds.dev);
	igt_assert_fd(dev->fds.dev_dir);

	dev->fds.drv_dir = open_driver_sysfs_dir(dev->fds.dev);
	igt_assert_fd(dev->fds.drv_dir);
}

static int close_if_opened(int *fd)
{
	int rc = 0;

	if (fd && *fd != -1) {
		rc = close(*fd);
		*fd = -1;
	}
	return rc;
}

static void cleanup_device_fds(struct device_fds *dev)
{
	igt_ignore_warn(close_if_opened(&dev->fds.dev));
	igt_ignore_warn(close_if_opened(&dev->fds.dev_dir));
	igt_ignore_warn(close_if_opened(&dev->fds.drv_dir));
}

/**
 * is_sysfs_reset_supported:
 * @fd: opened device file descriptor
 *
 * Check if device supports reset based on sysfs file presence.
 *
 * Returns:
 * True if device supports reset, false otherwise.
 */
static bool is_sysfs_reset_supported(int fd)
{
	struct stat st;
	int rc;
	int sysfs;
	int reset_fd = -1;

	sysfs = igt_sysfs_open(fd);

	if (sysfs >= 0) {
		reset_fd = openat(sysfs, "device/reset", O_WRONLY);
		close(sysfs);
	}

	if (reset_fd < 0)
		return false;

	rc = fstat(reset_fd, &st);
	close(reset_fd);

	if (rc || !S_ISREG(st.st_mode))
		return false;

	return true;
}

/* Unbind the driver from the device */
static void driver_unbind(struct device_fds *dev)
{
	igt_debug("unbind the driver from the device\n");
	igt_assert(igt_sysfs_set(dev->fds.drv_dir, "unbind",
		   dev->dev_bus_addr));
}

/* Re-bind the driver to the device */
static void driver_bind(struct device_fds *dev)
{
	igt_debug("rebind the driver to the device\n");
	igt_abort_on_f(!igt_sysfs_set(dev->fds.drv_dir, "bind",
		       dev->dev_bus_addr), "driver rebind failed");
}

/* Initiate device reset */
static void initiate_device_reset(struct device_fds *dev)
{
	igt_debug("reset device\n");
	igt_assert(igt_sysfs_set(dev->fds.dev_dir, "reset", "1"));
}

static bool is_i915_wedged(int i915)
{
	int err = 0;

	if (ioctl(i915, DRM_IOCTL_I915_GEM_THROTTLE))
		err = -errno;
	return err == -EIO;
}

/**
 * healthcheck:
 * @dev: structure with device descriptor, if descriptor equals -1
 * 	 the device is reopened
 */
static void healthcheck(struct device_fds *dev)
{
	if (dev->fds.dev == -1) {
		/* refresh device list */
		igt_devices_scan(true);
		igt_debug("reopen the device\n");
		dev->fds.dev = __drm_open_driver(DRIVER_ANY);
	}
	igt_assert_fd(dev->fds.dev);

	if (is_i915_device(dev->fds.dev))
		igt_assert(!is_i915_wedged(dev->fds.dev));
}

/**
 * set_device_filter:
 *
 * Sets device filter to ensure subtests always reopen the same device
 *
 * @dev_path: path to device under tests
 */
static void set_device_filter(const char* dev_path)
{
#define FILTER_PREFIX_LEN 4
	char filter[PATH_MAX + FILTER_PREFIX_LEN];

	igt_assert_lt(FILTER_PREFIX_LEN, snprintf(filter, sizeof(filter),
						  "sys:%s", dev_path));
	igt_device_filter_free_all();
	igt_assert_eq(igt_device_filter_add(filter), 1);
}

static void unbind_reset_rebind(struct device_fds *dev)
{
	igt_debug("close the device\n");
	close_if_opened(&dev->fds.dev);

	/**
	 * FIXME: Unbinding the i915 driver on some platforms with Azalia audio
	 * results in a kernel WARN on "i915 raw-wakerefs=1 wakelocks=1 on cleanup".
	 * The below CI friendly user level workaround prevents the warning from
	 * appearing. Drop this hack as soon as this is fixed in the kernel.
	 */
	if (is_i915_device(dev->fds.dev)) {
		uint32_t devid = intel_get_drm_devid(dev->fds.dev);
		if (igt_warn_on_f(IS_HASWELL(devid) || IS_BROADWELL(devid),
		    "Manually enabling audio PM to work around a kernel WARN\n"))
			igt_pm_enable_audio_runtime_pm();
	}

	driver_unbind(dev);

	initiate_device_reset(dev);

	driver_bind(dev);
}

igt_main
{
	struct device_fds dev = { .fds = {-1, -1, -1}, .dev_bus_addr = {0}};

	igt_fixture {
		char dev_path[PATH_MAX];

		igt_debug("opening device\n");
		init_device_fds(&dev);

		/* Make sure subtests always reopen the same device */
		igt_assert(device_sysfs_path(dev.fds.dev, dev_path));
		set_device_filter(dev_path);

		igt_skip_on(!is_sysfs_reset_supported(dev.fds.dev));

		igt_set_timeout(60, "device reset tests timed out after 60s");
	}

	igt_describe("Unbinds driver from device, initiates reset"
		     " then rebinds driver to device");
	igt_subtest("unbind-reset-rebind") {
		unbind_reset_rebind(&dev);
		healthcheck(&dev);
	}

	igt_describe("Resets device with bound driver");
	igt_subtest("reset-bound") {
		initiate_device_reset(&dev);
		healthcheck(&dev);
	}

	igt_fixture {
		igt_reset_timeout();
		cleanup_device_fds(&dev);
	}
}
