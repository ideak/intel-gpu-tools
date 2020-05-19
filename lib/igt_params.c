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

#include <assert.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

#include <i915_drm.h>

#include "igt_core.h"
#include "igt_params.h"
#include "igt_sysfs.h"
#include "igt_debugfs.h"

struct module_param_data {
	char *path;
	char *name;
	char *original_value;

	struct module_param_data *next;
};
struct module_param_data *module_params = NULL;

static void igt_params_exit_handler(int sig)
{
	struct module_param_data *data;
	int dir;

	for (data = module_params; data != NULL; data = data->next) {
		dir = open(data->path, O_RDONLY);

		if (!igt_sysfs_set(dir, data->name, data->original_value)) {
			const char msg[] = "WARNING: Module parameters "
				"may not have been reset to their "
				"original values\n";
			assert(write(STDERR_FILENO, msg, sizeof(msg))
			       == sizeof(msg));
		}

		close(dir);
	}
	/* free() is not AS-Safe, so we can't call it here. */
}

/**
 * igt_params_save:
 * @dir: file handle for path
 * @path: full path to the sysfs directory
 * @name: name of the sysfs attribute
 *
 * Reads the current value of a sysfs attribute, saves it on an array, then
 * installs an exit handler to restore it when the program exits.
 *
 * It is safe to call this function multiple times for the same parameter.
 *
 * Notice that this function is called by igt_set_module_param(), so that one -
 * or one of its wrappers - is the only function the test programs need to call.
 */
static void igt_params_save(int dir, const char *path, const char *name)
{
	struct module_param_data *data;

	/* Check if this parameter is already saved. */
	for (data = module_params; data != NULL; data = data->next)
		if (strcmp(data->path, path) == 0 &&
		    strcmp(data->name, name) == 0)
			return;

	if (!module_params)
		igt_install_exit_handler(igt_params_exit_handler);

	data = calloc(1, sizeof (*data));
	igt_assert(data);

	data->path = strdup(path);
	igt_assert(data->path);

	data->name = strdup(name);
	igt_assert(data->name);

	data->original_value = igt_sysfs_get(dir, name);
	igt_assert(data->original_value);

	data->next = module_params;
	module_params = data;
}

/**
 * __igt_params_open:
 * @device: fd of the device or -1 for default
 * @outpath: full path to the sysfs directory if not NULL
 * @param: name of parameter of interest
 *
 * Find parameter of interest and return parameter directory fd, parameter
 * is first searched at debugfs/dri/N/<device>_params and if not found will
 * look for parameter at /sys/module/<device>/parameters.
 *
 * Giving -1 here for default device will search for matching device from
 * debugfs/dri/N where N go from 0 to 63. First device found from debugfs
 * which exist also at /sys/module/<device> will be 'default'.
 * Default device will only be used for sysfs, not for debugfs.
 *
 * If outpath is not NULL caller is responsible to free given pointer.
 *
 * Returns:
 * Directory fd, or -1 on failure.
 */
static int __igt_params_open(int device, char **outpath, const char *param)
{
	int dir, params = -1;
	struct stat buffer;
	char searchname[64];
	char searchpath[PATH_MAX];
	char *foundname, *ctx;

	dir = igt_debugfs_dir(device);
	if (dir >= 0) {
		int devname;

		devname = openat(dir, "name", O_RDONLY);
		igt_require_f(devname >= 0,
		              "Driver need to name itself in debugfs!");

		read(devname, searchname, sizeof(searchname));
		close(devname);

		foundname = strtok_r(searchname, " ", &ctx);
		igt_require_f(foundname,
		              "Driver need to name itself in debugfs!");

		snprintf(searchpath, PATH_MAX, "%s_params", foundname);
		params = openat(dir, searchpath, O_RDONLY);

		if (params >= 0) {
			char *debugfspath = malloc(PATH_MAX);

			igt_debugfs_path(device, debugfspath, PATH_MAX);
			if (param != NULL) {
				char filepath[PATH_MAX];

				snprintf(filepath, PATH_MAX, "%s/%s",
					 debugfspath, param);

				if (stat(filepath, &buffer) == 0) {
					if (outpath != NULL)
						*outpath = debugfspath;
					else
						free(debugfspath);
				} else {
					free(debugfspath);
					close(params);
					params = -1;
				}
			} else if (outpath != NULL) {
				/*
				 * Caller is responsible to free this.
				 */
				*outpath = debugfspath;
			} else {
				free(debugfspath);
			}
		}
		close(dir);
	}

	if (params < 0) { /* builtin? */
		drm_version_t version;
		char name[32] = "";
		char path[PATH_MAX];

		if (device == -1) {
			/*
			 * find default device
			 */
			int file, i;
			const char *debugfs_root = igt_debugfs_mount();

			igt_assert(debugfs_root);

			for (i = 0; i < 63; i++) {
				char testpath[PATH_MAX];

				snprintf(searchpath, PATH_MAX,
					 "%s/dri/%d/name", debugfs_root, i);

				file = open(searchpath, O_RDONLY);

				if (file < 0)
					continue;

				read(file, searchname, sizeof(searchname));
				close(file);

				foundname = strtok_r(searchname, " ", &ctx);
				if (!foundname)
					continue;

				snprintf(testpath, PATH_MAX,
					 "/sys/module/%s/parameters",
					 foundname);

				if (stat(testpath, &buffer) == 0 &&
				    S_ISDIR(buffer.st_mode)) {
					snprintf(name, sizeof(name), "%s",
						 foundname);
					break;
				}
			}
		} else {
			memset(&version, 0, sizeof(version));
			version.name_len = sizeof(name);
			version.name = name;
			ioctl(device, DRM_IOCTL_VERSION, &version);
		}
		snprintf(path, sizeof(path), "/sys/module/%s/parameters", name);
		params = open(path, O_RDONLY);
		if (params >= 0 && outpath)
			*outpath = strdup(path);
	}

	return params;
}

