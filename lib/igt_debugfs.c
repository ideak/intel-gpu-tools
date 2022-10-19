/*
 * Copyright © 2013 Intel Corporation
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

#include <inttypes.h>
#include <sys/stat.h>
#include <sys/mount.h>
#ifdef __linux__
#include <sys/sysmacros.h>
#endif
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <i915_drm.h>
#include <poll.h>

#include "drmtest.h"
#include "igt_aux.h"
#include "igt_kms.h"
#include "igt_debugfs.h"
#include "igt_sysfs.h"

/**
 * SECTION:igt_debugfs
 * @short_description: Support code for debugfs features
 * @title: debugfs
 * @include: igt.h
 *
 * This library provides helpers to access debugfs features. On top of some
 * basic functions to access debugfs files with e.g. igt_debugfs_open() it also
 * provides higher-level wrappers for some debugfs features.
 *
 * # Other debugfs interface wrappers
 *
 * This covers the miscellaneous debugfs interface wrappers:
 *
 * - drm/i915 supports interfaces to evict certain classes of gem buffer
 *   objects, see igt_drop_caches_set().
 */

/*
 * General debugfs helpers
 */

static bool is_mountpoint(const char *path)
{
	char buf[strlen(path) + 4];
	struct stat st;
	dev_t dev;

	igt_assert_lt(snprintf(buf, sizeof(buf), "%s/.", path), sizeof(buf));
	if (stat(buf, &st))
		return false;

	if (!S_ISDIR(st.st_mode))
		return false;

	dev = st.st_dev;

	igt_assert_lt(snprintf(buf, sizeof(buf), "%s/..", path), sizeof(buf));
	if (stat(buf, &st))
		return false;

	if (!S_ISDIR(st.st_mode))
		return false;

	return dev != st.st_dev;
}

static const char *__igt_debugfs_mount(void)
{
	if (is_mountpoint("/sys/kernel/debug"))
		return "/sys/kernel/debug";

	if (is_mountpoint("/debug"))
		return "/debug";

	if (mount("debug", "/sys/kernel/debug", "debugfs", 0, 0))
		return NULL;

	return "/sys/kernel/debug";
}

/**
 * igt_debugfs_mount:
 *
 * This attempts to locate where debugfs is mounted on the filesystem,
 * and if not found, will then try to mount debugfs at /sys/kernel/debug.
 *
 * Returns:
 * The path to the debugfs mount point (e.g. /sys/kernel/debug)
 */
const char *igt_debugfs_mount(void)
{
	static const char *path;

	if (!path)
		path = __igt_debugfs_mount();

	return path;
}

/**
 * igt_debugfs_path:
 * @device: fd of the device
 * @path: buffer to store path
 * @pathlen: len of @path buffer.
 *
 * This finds the debugfs directory corresponding to @device.
 *
 * Returns:
 * The directory path, or NULL on failure.
 */
char *igt_debugfs_path(int device, char *path, int pathlen)
{
	struct stat st;
	const char *debugfs_root;
	int idx;

	debugfs_root = igt_debugfs_mount();
	igt_assert(debugfs_root);

	memset(&st, 0, sizeof(st));
	if (device != -1) { /* if no fd, we presume we want dri/0 */
		if (fstat(device, &st)) {
			igt_debug("Couldn't stat FD for DRM device: %m\n");
			return NULL;
		}

		if (!S_ISCHR(st.st_mode)) {
			igt_debug("FD for DRM device not a char device!\n");
			return NULL;
		}
	}

	idx = minor(st.st_rdev);
	snprintf(path, pathlen, "%s/dri/%d/name", debugfs_root, idx);
	if (stat(path, &st))
		return NULL;

	if (idx >= 64) {
		int file, name_len, cmp_len;
		char name[100], cmp[100];

		file = open(path, O_RDONLY);
		if (file < 0)
			return NULL;

		name_len = read(file, name, sizeof(name));
		close(file);

		for (idx = 0; idx < 16; idx++) {
			snprintf(path, pathlen, "%s/dri/%d/name",
				 debugfs_root, idx);
			file = open(path, O_RDONLY);
			if (file < 0)
				continue;

			cmp_len = read(file, cmp, sizeof(cmp));
			close(file);

			if (cmp_len == name_len && !memcmp(cmp, name, name_len))
				break;
		}

		if (idx == 16)
			return NULL;
	}

	snprintf(path, pathlen, "%s/dri/%d", debugfs_root, idx);
	return path;
}

