/*
 * Copyright © 2011 Intel Corporation
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
 *    Daniel Vetter <daniel.vetter@ffwll.ch>
 *
 */

/*
 * Testcase: Unreferencing of active buffers
 *
 * Execs buffers and immediately unreferences them, hence the kernel active list
 * will be the last one to hold a reference on them.
 */
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <sys/ioctl.h>
#include <sys/signal.h>
#include <sys/time.h>

#include "igt.h"
#include "i915/gem.h"

IGT_TEST_DESCRIPTION("Test unreferencing of active buffers.");

static int __execbuf(int i915, struct drm_i915_gem_execbuffer2 *execbuf)
{
	int err;

	err = 0;
	if (ioctl(i915, DRM_IOCTL_I915_GEM_EXECBUFFER2, execbuf)) {
		err = -errno;
		igt_assume(err);
	}

	errno = 0;
	return err;
}

static void alarm_handler(int sig)
{
}

igt_simple_main
{
	struct sigaction old_sa, sa = { .sa_handler = alarm_handler };
	unsigned int last[2]= { -1, -1 }, count;
	struct itimerval itv;
	igt_spin_t *spin;
	int i915;

	i915 = drm_open_driver(DRIVER_INTEL);
	igt_require_gem(i915);

	spin = igt_spin_new(i915);
	fcntl(i915, F_SETFL, fcntl(i915, F_GETFL) | O_NONBLOCK);

	sigaction(SIGALRM, &sa, &old_sa);
	itv.it_interval.tv_sec = 0;
	itv.it_interval.tv_usec = 1000;
	itv.it_value.tv_sec = 0;
	itv.it_value.tv_usec = 10000;
	setitimer(ITIMER_REAL, &itv, NULL);

	count = 0;
	do {
		struct drm_i915_gem_exec_object2 obj[2] = {
			{ .handle = gem_create(i915, 4096) },
			spin->obj[IGT_SPIN_BATCH],
		};
		struct drm_i915_gem_execbuffer2 execbuf = {
			.buffers_ptr = to_user_pointer(obj),
			.buffer_count = ARRAY_SIZE(obj),
		};
		int err = __execbuf(i915, &execbuf);
		gem_close(i915, obj[0].handle);

		if (err == 0) {
			count++;
			continue;
		}

		if (err == -EWOULDBLOCK)
			break;

		if (last[1] == count)
			break;

		/* sleep until the next timer interrupt (woken on signal) */
		pause();
		last[1] = last[0];
		last[0] = count;
	} while (1);

	memset(&itv, 0, sizeof(itv));
	setitimer(ITIMER_REAL, &itv, NULL);
	sigaction(SIGALRM, &old_sa, NULL);

	igt_spin_free(i915, spin);
}
