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
 *
 * Authors:
 *  Paul Kocialkowski <paul.kocialkowski@linux.intel.com>
 */

#include "config.h"

#include <fcntl.h>
#include <pixman.h>
#include <cairo.h>

#include "igt.h"

/**
 * SECTION:igt_frame
 * @short_description: Library for frame-related tests
 * @title: Frame
 * @include: igt_frame.h
 *
 * This library contains helpers for frame-related tests. This includes common
 * frame dumping as well as frame comparison helpers.
 */

/**
 * igt_frame_dump_is_enabled:
 *
 * Get whether frame dumping is enabled.
 *
 * Returns: A boolean indicating whether frame dumping is enabled
 */
bool igt_frame_dump_is_enabled(void)
{
	return frame_dump_path != NULL;
}

static void igt_write_frame_to_png(cairo_surface_t *surface, int fd,
				   const char *qualifier, const char *suffix)
{
	char path[PATH_MAX];
	const char *test_name;
	const char *subtest_name;
	cairo_status_t status;
	int index;

	test_name = igt_test_name();
	subtest_name = igt_subtest_name();

	if (suffix)
		snprintf(path, PATH_MAX, "%s/frame-%s-%s-%s-%s.png",
			 frame_dump_path, test_name, subtest_name, qualifier,
			 suffix);
	else
		snprintf(path, PATH_MAX, "%s/frame-%s-%s-%s.png",
			 frame_dump_path, test_name, subtest_name, qualifier);

	igt_debug("Dumping %s frame to %s...\n", qualifier, path);

	status = cairo_surface_write_to_png(surface, path);

	igt_assert_eq(status, CAIRO_STATUS_SUCCESS);

	index = strlen(path);

	if (fd >= 0 && index < (PATH_MAX - 1)) {
		path[index++] = '\n';
		path[index] = '\0';

		write(fd, path, strlen(path));
	}
}

/**
 * igt_write_compared_frames_to_png:
 * @reference: The reference cairo surface
 * @capture: The captured cairo surface
 * @reference_suffix: The suffix to give to the reference png file
 * @capture_suffix: The suffix to give to the capture png file
 *
 * Write previously compared frames to png files.
 */
void igt_write_compared_frames_to_png(cairo_surface_t *reference,
				      cairo_surface_t *capture,
				      const char *reference_suffix,
				      const char *capture_suffix)
{
	char *id;
	const char *test_name;
	const char *subtest_name;
	char path[PATH_MAX];
	int fd = -1;

	if (!igt_frame_dump_is_enabled())
		return;

	id = getenv("IGT_FRAME_DUMP_ID");

	test_name = igt_test_name();
	subtest_name = igt_subtest_name();

	if (id)
		snprintf(path, PATH_MAX, "%s/frame-%s-%s-%s.txt",
			 frame_dump_path, test_name, subtest_name, id);
	else
		snprintf(path, PATH_MAX, "%s/frame-%s-%s.txt",
			 frame_dump_path, test_name, subtest_name);

	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	igt_assert(fd >= 0);

	igt_debug("Writing dump report to %s...\n", path);

	igt_write_frame_to_png(reference, fd, "reference", reference_suffix);
	igt_write_frame_to_png(capture, fd, "capture", capture_suffix);

	close(fd);
}