/**
 * igt_debugfs_dir:
 * @device: fd of the device
 *
 * This opens the debugfs directory corresponding to device for use
 * with igt_sysfs_get() and related functions.
 *
 * Returns:
 * The directory fd, or -1 on failure.
 */
int igt_debugfs_dir(int device)
{
	char path[200];

	if (!igt_debugfs_path(device, path, sizeof(path)))
		return -1;

	igt_debug("Opening debugfs directory '%s'\n", path);
	return open(path, O_RDONLY);
}

/**
 * igt_debugfs_connector_dir:
 * @device: fd of the device
 * @conn_name: conenctor name
 * @mode: mode bits as used by open()
 *
 * This opens the debugfs directory corresponding to connector on the device
 * for use with igt_sysfs_get() and related functions.
 *
 * Returns:
 * The directory fd, or -1 on failure.
 */
int igt_debugfs_connector_dir(int device, char *conn_name, int mode)
{
	int dir, ret;

	dir = igt_debugfs_dir(device);
	if (dir < 0)
		return dir;

	ret = openat(dir, conn_name, mode);

	close(dir);

	return ret;
}

/**
 * igt_debugfs_pipe_dir:
 * @device: fd of the device
 * @pipe: index of pipe
 * @mode: mode bits as used by open()
 *
 * This opens the debugfs directory corresponding to the pipe index on the
 * device for use with igt_sysfs_get() and related functions. This is just
 * syntax sugar for igt_debugfs_open().
 *
 * Returns:
 * The directory fd, or -1 on failure.
 */
int igt_debugfs_pipe_dir(int device, int pipe, int mode)
{
	char buf[128];

	snprintf(buf, sizeof(buf), "crtc-%d", pipe);
	return igt_debugfs_open(device, buf, mode);
}

/**
 * igt_debugfs_open:
 * @filename: name of the debugfs node to open
 * @mode: mode bits as used by open()
 *
 * This opens a debugfs file as a Unix file descriptor. The filename should be
 * relative to the drm device's root, i.e. without "drm/$minor".
 *
 * Returns:
 * The Unix file descriptor for the debugfs file or -1 if that didn't work out.
 */
int igt_debugfs_open(int device, const char *filename, int mode)
{
	int dir, ret;

	dir = igt_debugfs_dir(device);
	if (dir < 0)
		return dir;

	ret = openat(dir, filename, mode);

	close(dir);

	return ret;
}

/**
 * igt_debugfs_exists:
 * @device: the drm device file fd
 * @filename: file name
 * @mode: mode bits as used by open()
 *
 * Test that the specified debugfs file exists and can be opened with the
 * requested mode.
 */
bool igt_debugfs_exists(int device, const char *filename, int mode)
{
	int fd = igt_debugfs_open(device, filename, mode);

	if (fd >= 0) {
		close(fd);
		return true;
	}

	return false;
}

/**
 * igt_debugfs_simple_read:
 * @filename: file name
 * @buf: buffer where the contents will be stored, allocated by the caller
 * @size: size of the buffer
 *
 * This function is similar to __igt_debugfs_read, the difference is that it
 * expects the debugfs directory to be open and it's descriptor passed as the
 * first argument.
 *
 * Returns:
 * -errorno on failure or bytes read on success
 */
int igt_debugfs_simple_read(int dir, const char *filename, char *buf, int size)
{
	int len;

	len = igt_sysfs_read(dir, filename, buf, size - 1);
	if (len < 0)
		buf[0] = '\0';
	else
		buf[len] = '\0';

	return len;
}

/**
 * __igt_debugfs_read:
 * @filename: file name
 * @buf: buffer where the contents will be stored, allocated by the caller
 * @size: size of the buffer
 *
 * This function opens the debugfs file, reads it, stores the content in the
 * provided buffer, then closes the file. Users should make sure that the buffer
 * provided is big enough to fit the whole file, plus one byte.
 */
void __igt_debugfs_read(int fd, const char *filename, char *buf, int size)
{
	int dir = igt_debugfs_dir(fd);

	igt_debugfs_simple_read(dir, filename, buf, size);
	close(dir);
}

/**
 * __igt_debugfs_write:
 * @fd: the drm device file fd
 * @filename: file name
 * @buf: buffer to be written to the debugfs file
 * @size: size of the buffer
 *
 * This function opens the debugfs file, writes it, then closes the file.
 */
