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

#define MODULE_PARAM_DIR "/sys/module/i915/parameters/"
#define PARAM_NAME_MAX_SZ 32
#define PARAM_VALUE_MAX_SZ 16
#define PARAM_FILE_PATH_MAX_SZ (strlen(MODULE_PARAM_DIR) + PARAM_NAME_MAX_SZ)

struct module_param_data {
	char name[PARAM_NAME_MAX_SZ];
	char original_value[PARAM_VALUE_MAX_SZ];

	struct module_param_data *next;
};
struct module_param_data *module_params = NULL;

static void igt_module_param_exit_handler(int sig)
{
	const size_t dir_len = strlen(MODULE_PARAM_DIR);
	char file_path[PARAM_FILE_PATH_MAX_SZ];
	struct module_param_data *data;
	int fd;

	/* We don't need to assert string sizes on this function since they were
	 * already checked before being stored on the lists. Besides,
	 * igt_assert() is not AS-Safe. */
	strcpy(file_path, MODULE_PARAM_DIR);

	for (data = module_params; data != NULL; data = data->next) {
		strcpy(file_path + dir_len, data->name);

		fd = open(file_path, O_RDWR);
		if (fd >= 0) {
			int size = strlen (data->original_value);

			if (size != write(fd, data->original_value, size)) {
				const char msg[] = "WARNING: Module parameters "
					"may not have been reset to their "
					"original values\n";
				assert(write(STDERR_FILENO, msg, sizeof(msg))
				       == sizeof(msg));
			}

			close(fd);
		}
	}
	/* free() is not AS-Safe, so we can't call it here. */
}

/**
 * igt_save_module_param:
 * @name: name of the i915.ko module parameter
 * @file_path: full sysfs file path for the parameter
 *
 * Reads the current value of an i915.ko module parameter, saves it on an array,
 * then installs an exit handler to restore it when the program exits.
 *
 * It is safe to call this function multiple times for the same parameter.
 *
 * Notice that this function is called by igt_set_module_param(), so that one -
 * or one of its wrappers - is the only function the test programs need to call.
 */
static void igt_save_module_param(const char *name, const char *file_path)
{
	struct module_param_data *data;
	size_t n;
	int fd;

	/* Check if this parameter is already saved. */
	for (data = module_params; data != NULL; data = data->next)
		if (strncmp(data->name, name, PARAM_NAME_MAX_SZ) == 0)
			return;

	if (!module_params)
		igt_install_exit_handler(igt_module_param_exit_handler);

	data = calloc(1, sizeof (*data));
	igt_assert(data);

	strncpy(data->name, name, PARAM_NAME_MAX_SZ - 1);

	fd = open(file_path, O_RDONLY);
	igt_assert(fd >= 0);

	n = read(fd, data->original_value, PARAM_VALUE_MAX_SZ);
	igt_assert_f(n > 0 && n < PARAM_VALUE_MAX_SZ,
		     "Need to increase PARAM_VALUE_MAX_SZ\n");

	igt_assert(close(fd) == 0);

	data->next = module_params;
	module_params = data;
}

/**
 * igt_sysfs_open_parameters:
 * @device: fd of the device
 *
 * This opens the module parameters directory (under sysfs) corresponding
 * to the device for use with igt_sysfs_set() and igt_sysfs_get().
 *
 * Returns:
 * The directory fd, or -1 on failure.
 */
int igt_sysfs_open_parameters(int device)
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
	}

	return params;
}

/**
 * igt_sysfs_set_parameters:
 * @device: fd of the device
 * @parameter: the name of the parameter to set
 * @fmt: printf-esque format string
 *
 * Returns true on success
 */
bool igt_sysfs_set_parameter(int device,
			     const char *parameter,
			     const char *fmt, ...)
{
	va_list ap;
	int dir;
	int ret;

	dir = igt_sysfs_open_parameters(device);
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
	char file_path[PARAM_FILE_PATH_MAX_SZ];
	size_t len = strlen(val);
	int fd;

	igt_assert_f(strlen(name) < PARAM_NAME_MAX_SZ,
		     "Need to increase PARAM_NAME_MAX_SZ\n");
	strcpy(file_path, MODULE_PARAM_DIR);
	strcpy(file_path + strlen(MODULE_PARAM_DIR), name);

	igt_save_module_param(name, file_path);

	fd = open(file_path, O_RDWR);
	igt_assert(write(fd, val, len) == len);
	igt_assert(close(fd) == 0);
}

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
