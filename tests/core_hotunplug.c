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
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
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
		int drm_hc;	/* for health check */
		int sysfs_dev;
		int sysfs_bus;
		int sysfs_drv;
	} fd;	/* >= 0: valid fd, == -1: closed, < -1: close failed */
	const char *dev_bus_addr;
	const char *failure;
};

/* Helpers */

/**
 * Subtests must be able to close examined devices completely.  Don't
 * use drm_open_driver() since in case of an i915 device it opens it
 * twice and keeps a second file descriptor open for exit handler use.
 */
static int local_drm_open_driver(bool render, const char *when, const char *why)
{
	int fd_drm;

	igt_debug("%sopening %s device%s\n", when, render ? "render" : "DRM",
		  why);

	fd_drm = render ? __drm_open_driver_render(DRIVER_ANY) :
			  __drm_open_driver(DRIVER_ANY);
	igt_assert_fd(fd_drm);

	return fd_drm;
}

static int local_close(int fd, const char *warning)
{
	errno = 0;
	if (igt_warn_on_f(close(fd), "%s\n", warning))
		return -errno;	/* (never -1) */

	return -1;	/* success - return 'closed' */
}

static int close_device(int fd_drm, const char *when, const char *which)
{
	if (fd_drm < 0)	/* not open - return current status */
		return fd_drm;

	igt_debug("%sclosing %sdevice instance\n", when, which);
	return local_close(fd_drm, "Device close failed");
}

static int close_sysfs(int fd_sysfs_dev)
{
	if (fd_sysfs_dev < 0)	/* not open - return current status */
		return fd_sysfs_dev;

	return local_close(fd_sysfs_dev, "Device sysfs node close failed");
}

static void prepare(struct hotunplug *priv)
{
	const char *filter = igt_device_filter_get(0), *sysfs_path;

	igt_assert(filter);

	priv->dev_bus_addr = strrchr(filter, '/');
	igt_assert(priv->dev_bus_addr++);

	sysfs_path = strchr(filter, ':');
	igt_assert(sysfs_path++);

	igt_assert_eq(priv->fd.sysfs_dev, -1);
	priv->fd.sysfs_dev = open(sysfs_path, O_DIRECTORY);
	igt_assert_fd(priv->fd.sysfs_dev);

	priv->fd.sysfs_drv = openat(priv->fd.sysfs_dev, "driver", O_DIRECTORY);
	igt_assert_fd(priv->fd.sysfs_drv);

	priv->fd.sysfs_bus = openat(priv->fd.sysfs_dev, "subsystem/devices",
				    O_DIRECTORY);
	igt_assert_fd(priv->fd.sysfs_bus);

	priv->fd.sysfs_dev = close_sysfs(priv->fd.sysfs_dev);
}

/* Unbind the driver from the device */
static void driver_unbind(struct hotunplug *priv, const char *prefix,
			  int timeout)
{
	igt_debug("%sunbinding the driver from the device\n", prefix);
	priv->failure = "Driver unbind failure!";

	igt_set_timeout(timeout, "Driver unbind timeout!");
	igt_assert_f(igt_sysfs_set(priv->fd.sysfs_drv, "unbind",
				   priv->dev_bus_addr),
		     "Driver unbind failure!\n");
	igt_reset_timeout();

	igt_assert_f(faccessat(priv->fd.sysfs_drv, priv->dev_bus_addr, F_OK, 0),
		     "Unbound device still present\n");
}

/* Re-bind the driver to the device */
static void driver_bind(struct hotunplug *priv, int timeout)
{
	igt_debug("rebinding the driver to the device\n");
	priv->failure = "Driver re-bind failure!";

	igt_set_timeout(timeout, "Driver re-bind timeout!");
	igt_assert_f(igt_sysfs_set(priv->fd.sysfs_drv, "bind",
				   priv->dev_bus_addr),
		     "Driver re-bind failure\n!");
	igt_reset_timeout();

	igt_fail_on_f(faccessat(priv->fd.sysfs_drv, priv->dev_bus_addr,
				F_OK, 0),
		      "Rebound device not present!\n");
}

