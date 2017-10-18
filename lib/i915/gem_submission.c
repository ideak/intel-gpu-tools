/*
 * Copyright © 2017 Intel Corporation
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

#include <stdbool.h>

#include "igt_core.h"
#include "igt_sysfs.h"

#include "i915/gem_submission.h"

/**
 * SECTION:gem_submission
 * @short_description: Helpers for determining submission method
 * @title: GEM Submission
 *
 * This helper library contains functions used for getting information on
 * currently used hardware submission method. Different generations of hardware
 * support different submission backends, currently we're distinguishing 3
 * different methods: legacy ringbuffer submission, execlists, GuC submission.
 * For legacy ringbuffer submission, there's also a variation where we're using
 * semaphores for synchronization between engines.
 */

/**
 * gem_submission_method:
 * @fd: open i915 drm file descriptor
 *
 * Returns: Submission method bitmap.
 */
unsigned gem_submission_method(int fd)
{
	unsigned flags = 0;
	bool active;
	int dir;

	dir = igt_sysfs_open_parameters(fd);
	if (dir < 0)
		return 0;

	active = igt_sysfs_get_boolean(dir, "enable_guc_submission");
	if (active) {
		flags |= GEM_SUBMISSION_GUC | GEM_SUBMISSION_EXECLISTS;
		goto out;
	}

	active = igt_sysfs_get_boolean(dir, "enable_execlists");
	if (active) {
		flags |= GEM_SUBMISSION_EXECLISTS;
		goto out;
	}

	active = igt_sysfs_get_boolean(dir, "semaphores");
	if (active) {
		flags |= GEM_SUBMISSION_SEMAPHORES;
	}

out:
	close(dir);
	return flags;
}

/**
 * gem_submission_print_method:
 * @fd: open i915 drm file descriptor
 *
 * Helper for pretty-printing currently used submission method
 */
void gem_submission_print_method(int fd)
{
	const unsigned flags = gem_submission_method(fd);

	if (flags & GEM_SUBMISSION_GUC) {
		igt_info("Using GuC submission\n");
		return;
	}

	if (flags & GEM_SUBMISSION_EXECLISTS) {
		igt_info("Using Execlists submission\n");
		return;
	}

	igt_info("Using Legacy submission%s\n",
		 flags & GEM_SUBMISSION_SEMAPHORES ? ", with semaphores" : "");
}

/**
 * gem_has_semaphores:
 * @fd: open i915 drm file descriptor
 *
 * Feature test macro to query whether the driver is using semaphores for
 * synchronization between engines.
 */
bool gem_has_semaphores(int fd)
{
	return gem_submission_method(fd) & GEM_SUBMISSION_SEMAPHORES;
}

/**
 * gem_has_execlists:
 * @fd: open i915 drm file descriptor
 *
 * Feature test macro to query whether the driver is using execlists as a
 * hardware submission method.
 */
bool gem_has_execlists(int fd)
{
	return gem_submission_method(fd) & GEM_SUBMISSION_EXECLISTS;
}