/**
 * igt_params_open:
 * @device: fd of the device
 *
 * This opens the module parameters directory (under sysfs) corresponding
 * to the device for use with igt_sysfs_set() and igt_sysfs_get().
 *
 * Returns:
 * The directory fd, or -1 on failure.
 */
int igt_params_open(int device)
{
	return __igt_params_open(device, NULL, NULL);
}

__attribute__((format(printf, 3, 0)))
static bool __igt_params_set(int device, const char *parameter,
			     const char *fmt, va_list ap, bool save)
{
	char *path = NULL;
	int dir;
	int ret;

	dir = __igt_params_open(device, save ? &path : NULL, parameter);
	if (dir < 0)
		return false;

	if (save)
		igt_params_save(dir, path, parameter);

	ret = igt_sysfs_vprintf(dir, parameter, fmt, ap);

	close(dir);
	free(path);

	return ret > 0;
}

/**
 * igt_params_set:
 * @device: fd of the device
 * @parameter: the name of the parameter to set
 * @fmt: printf-esque format string
 *
 * Returns true on success
 */
bool igt_params_set(int device, const char *parameter, const char *fmt, ...)
{
	va_list ap;
	bool ret;

	va_start(ap, fmt);
	ret = __igt_params_set(device, parameter, fmt, ap, false);
	va_end(ap);

	return ret;
}

/**
 * igt_params_save_and_set:
 * @device: fd of the device or -1 to default.
 * @parameter: the name of the parameter to set
 * @fmt: printf-esque format string
 *
 * Save original value to be restored by exit handler. Parameter is first
 * searched at debugfs/dri/N/<device>_params and if not found will look for
 * parameter at /sys/module/<device>/parameters.
 *
 * Giving -1 here for default device will search for matching device from
 * debugfs/dri/N where N go from 0 to 63. First device found from debugfs
 * which exist also at /sys/module/<device> will be 'default'.
 *
 * Returns true on success
 */
bool igt_params_save_and_set(int device, const char *parameter, const char *fmt, ...)
{
	va_list ap;
	bool ret;

	va_start(ap, fmt);
	ret = __igt_params_set(device, parameter, fmt, ap, true);
	va_end(ap);

	return ret;
}

/**
 * igt_set_module_param:
 * @name: i915.ko parameter name
 * @val: i915.ko parameter value
 *
 * This function sets the desired value for the given i915.ko parameter. It also
 * takes care of saving and restoring the values that were already set before
 * the test was run.
 *
 * Please consider using igt_set_module_param_int() for the integer and bool
 * parameters.
 */
void igt_set_module_param(int device, const char *name, const char *val)
{
	igt_assert(igt_params_save_and_set(device, name, "%s", val));
}

/**
 * igt_set_module_param_int:
 * @name: i915.ko parameter name
 * @val: i915.ko parameter value
 *
 * This is a wrapper for igt_set_module_param() that takes an integer instead of
 * a string. Please see igt_set_module_param().
 */
void igt_set_module_param_int(int device, const char *name, int val)
{
	igt_assert(igt_params_save_and_set(device, name, "%d", val));
}
