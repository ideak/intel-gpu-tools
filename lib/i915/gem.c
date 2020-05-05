/*
 * Copyright © 2007,2014,2020 Intel Corporation
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

#include <fcntl.h>
#include <sys/ioctl.h>

#include "i915/gem.h"
#include "igt.h"
#include "igt_debugfs.h"
#include "igt_sysfs.h"

static void reset_device(int i915)
{
	int dir;

	dir = igt_debugfs_dir(i915);
	igt_require(dir >= 0);

	if (ioctl(i915, DRM_IOCTL_I915_GEM_THROTTLE)) {
		igt_info("Found wedged device, trying to reset and continue\n");
		igt_sysfs_set(dir, "i915_wedged", "-1");
	}
	igt_sysfs_set(dir, "i915_next_seqno", "1");

	close(dir);
}

void igt_require_gem(int i915)
{
	int err;

	igt_require_intel(i915);

	/*
	 * We only want to use the throttle-ioctl for its -EIO reporting
	 * of a wedged device, not for actually waiting on outstanding
	 * requests! So create a new drm_file for the device that is clean.
	 */
	i915 = gem_reopen_driver(i915);

	/*
	 * Reset the global seqno at the start of each test. This ensures that
	 * the test will not wrap unless it explicitly sets up seqno wrapping
	 * itself, which avoids accidentally hanging when setting up long
	 * sequences of batches.
	 */
	reset_device(i915);

	err = 0;
	if (ioctl(i915, DRM_IOCTL_I915_GEM_THROTTLE)) {
		err = -errno;
		igt_assume(err);
	}

	close(i915);

	igt_require_f(err == 0, "Unresponsive i915/GEM device\n");
}

/**
 * gem_quiescent_gpu:
 * @i915: open i915 drm file descriptor
 *
 * Ensure the gpu is idle by launching a nop execbuf and stalling for it. This
 * is automatically run when opening a drm device node and is also installed as
 * an exit handler to have the best assurance that the test is run in a pristine
 * and controlled environment.
 *
 * This function simply allows tests to make additional calls in-between, if so
 * desired.
 */
void gem_quiescent_gpu(int i915)
{
	igt_terminate_spins();

	igt_drop_caches_set(i915,
			    DROP_ACTIVE | DROP_RETIRE | DROP_IDLE | DROP_FREED);
}

/**
 * gem_reopen_driver:
 * @i915: re-open the i915 drm file descriptor
 *
 * Re-opens the drm fd which is useful in instances where a clean default
 * context is needed.
 */
int gem_reopen_driver(int i915)
{
	char path[256];

	snprintf(path, sizeof(path), "/proc/self/fd/%d", i915);
	i915 = open(path, O_RDWR);
	igt_assert_fd(i915);

	return i915;
}