/* Remove (virtually unplug) the device from its bus */
static void device_unplug(struct hotunplug *priv, const char *prefix,
			  int timeout)
{
	igt_require(priv->fd.sysfs_dev == -1);

	priv->fd.sysfs_dev = openat(priv->fd.sysfs_bus, priv->dev_bus_addr,
				    O_DIRECTORY);
	igt_assert_fd(priv->fd.sysfs_dev);

	igt_debug("%sunplugging the device\n", prefix);
	priv->failure = "Device unplug failure!";

	igt_set_timeout(timeout, "Device unplug timeout!");
	igt_assert_f(igt_sysfs_set(priv->fd.sysfs_dev, "remove", "1"),
		     "Device unplug failure\n!");
	igt_reset_timeout();

	priv->fd.sysfs_dev = close_sysfs(priv->fd.sysfs_dev);
	igt_assert_eq(priv->fd.sysfs_dev, -1);

	igt_assert_f(faccessat(priv->fd.sysfs_bus, priv->dev_bus_addr, F_OK, 0),
		     "Unplugged device still present\n");
}

/* Re-discover the device by rescanning its bus */
static void bus_rescan(struct hotunplug *priv, int timeout)
{
	igt_debug("rediscovering the device\n");
	priv->failure = "Bus rescan failure!";

	igt_set_timeout(timeout, "Bus rescan timeout!");
	igt_assert_f(igt_sysfs_set(priv->fd.sysfs_bus, "../rescan", "1"),
		       "Bus rescan failure!\n");
	igt_reset_timeout();

	igt_fail_on_f(faccessat(priv->fd.sysfs_bus, priv->dev_bus_addr,
				F_OK, 0),
		      "Fakely unplugged device not rediscovered!\n");
}

static void cleanup(struct hotunplug *priv)
{
	priv->fd.drm = close_device(priv->fd.drm, "post ", "exercised ");
	priv->fd.drm_hc = close_device(priv->fd.drm_hc, "post ",
							"health checked ");
	priv->fd.sysfs_dev = close_sysfs(priv->fd.sysfs_dev);
}

static bool local_i915_is_wedged(int i915)
{
	int err = 0;

	if (ioctl(i915, DRM_IOCTL_I915_GEM_THROTTLE))
		err = -errno;
	return err == -EIO;
}

static bool hang_detected = false;

static void local_sig_abort(int sig)
{
	errno = 0; /* inside a signal, last errno reporting is confusing */
	hang_detected = true;
}

static int local_i915_healthcheck(int i915, const char *prefix)
{
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	struct drm_i915_gem_exec_object2 obj = { };
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(&obj),
		.buffer_count = 1,
	};
	const struct intel_execution_engine2 *engine;

	/* stop our hang detector possibly still running if we failed before */
	igt_stop_hang_detector();

	/* don't run again before GPU reset if hang has been already detected */
	if (hang_detected)
		return -EIO;

	igt_debug("%srunning i915 GPU healthcheck\n", prefix);

	if (local_i915_is_wedged(i915))
		return -EIO;

	obj.handle = gem_create(i915, 4096);
	gem_write(i915, obj.handle, 0, &bbe, sizeof(bbe));

	igt_fork_hang_detector(i915);
	signal(SIGIO, local_sig_abort);

	__for_each_physical_engine(i915, engine) {
		execbuf.flags = engine->flags;
		gem_execbuf(i915, &execbuf);
	}

	gem_sync(i915, obj.handle);
	gem_close(i915, obj.handle);

	igt_stop_hang_detector();
	if (hang_detected)
		return -EIO;

	if (local_i915_is_wedged(i915))
		return -EIO;

	return 0;
}

static int local_i915_recover(int i915)
{
	hang_detected = false;
	if (!local_i915_healthcheck(i915, "re-"))
		return 0;

	igt_debug("forcing i915 GPU reset\n");
	igt_force_gpu_reset(i915);

	hang_detected = false;
	return local_i915_healthcheck(i915, "post-");
}

#define FLAG_RENDER	(1 << 0)
#define FLAG_RECOVER	(1 << 1)
static void node_healthcheck(struct hotunplug *priv, unsigned flags)
{
	bool render = flags & FLAG_RENDER;
	/* preserve potentially dirty device status stored in priv->fd.drm */
	bool closed = priv->fd.drm_hc == -1;
	int fd_drm;

	priv->failure = render ? "Render device reopen failure!" :
				 "DRM device reopen failure!";
	fd_drm = local_drm_open_driver(render, "re", " for health check");
	if (closed)	/* store fd for cleanup if not dirty */
		priv->fd.drm_hc = fd_drm;

	if (is_i915_device(fd_drm)) {
		/* don't report library failed asserts as healthcheck failure */
		priv->failure = "Unrecoverable test failure";
		if (local_i915_healthcheck(fd_drm, "") &&
		    (!(flags & FLAG_RECOVER) || local_i915_recover(fd_drm)))
			priv->failure = "Healthcheck failure!";
		else
			priv->failure = NULL;

	} else {
		/* no device specific healthcheck, rely on reopen result */
		priv->failure = NULL;
	}

	fd_drm = close_device(fd_drm, "", "health checked ");
	if (closed || fd_drm < -1)	/* update status for post_healthcheck */
		priv->fd.drm_hc = fd_drm;
}