void __igt_debugfs_write(int fd, const char *filename, const char *buf, int size)
{
	int dir = igt_debugfs_dir(fd);

	igt_sysfs_write(dir, filename, buf, size);
	close(dir);
}

/**
 * igt_debugfs_search:
 * @filename: file name
 * @substring: string to search for in @filename
 *
 * Searches each line in @filename for the substring specified in @substring.
 *
 * Returns: True if the @substring is found to occur in @filename
 */
bool igt_debugfs_search(int device, const char *filename, const char *substring)
{
	FILE *file;
	size_t n = 0;
	char *line = NULL;
	bool matched = false;
	int fd;

	fd = igt_debugfs_open(device, filename, O_RDONLY);
	file = fdopen(fd, "r");
	igt_assert(file);

	while (getline(&line, &n, file) >= 0) {
		matched = strstr(line, substring) != NULL;
		if (matched)
			break;
	}

	free(line);
	fclose(file);
	close(fd);

	return matched;
}

static void igt_hpd_storm_exit_handler(int sig)
{
	int fd = drm_open_driver(DRIVER_INTEL);

	/* Here we assume that only one i915 device will be ever present */
	igt_hpd_storm_reset(fd);

	close(fd);
}

/**
 * igt_hpd_storm_set_threshold:
 * @threshold: How many hotplugs per second required to trigger an HPD storm,
 * or 0 to disable storm detection.
 *
 * Convienence helper to configure the HPD storm detection threshold for i915
 * through debugfs. Useful for hotplugging tests where HPD storm detection
 * might get in the way and slow things down.
 *
 * If the system does not support HPD storm detection, this function does
 * nothing.
 *
 * See: https://01.org/linuxgraphics/gfx-docs/drm/gpu/i915.html#hotplug
 */
void igt_hpd_storm_set_threshold(int drm_fd, unsigned int threshold)
{
	int fd = igt_debugfs_open(drm_fd, "i915_hpd_storm_ctl", O_WRONLY);
	char buf[16];

	if (fd < 0)
		return;

	igt_debug("Setting HPD storm threshold to %d\n", threshold);
	snprintf(buf, sizeof(buf), "%d", threshold);
	igt_assert_eq(write(fd, buf, strlen(buf)), strlen(buf));

	close(fd);
	igt_install_exit_handler(igt_hpd_storm_exit_handler);
}

/**
 * igt_hpd_storm_reset:
 *
 * Convienence helper to reset HPD storm detection to it's default settings.
 * If hotplug detection was disabled on any ports due to an HPD storm, it will
 * be immediately re-enabled. Always called on exit if the HPD storm detection
 * threshold was modified during any tests.
 *
 * If the system does not support HPD storm detection, this function does
 * nothing.
 *
 * See: https://01.org/linuxgraphics/gfx-docs/drm/gpu/i915.html#hotplug
 */
void igt_hpd_storm_reset(int drm_fd)
{
	int fd = igt_debugfs_open(drm_fd, "i915_hpd_storm_ctl", O_WRONLY);
	const char *buf = "reset";

	if (fd < 0)
		return;

	igt_debug("Resetting HPD storm threshold\n");
	igt_assert_eq(write(fd, buf, strlen(buf)), strlen(buf));

	close(fd);
}

/**
 * igt_hpd_storm_detected:
 *
 * Checks whether or not i915 has detected an HPD interrupt storm on any of the
 * system's ports.
 *
 * This function always returns false on systems that do not support HPD storm
 * detection.
 *
 * See: https://01.org/linuxgraphics/gfx-docs/drm/gpu/i915.html#hotplug
 *
 * Returns: Whether or not an HPD storm has been detected.
 */
bool igt_hpd_storm_detected(int drm_fd)
{
	int fd = igt_debugfs_open(drm_fd, "i915_hpd_storm_ctl", O_RDONLY);
	char *start_loc;
	char buf[32] = {0}, detected_str[4];
	bool ret;

	if (fd < 0)
		return false;

	igt_assert_lt(0, read(fd, buf, sizeof(buf) - 1));
	igt_assert(start_loc = strstr(buf, "Detected: "));
	igt_assert_eq(sscanf(start_loc, "Detected: %s\n", detected_str), 1);

	if (strcmp(detected_str, "yes") == 0)
		ret = true;
	else if (strcmp(detected_str, "no") == 0)
		ret = false;
	else
		igt_fail_on_f(true, "Unknown hpd storm detection status '%s'\n",
			      detected_str);

	close(fd);
	return ret;
}

