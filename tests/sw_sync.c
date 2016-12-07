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
#include "igt_primes.h"

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

static void test_sync_busy(void)
{
	int fence, ret;
	int timeline;
	int seqno;

	timeline = sw_sync_timeline_create();
	fence = sw_sync_fence_create(timeline, 5);

	/* Make sure that fence has not been signaled yet */
	ret = sync_wait(fence, 0);
	igt_assert_f(ret == -1 && errno == ETIME, "Fence signaled early (timeline value 0, fence seqno 5)\n");

	/* Advance timeline from 0 -> 1 */
	sw_sync_timeline_inc(timeline, 1);

	/* Make sure that fence has not been signaled yet */
	ret = sync_wait(fence, 0);
	igt_assert_f(ret == -1 && errno == ETIME, "Fence signaled early (timeline value 1, fence seqno 5)\n");

	/* Advance timeline from 1 -> 5: signaling the fence (seqno 5)*/
	sw_sync_timeline_inc(timeline, 4);
	ret = sync_wait(fence, 0);
	igt_assert_f(ret == 0, "Fence not signaled (timeline value 5, fence seqno 5)\n");

	/* Go even further, and confirm wait still succeeds */
	sw_sync_timeline_inc(timeline, 5);
	ret = sync_wait(fence, 0);
	igt_assert_f(ret == 0, "Fence not signaled (timeline value 10, fence seqno 5)\n");

	seqno = 10;
	for_each_prime_number(prime, 100) {
		int fence_prime;
		seqno += prime;

		fence_prime = sw_sync_fence_create(timeline, seqno);
		sw_sync_timeline_inc(timeline, prime);

		ret = sync_wait(fence_prime, 0);
		igt_assert_f(ret == 0, "Fence not signaled during test of prime timeline increments\n");
		close(fence_prime);
	}

	close(fence);
	close(timeline);
}

static void test_sync_merge(void)
{
	int in_fence[3];
	int fence_merge;
	int timeline;
	int active, signaled;

	timeline = sw_sync_timeline_create();
	in_fence[0] = sw_sync_fence_create(timeline, 1);
	in_fence[1] = sw_sync_fence_create(timeline, 2);
	in_fence[2] = sw_sync_fence_create(timeline, 3);

	fence_merge = sync_merge(in_fence[0], in_fence[1]);
	fence_merge = sync_merge(in_fence[2], fence_merge);

	/* confirm all fences have one active point (even d) */
	active = sync_fence_count_status(in_fence[0],
					    SW_SYNC_FENCE_STATUS_ACTIVE);
	igt_assert_f(active == 1, "in_fence[0] has too many active fences\n");
	active = sync_fence_count_status(in_fence[1],
					    SW_SYNC_FENCE_STATUS_ACTIVE);
	igt_assert_f(active == 1, "in_fence[1] has too many active fences\n");
	active = sync_fence_count_status(in_fence[2],
					    SW_SYNC_FENCE_STATUS_ACTIVE);
	igt_assert_f(active == 1, "in_fence[2] has too many active fences\n");
	active = sync_fence_count_status(fence_merge,
					    SW_SYNC_FENCE_STATUS_ACTIVE);
	igt_assert_f(active == 1, "fence_merge has too many active fences\n");

	/* confirm that fence_merge is not signaled until the max of fence 0,1,2 */
	sw_sync_timeline_inc(timeline, 1);
	signaled = sync_fence_count_status(in_fence[0],
					      SW_SYNC_FENCE_STATUS_SIGNALED);
	active = sync_fence_count_status(fence_merge,
					    SW_SYNC_FENCE_STATUS_ACTIVE);
	igt_assert_f(signaled == 1, "in_fence[0] did not signal\n");
	igt_assert_f(active == 1, "fence_merge signaled too early\n");

	sw_sync_timeline_inc(timeline, 1);
	signaled = sync_fence_count_status(in_fence[1],
					      SW_SYNC_FENCE_STATUS_SIGNALED);
	active = sync_fence_count_status(fence_merge,
					    SW_SYNC_FENCE_STATUS_ACTIVE);
	igt_assert_f(signaled == 1, "in_fence[1] did not signal\n");
	igt_assert_f(active == 1, "fence_merge signaled too early\n");

	sw_sync_timeline_inc(timeline, 1);
	signaled = sync_fence_count_status(in_fence[2],
					      SW_SYNC_FENCE_STATUS_SIGNALED);
	igt_assert_f(signaled == 1, "in_fence[2] did not signal\n");
	signaled = sync_fence_count_status(fence_merge,
					       SW_SYNC_FENCE_STATUS_SIGNALED);
	active = sync_fence_count_status(fence_merge,
					    SW_SYNC_FENCE_STATUS_ACTIVE);
	igt_assert_f(active == 0 && signaled == 1,
		     "fence_merge did not signal\n");

	close(in_fence[0]);
	close(in_fence[1]);
	close(in_fence[2]);
	close(fence_merge);
	close(timeline);
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

	igt_subtest("sync_busy")
		test_sync_busy();

	igt_subtest("sync_merge")
		test_sync_merge();
}

