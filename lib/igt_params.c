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

#include <i915_drm.h>

#include "igt_core.h"
#include "igt_params.h"
#include "igt_sysfs.h"

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

static int __igt_params_open(int device, char **outpath)
{
	int dir, params = -1;

	dir = igt_sysfs_open(device);
	if (dir >= 0) {
		params = openat(dir,
				"device/driver/module/parameters",
				O_RDONLY);
		close(dir);
	}

	if (params < 0) { /* builtin? */
		drm_version_t version;
		char name[32] = "";
		char path[PATH_MAX];

		memset(&version, 0, sizeof(version));
		version.name_len = sizeof(name);
		version.name = name;
		ioctl(device, DRM_IOCTL_VERSION, &version);

		sprintf(path, "/sys/module/%s/parameters", name);
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
	return __igt_params_open(device, NULL);
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
	int dir;
	int ret;

	dir = igt_params_open(device);
	if (dir < 0)
		return false;

	va_start(ap, fmt);
	ret = igt_sysfs_vprintf(dir, parameter, fmt, ap);
	va_end(ap);

	close(dir);

	return ret > 0;
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
void igt_set_module_param(const char *name, const char *val)
{
	const char *path = "/sys/module/i915/parameters";
	int dir;

	dir = open(path, O_RDONLY);
	igt_assert(dir >= 0);

	igt_params_save(dir, path, name);

	igt_assert(igt_sysfs_set(dir, name, val));

	igt_assert(close(dir) == 0);
}

#define PARAM_VALUE_MAX_SZ 16

/**
 * igt_set_module_param_int:
 * @name: i915.ko parameter name
 * @val: i915.ko parameter value
 *
 * This is a wrapper for igt_set_module_param() that takes an integer instead of
 * a string. Please see igt_set_module_param().
 */
void igt_set_module_param_int(const char *name, int val)
{
	char str[PARAM_VALUE_MAX_SZ];
	int n;

	n = snprintf(str, PARAM_VALUE_MAX_SZ, "%d\n", val);
	igt_assert_f(n < PARAM_VALUE_MAX_SZ,
		     "Need to increase PARAM_VALUE_MAX_SZ\n");

	igt_set_module_param(name, str);
}