static void healthcheck(struct hotunplug *priv, bool recover)
{
	/* device name may have changed, rebuild IGT device list */
	igt_devices_scan(true);

	node_healthcheck(priv, recover ? FLAG_RECOVER : 0);
	if (!priv->failure)
		node_healthcheck(priv,
				 FLAG_RENDER | (recover ? FLAG_RECOVER : 0));

	/* not only request igt_abort on failure, also fail the health check */
	igt_fail_on_f(priv->failure, "%s\n", priv->failure);
}

static void recover(struct hotunplug *priv)
{
	cleanup(priv);

	/* unbind the driver from a possibly hot rebound unhealthy device */
	if (!faccessat(priv->fd.sysfs_drv, priv->dev_bus_addr, F_OK, 0) &&
	    priv->fd.drm == -1 && priv->fd.drm_hc == -1 && priv->failure)
		driver_unbind(priv, "post ", 60);

	if (faccessat(priv->fd.sysfs_bus, priv->dev_bus_addr, F_OK, 0))
		bus_rescan(priv, 60);

	else if (faccessat(priv->fd.sysfs_drv, priv->dev_bus_addr, F_OK, 0))
		driver_bind(priv, 60);

	if (priv->failure)
		healthcheck(priv, true);
}

static void post_healthcheck(struct hotunplug *priv)
{
	igt_abort_on_f(priv->failure, "%s\n", priv->failure);

	cleanup(priv);
	igt_require(priv->fd.drm == -1);
	igt_require(priv->fd.drm_hc == -1);
}

static void set_filter_from_device(int fd)
{
	const char *filter_type = "sys:";
	char filter[strlen(filter_type) + PATH_MAX + 1];
	char *dst = stpcpy(filter, filter_type);
	char path[PATH_MAX + 1];

	igt_assert(igt_sysfs_path(fd, path, PATH_MAX));
	igt_ignore_warn(strncat(path, "/device", PATH_MAX - strlen(path)));
	igt_assert(realpath(path, dst));

	igt_device_filter_free_all();
	igt_assert_eq(igt_device_filter_add(filter), 1);
}

/* Subtests */

static void unbind_rebind(struct hotunplug *priv)
{
	igt_assert_eq(priv->fd.drm, -1);
	igt_assert_eq(priv->fd.drm_hc, -1);

	driver_unbind(priv, "", 0);

	driver_bind(priv, 0);

	healthcheck(priv, false);
}

static void unplug_rescan(struct hotunplug *priv)
{
	igt_assert_eq(priv->fd.drm, -1);
	igt_assert_eq(priv->fd.drm_hc, -1);

	device_unplug(priv, "", 0);

	bus_rescan(priv, 0);

	healthcheck(priv, false);
}

static void hotunbind_rebind(struct hotunplug *priv)
{
	igt_assert_eq(priv->fd.drm, -1);
	igt_assert_eq(priv->fd.drm_hc, -1);
	priv->fd.drm = local_drm_open_driver(false, "", " for hot unbind");

	driver_unbind(priv, "hot ", 0);

	priv->fd.drm = close_device(priv->fd.drm, "late ", "unbound ");
	igt_assert_eq(priv->fd.drm, -1);

	driver_bind(priv, 0);

	healthcheck(priv, false);
}

static void hotunplug_rescan(struct hotunplug *priv)
{
	igt_assert_eq(priv->fd.drm, -1);
	igt_assert_eq(priv->fd.drm_hc, -1);
	priv->fd.drm = local_drm_open_driver(false, "", " for hot unplug");

	device_unplug(priv, "hot ", 0);

	priv->fd.drm = close_device(priv->fd.drm, "late ", "removed ");
	igt_assert_eq(priv->fd.drm, -1);

	bus_rescan(priv, 0);

	healthcheck(priv, false);
}

