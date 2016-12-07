/*
 * Copyright © 2016 Collabora, Ltd.
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
 *    Robert Foss <robert.foss@collabora.com>
 */

#include <stdint.h>
#include <unistd.h>

#include "igt.h"
#include "igt_aux.h"

#include "sw_sync.h"


IGT_TEST_DESCRIPTION("Test SW Sync Framework");

static void test_alloc_timeline(void)
{
	int timeline;

	timeline = sw_sync_timeline_create();
	close(timeline);
}

static void test_alloc_fence(void)
{
	int in_fence;
	int timeline;

	timeline = sw_sync_timeline_create();
	in_fence = sw_sync_fence_create(timeline, 0);

	close(in_fence);
	close(timeline);
}

static void test_alloc_fence_invalid_timeline(void)
{
	igt_assert_f(__sw_sync_fence_create(-1, 0) < 0,
	    "Did not fail to create fence on invalid timeline\n");
}

static void test_alloc_merge_fence(void)
{
	int in_fence[2];
	int fence_merge;
	int timeline[2];

	timeline[0] = sw_sync_timeline_create();
	timeline[1] = sw_sync_timeline_create();

	in_fence[0] = sw_sync_fence_create(timeline[0], 1);
	in_fence[1] = sw_sync_fence_create(timeline[1], 1);
	fence_merge = sync_merge(in_fence[1], in_fence[0]);

	close(in_fence[0]);
	close(in_fence[1]);
	close(fence_merge);
	close(timeline[0]);
	close(timeline[1]);
}

igt_main
{
	igt_subtest("alloc_timeline")
		test_alloc_timeline();

	igt_subtest("alloc_fence")
		test_alloc_fence();

	igt_subtest("alloc_fence_invalid_timeline")
		test_alloc_fence_invalid_timeline();

	igt_subtest("alloc_merge_fence")
		test_alloc_merge_fence();
}

