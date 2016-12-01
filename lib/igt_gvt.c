/*
 * Copyright © 2016 Intel Corporation
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

#include "igt.h"
#include "igt_gvt.h"
#include "igt_sysfs.h"

#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>

static bool is_gvt_enabled(void)
{
	FILE *file;
	int value;
	bool enabled = false;

	file = fopen("/sys/module/i915/parameters/enable_gvt", "r");
	if (!file)
		return false;

	if (fscanf(file, "%d", &value) == 1)
		enabled = value;
	fclose(file);

	errno = 0;
	return enabled;
}

static void unload_i915(void)
{
	kick_fbcon(false);
	/* pkill alsact */

	igt_ignore_warn(system("/sbin/modprobe -s -r i915"));
}

bool igt_gvt_load_module(void)
{
	if (is_gvt_enabled())
		return true;

	unload_i915();
	igt_ignore_warn(system("/sbin/modprobe -s i915 enable_gvt=1"));

	return is_gvt_enabled();
}

void igt_gvt_unload_module(void)
{
	if (!is_gvt_enabled())
		return;

	unload_i915();
	igt_ignore_warn(system("/sbin/modprobe -s i915 enable_gvt=0"));

	igt_assert(!is_gvt_enabled());
}