static void hotrebind_lateclose(struct hotunplug *priv)
{
	igt_assert_eq(priv->fd.drm, -1);
	igt_assert_eq(priv->fd.drm_hc, -1);
	priv->fd.drm = local_drm_open_driver(false, "", " for hot rebind");

	driver_unbind(priv, "hot ", 60);

	driver_bind(priv, 0);

	healthcheck(priv, false);

	priv->fd.drm = close_device(priv->fd.drm, "late ", "unbound ");
	igt_assert_eq(priv->fd.drm, -1);

	healthcheck(priv, false);
}

static void hotreplug_lateclose(struct hotunplug *priv)
{
	igt_assert_eq(priv->fd.drm, -1);
	igt_assert_eq(priv->fd.drm_hc, -1);
	priv->fd.drm = local_drm_open_driver(false, "", " for hot replug");

	device_unplug(priv, "hot ", 60);

	bus_rescan(priv, 0);

	healthcheck(priv, false);

	priv->fd.drm = close_device(priv->fd.drm, "late ", "removed ");
	igt_assert_eq(priv->fd.drm, -1);

	healthcheck(priv, false);
}

/* Main */

igt_main
{
	struct hotunplug priv = {
		.fd		= { .drm = -1, .drm_hc = -1, .sysfs_dev = -1, },
		.failure	= NULL,
	};

	igt_fixture {
		int fd_drm;

		fd_drm = __drm_open_driver(DRIVER_ANY);
		igt_skip_on_f(fd_drm < 0, "No known DRM device found\n");

		if (is_i915_device(fd_drm)) {
			uint32_t devid = intel_get_drm_devid(fd_drm);

			gem_quiescent_gpu(fd_drm);
			igt_require_gem(fd_drm);

			/**
			 * FIXME: Unbinding the i915 driver on some Haswell
			 * platforms with Azalia audio results in a kernel WARN
			 * on "i915 raw-wakerefs=1 wakelocks=1 on cleanup".  The
			 * below CI friendly user level workaround prevents the
			 * warning from appearing.  Drop this hack as soon as
			 * this is fixed in the kernel.
			 */
			if (igt_warn_on_f(IS_HASWELL(devid) ||
					  IS_BROADWELL(devid),
			    "Manually enabling audio PM to work around a kernel WARN\n"))
				igt_pm_enable_audio_runtime_pm();
		}

		/* Make sure subtests always reopen the same device */
		set_filter_from_device(fd_drm);

		igt_assert_eq(close_device(fd_drm, "", "selected "), -1);

		prepare(&priv);

		node_healthcheck(&priv, 0);
		if (!priv.failure)
			node_healthcheck(&priv, FLAG_RENDER);
		igt_skip_on_f(priv.failure, "%s\n", priv.failure);
	}

	igt_subtest_group {
		igt_describe("Check if the driver can be cleanly unbound from a device believed to be closed, then rebound");
		igt_subtest("unbind-rebind")
			unbind_rebind(&priv);

		igt_fixture
			recover(&priv);
	}

	igt_fixture
		post_healthcheck(&priv);

	igt_subtest_group {
		igt_describe("Check if a device believed to be closed can be cleanly unplugged, then restored");
		igt_subtest("unplug-rescan")
			unplug_rescan(&priv);

		igt_fixture
			recover(&priv);
	}

	igt_fixture
		post_healthcheck(&priv);

	igt_subtest_group {
		igt_describe("Check if the driver can be cleanly unbound from an open device, then released and rebound");
		igt_subtest("hotunbind-rebind")
			hotunbind_rebind(&priv);

		igt_fixture
			recover(&priv);
	}

	igt_fixture
		post_healthcheck(&priv);

	igt_subtest_group {
		igt_describe("Check if an open device can be cleanly unplugged, then released and restored");
		igt_subtest("hotunplug-rescan")
			hotunplug_rescan(&priv);

		igt_fixture
			recover(&priv);
	}

	igt_fixture
		post_healthcheck(&priv);

	igt_subtest_group {
		igt_describe("Check if the driver hot unbound from a still open device can be cleanly rebound, then the old instance released");
		igt_subtest("hotrebind-lateclose")
			hotrebind_lateclose(&priv);

		igt_fixture
			recover(&priv);
	}

	igt_fixture
		post_healthcheck(&priv);

	igt_subtest_group {
		igt_describe("Check if a still open while hot unplugged device can be cleanly restored, then the old instance released");
		igt_subtest("hotreplug-lateclose")
			hotreplug_lateclose(&priv);

		igt_fixture
			recover(&priv);
	}

	igt_fixture {
		post_healthcheck(&priv);

		igt_ignore_warn(close(priv.fd.sysfs_bus));
		igt_ignore_warn(close(priv.fd.sysfs_drv));
	}
}