/**
 * igt_require_hpd_storm_ctl:
 *
 * Skips the current test if the system does not have HPD storm detection.
 *
 * See: https://01.org/linuxgraphics/gfx-docs/drm/gpu/i915.html#hotplug
 */
void igt_require_hpd_storm_ctl(int drm_fd)
{
	int fd = igt_debugfs_open(drm_fd, "i915_hpd_storm_ctl", O_RDONLY);

	igt_require_f(fd > 0, "No i915_hpd_storm_ctl found in debugfs\n");
	close(fd);
}

/**
 * igt_reset_fifo_underrun_reporting:
 * @drm_fd: drm device file descriptor
 *
 * Resets fifo underrun reporting, if supported by the device. Useful since fifo
 * underrun reporting tends to be one-shot, so good to reset it before the
 * actual functional test again in case there's been a separate issue happening
 * while preparing the test setup.
 */
void igt_reset_fifo_underrun_reporting(int drm_fd)
{
	int fd = igt_debugfs_open(drm_fd, "i915_fifo_underrun_reset", O_WRONLY);
	if (fd >= 0) {
		igt_assert_eq(write(fd, "y", 1), 1);

		close(fd);
	}
}

/*
 * Drop caches
 */

/**
 * igt_drop_caches_has:
 * @val: bitmask for DROP_* values
 *
 * This queries the debugfs to see if it supports the full set of desired
 * operations.
 */
bool igt_drop_caches_has(int drm_fd, uint64_t val)
{
	uint64_t mask;
	int dir;

	mask = 0;
	dir = igt_debugfs_dir(drm_fd);
	igt_sysfs_scanf(dir, "i915_gem_drop_caches", "0x%" PRIx64, &mask);
	close(dir);

	return (val & mask) == val;
}

/**
 * igt_drop_caches_set:
 * @val: bitmask for DROP_* values
 *
 * This calls the debugfs interface the drm/i915 GEM driver exposes to drop or
 * evict certain classes of gem buffer objects.
 */
void igt_drop_caches_set(int drm_fd, uint64_t val)
{
	int dir;

	dir = igt_debugfs_dir(drm_fd);
	if (is_i915_device(drm_fd)) {
		igt_assert(igt_sysfs_printf(dir, "i915_gem_drop_caches",
					    "0x%" PRIx64, val) > 0);
	} else if (is_msm_device(drm_fd)) {
		/*
		 * msm doesn't currently have debugs that supports fine grained
		 * control of *what* to drop, just # of objects to scan (equiv
		 * to shrink_control::nr_to_scan).  To meet that limit it will
		 * first try shrinking, and then dropping idle.  So just tell
		 * it to try and drop as many objects as possible:
		 */
		igt_assert(igt_sysfs_printf(dir, "shrink", "0x%" PRIx64,
					    ~(uint64_t)0) > 0);
	}
	close(dir);
}

static int get_object_count(int fd)
{
	int dir, ret, scanned;

	igt_drop_caches_set(fd,
			    DROP_RETIRE | DROP_ACTIVE | DROP_IDLE | DROP_FREED);

	dir = igt_debugfs_dir(fd);
	scanned = igt_sysfs_scanf(dir, "i915_gem_objects",
				  "%i objects", &ret);
	igt_assert_eq(scanned, 1);
	close(dir);

	return ret;
}

/**
 * igt_get_stable_obj_count:
 * @driver: fd to drm/i915 GEM driver
 *
 * This puts the driver into a stable (quiescent) state and then returns the
 * current number of gem buffer objects as reported in the i915_gem_objects
 * debugFS interface.
 */
int igt_get_stable_obj_count(int driver)
{
	/* The test relies on the system being in the same state before and
	 * after the test so any difference in the object count is a result of
	 * leaks during the test. */
	return get_object_count(driver);
}

void __igt_debugfs_dump(int device, const char *filename, int level)
{
	char *contents;
	int dir;

	dir = igt_debugfs_dir(device);
	contents = igt_sysfs_get(dir, filename);
	close(dir);

	igt_log(IGT_LOG_DOMAIN, level, "%s:\n%s\n", filename, contents);
	free(contents);
}
