/*
 * Copyright © 2013 Intel Corporation
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

#include <sched.h>
#include <sys/poll.h>

#include "i915/gem.h"
#include "igt.h"
#include "igt_psr.h"
#include "igt_rand.h"
#include "igt_stats.h"

#if defined(__x86_64__) || defined(__i386__)
#define cpu_relax()	__builtin_ia32_pause()
#else
#define cpu_relax()	asm volatile("": : :"memory")
#endif

#ifndef DRM_CAP_CURSOR_WIDTH
#define DRM_CAP_CURSOR_WIDTH 0x8
#endif

#ifndef DRM_CAP_CURSOR_HEIGHT
#define DRM_CAP_CURSOR_HEIGHT 0x9
#endif

#define PAGE_SIZE	4096

IGT_TEST_DESCRIPTION("Stress legacy cursor ioctl");

igt_pipe_crc_t *pipe_crc;

static int try_commit(igt_display_t *display)
{
	return (display->is_atomic) ?
			igt_display_try_commit_atomic(display,
					DRM_MODE_ATOMIC_TEST_ONLY |
					DRM_MODE_ATOMIC_ALLOW_MODESET,
					NULL) :
			igt_display_try_commit2(display, COMMIT_LEGACY);
}

static void override_output_modes(igt_display_t *display,
				  igt_output_t *output1,
				  igt_output_t *output2)
{
	bool found = igt_override_all_active_output_modes_to_fit_bw(display);
	igt_require_f(found, "No valid mode combo found.\n");

	igt_output_set_pipe(output1, PIPE_NONE);
	igt_output_set_pipe(output2, PIPE_NONE);
}

static void stress(igt_display_t *display,
		   enum pipe pipe, int num_children, unsigned mode,
		   int timeout)
{
	struct drm_mode_cursor arg;
	uint64_t *results;
	bool torture;
	int n;
	unsigned crtc_id[IGT_MAX_PIPES] = {0}, num_crtcs;

	torture = false;
	if (num_children < 0) {
		torture = true;
		num_children = -num_children;
	}

	results = mmap(NULL, PAGE_SIZE, PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
	igt_assert(results != MAP_FAILED);

	memset(&arg, 0, sizeof(arg));
	arg.flags = DRM_MODE_CURSOR_BO;
	arg.crtc_id = 0;
	arg.width = 64;
	arg.height = 64;
	arg.handle = kmstest_dumb_create(display->drm_fd, 64, 64, 32, NULL, NULL);

	if (pipe < 0) {
		num_crtcs = display->n_pipes;
		for_each_pipe(display, n) {
			arg.crtc_id = crtc_id[n] = display->pipes[n].crtc_id;
			do_ioctl(display->drm_fd, DRM_IOCTL_MODE_CURSOR, &arg);
		}
	} else {
		num_crtcs = 1;
		if(display->pipes[pipe].enabled) {
			arg.crtc_id = crtc_id[0] = display->pipes[pipe].crtc_id;
			do_ioctl(display->drm_fd, DRM_IOCTL_MODE_CURSOR, &arg);
		}
	}

	arg.flags = mode;
	igt_fork(child, num_children) {
		struct sched_param rt = {.sched_priority = 99 };
		cpu_set_t allowed;
		unsigned long count = 0;

		sched_setscheduler(getpid(), SCHED_RR, &rt);

		CPU_ZERO(&allowed);
		CPU_SET(child, &allowed);
		sched_setaffinity(getpid(), sizeof(cpu_set_t), &allowed);

		hars_petruska_f54_1_random_perturb(child);
		igt_until_timeout(timeout) {
			arg.crtc_id = crtc_id[hars_petruska_f54_1_random_unsafe() % num_crtcs];
			if (arg.crtc_id)
				do_ioctl(display->drm_fd, DRM_IOCTL_MODE_CURSOR, &arg);
			count++;
		}

		igt_debug("[%d] count=%lu\n", child, count);
		results[child] = count;
	}
	if (torture) {
		igt_fork(child, num_children) {
			struct sched_param rt = {.sched_priority = 1 };
			cpu_set_t allowed;
			unsigned long long count = 0;

			sched_setscheduler(getpid(), SCHED_RR, &rt);

			CPU_ZERO(&allowed);
			CPU_SET(child, &allowed);
			sched_setaffinity(getpid(), sizeof(cpu_set_t), &allowed);
			igt_until_timeout(timeout) {
				count++;
				cpu_relax();
			}
			igt_debug("[hog:%d] count=%llu\n", child, count);
		}
	}
	igt_waitchildren();

	if (num_children > 1) {
		igt_stats_t stats;

		igt_stats_init_with_size(&stats, num_children);
		results[num_children] = 0;
		for (int child = 0; child < num_children; child++) {
			igt_stats_push(&stats, results[child]);
			results[num_children] += results[child];
		}
		igt_info("Total updates %llu (median of %d processes is %.2f)\n",
			 (long long)results[num_children],
			 num_children,
			 igt_stats_get_median(&stats));
		igt_stats_fini(&stats);
	} else {
		igt_info("Total updates %llu\n", (long long)results[0]);
	}

	gem_close(display->drm_fd, arg.handle);
	munmap(results, PAGE_SIZE);
}

static igt_output_t *set_fb_on_crtc(igt_display_t *display, enum pipe pipe, struct igt_fb *fb_info)
{
	igt_output_t *output;

	for_each_valid_output_on_pipe(display, pipe, output) {
		drmModeModeInfoPtr mode;
		igt_plane_t *primary;

		if (output->pending_pipe != PIPE_NONE)
			continue;

		igt_output_set_pipe(output, pipe);
		mode = igt_output_get_mode(output);

		igt_create_pattern_fb(display->drm_fd,
			      mode->hdisplay, mode->vdisplay,
			      DRM_FORMAT_XRGB8888, I915_TILING_NONE, fb_info);

		primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
		igt_plane_set_fb(primary, fb_info);

		return output;
	}

	return NULL;
}

static	igt_plane_t
*set_cursor_on_pipe(igt_display_t *display, enum pipe pipe, struct igt_fb *fb)
{
	igt_plane_t *plane, *cursor = NULL;

	for_each_plane_on_pipe(display, pipe, plane) {
		if (plane->type != DRM_PLANE_TYPE_CURSOR)
			continue;

		cursor = plane;
		break;
	}

	igt_require(cursor);
	igt_plane_set_fb(cursor, fb);

	return cursor;
}

static void populate_cursor_args(igt_display_t *display, enum pipe pipe,
				 struct drm_mode_cursor *arg, struct igt_fb *fb)
{
	arg->crtc_id = display->pipes[pipe].crtc_id;
	arg->flags = DRM_MODE_CURSOR_MOVE;
	arg->x = 128;
	arg->y = 128;
	arg->width = fb->width;
	arg->height = fb->height;
	arg->handle = fb->gem_handle;
	arg[1] = *arg;
}

static enum pipe find_connected_pipe(igt_display_t *display, bool second)
{
	enum pipe pipe, first = PIPE_NONE;
	igt_output_t *output;
	igt_output_t *first_output = NULL;
	bool found = false;

	if (!second) {
		igt_pipe_crc_free(pipe_crc);
		pipe_crc = NULL;

		/* Clear display, events will be eaten by commit.. */
		igt_display_reset(display);
	}

	for_each_pipe_with_valid_output(display, pipe, output) {
		if (first == pipe || output == first_output)
			continue;

		if (second) {
			first = pipe;
			first_output = output;
			second = false;
			continue;
		}

		found = true;
		break;
	}

	if (first_output)
		igt_require_f(found, "No second valid output found\n");
	else
		igt_require_f(found, "No valid outputs found\n");

	return pipe;
}

static void flip_nonblocking(igt_display_t *display, enum pipe pipe_id, bool atomic, struct igt_fb *fb, void *data)
{
	igt_pipe_t *pipe = &display->pipes[pipe_id];
	igt_plane_t *primary = igt_pipe_get_plane_type(pipe, DRM_PLANE_TYPE_PRIMARY);
	int ret;

	igt_set_timeout(5, "Scheduling page flip\n");
	if (!atomic) {
		/* Schedule a nonblocking flip for the next vblank */
		do {
			ret = drmModePageFlip(display->drm_fd, pipe->crtc_id, fb->fb_id,
					      DRM_MODE_PAGE_FLIP_EVENT, data);
		} while (ret == -EBUSY);
	} else {
		igt_plane_set_fb(primary, fb);
		do {
			ret = igt_display_try_commit_atomic(display, DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT, data);
		} while (ret == -EBUSY);
	}
	igt_assert(!ret);
	igt_reset_timeout();
}

enum flip_test {
	flip_test_legacy = 0,
	flip_test_varying_size,
	flip_test_toggle_visibility,
	flip_test_atomic,
	flip_test_atomic_transitions,
	flip_test_atomic_transitions_varying_size,
	flip_test_last = flip_test_atomic_transitions_varying_size
};

static bool cursor_slowpath(igt_display_t *display, enum flip_test mode)
{
	/* Intel display 9 and newer will handle cursor movement as fastsets */
	if (is_i915_device(display->drm_fd) &&
	    intel_display_ver(intel_get_drm_devid(display->drm_fd)) >= 9)
	    return true;

	/* cursor moving doesn't take slowpath, everything else does. */
	if (mode == flip_test_legacy || mode == flip_test_atomic)
		return false;

	return true;
}

/*
 * On platforms with two-stage watermark programming
 * changing sprite visibility may require a extra vblank wait.
 *
 * Handle this here.
 */
static bool mode_requires_extra_vblank(enum flip_test mode)
{
	if (mode == flip_test_atomic_transitions ||
	    mode == flip_test_atomic_transitions_varying_size)
		return true;

	return false;
}

static void transition_nonblocking(igt_display_t *display, enum pipe pipe_id,
				   struct igt_fb *prim_fb, struct igt_fb *argb_fb,
				   bool hide_sprite)
{
	igt_pipe_t *pipe = &display->pipes[pipe_id];
	igt_plane_t *primary = igt_pipe_get_plane_type(pipe, DRM_PLANE_TYPE_PRIMARY);
	igt_plane_t *sprite = igt_pipe_get_plane_type(pipe, DRM_PLANE_TYPE_OVERLAY);

	if (hide_sprite) {
		igt_plane_set_fb(primary, prim_fb);
		igt_plane_set_fb(sprite, NULL);
	} else {
		int ret;

		igt_plane_set_fb(primary, NULL);
		igt_plane_set_fb(sprite, argb_fb);

		ret = igt_display_try_commit_atomic(display, DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT, display);
		if (!ret)
			return;

		igt_assert(ret == -EINVAL);

		igt_plane_set_fb(primary, prim_fb);
		igt_plane_set_fb(sprite, prim_fb);
	}
	igt_display_commit_atomic(display, DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT, display);
}

static void prepare_flip_test(igt_display_t *display,
			      enum flip_test mode,
			      enum pipe flip_pipe,
			      enum pipe cursor_pipe,
			      struct drm_mode_cursor *arg,
			      const struct igt_fb *prim_fb,
			      struct igt_fb *argb_fb,
			      struct igt_fb *cursor_fb2)
{
	argb_fb->gem_handle = 0;
	cursor_fb2->gem_handle = 0;

	if (mode == flip_test_varying_size ||
	    mode == flip_test_atomic_transitions_varying_size) {
		uint64_t width, height;

		do_or_die(drmGetCap(display->drm_fd, DRM_CAP_CURSOR_WIDTH, &width));
		do_or_die(drmGetCap(display->drm_fd, DRM_CAP_CURSOR_HEIGHT, &height));

		igt_skip_on(width <= 64 && height <= 64);
		igt_create_color_fb(display->drm_fd, width, height,
				    DRM_FORMAT_ARGB8888, 0, 1., 0., .7, cursor_fb2);

		arg[0].flags = arg[1].flags = DRM_MODE_CURSOR_BO;
		arg[1].handle = cursor_fb2->gem_handle;
		arg[1].width = width;
		arg[1].height = height;
	}

	if (mode == flip_test_legacy ||
	    mode == flip_test_atomic) {
		arg[1].x = 192;
		arg[1].y = 192;
	}

	if (mode == flip_test_toggle_visibility) {
		arg[0].flags = arg[1].flags = DRM_MODE_CURSOR_BO;
		arg[1].handle = 0;
		arg[1].width = arg[1].height = 0;
	}

	if (mode == flip_test_atomic_transitions ||
	    mode == flip_test_atomic_transitions_varying_size) {
		igt_require(display->pipes[flip_pipe].n_planes > 1 &&
		            display->pipes[flip_pipe].planes[1].type != DRM_PLANE_TYPE_CURSOR);

		igt_create_color_pattern_fb(display->drm_fd, prim_fb->width, prim_fb->height,
					    DRM_FORMAT_ARGB8888, 0, .1, .1, .1, argb_fb);
	}
}

static void flip(igt_display_t *display,
		int cursor_pipe, int flip_pipe,
		int timeout, enum flip_test mode)
{
	struct drm_mode_cursor arg[2];
	uint64_t *results;
	struct igt_fb fb_info, fb_info2, argb_fb, cursor_fb, cursor_fb2;
	igt_output_t *output, *output2;
	igt_plane_t *cursor;

	results = mmap(NULL, PAGE_SIZE, PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
	igt_assert(results != MAP_FAILED);

	igt_display_reset(display);

	flip_pipe = find_connected_pipe(display, !!flip_pipe);
	cursor_pipe = find_connected_pipe(display, !!cursor_pipe);

	igt_info("Using pipe %s for page flip, pipe %s for cursor\n",
		  kmstest_pipe_name(flip_pipe), kmstest_pipe_name(cursor_pipe));

	if (mode >= flip_test_atomic)
		igt_require(display->is_atomic);

	if (mode == flip_test_atomic_transitions ||
		mode == flip_test_atomic_transitions_varying_size) {
		igt_require(igt_pipe_get_plane_type(&display->pipes[flip_pipe],
					DRM_PLANE_TYPE_OVERLAY));
	}

	igt_require((output = set_fb_on_crtc(display, flip_pipe, &fb_info)));
	if (flip_pipe != cursor_pipe) {
		igt_require((output2 = set_fb_on_crtc(display, cursor_pipe, &fb_info2)));

		if (try_commit(display)) {
			override_output_modes(display, output, output2);

			igt_require((output = set_fb_on_crtc(display, flip_pipe, &fb_info)));
			igt_require((output2 = set_fb_on_crtc(display, cursor_pipe, &fb_info2)));
		}
	}

	igt_create_color_fb(display->drm_fd, fb_info.width, fb_info.height, DRM_FORMAT_ARGB8888, 0, .5, .5, .5, &cursor_fb);

	igt_create_color_fb(display->drm_fd, 64, 64, DRM_FORMAT_ARGB8888, 0, 1., 1., 1., &cursor_fb);
	cursor = set_cursor_on_pipe(display, cursor_pipe, &cursor_fb);
	populate_cursor_args(display, cursor_pipe, arg, &cursor_fb);

	prepare_flip_test(display, mode, flip_pipe, cursor_pipe, arg, &fb_info, &argb_fb, &cursor_fb2);

	igt_display_commit2(display, display->is_atomic ? COMMIT_ATOMIC : COMMIT_LEGACY);

	igt_fork(child, 1) {
		unsigned long count = 0;

		igt_until_timeout(timeout) {
			do_ioctl(display->drm_fd, DRM_IOCTL_MODE_CURSOR, &arg[(count & 64)/64]);
			count++;
		}

		igt_debug("cursor count=%lu\n", count);
		results[0] = count;
	}
	igt_fork(child, 1) {
		unsigned long count = 0;

		igt_until_timeout(timeout) {
			char buf[128];

			switch (mode) {
			default:
				flip_nonblocking(display, flip_pipe, mode >= flip_test_atomic, &fb_info, NULL);
				break;
			case flip_test_atomic_transitions:
			case flip_test_atomic_transitions_varying_size:
				transition_nonblocking(display, flip_pipe, &fb_info, &argb_fb, count & 1);
				break;
			}

			while (read(display->drm_fd, buf, sizeof(buf)) < 0 &&
			       (errno == EINTR || errno == EAGAIN))
				;
			count++;
		}

		igt_debug("flip count=%lu\n", count);
		results[1] = count;
	}
	igt_waitchildren();

	munmap(results, PAGE_SIZE);

	/* Clean-up */
	igt_plane_set_fb(cursor, NULL);
	igt_plane_set_fb(igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY),
			 NULL);
	igt_output_set_pipe(output, PIPE_NONE);
	if (flip_pipe != cursor_pipe) {
		igt_plane_set_fb(igt_output_get_plane_type(output2, DRM_PLANE_TYPE_PRIMARY),
			 NULL);
		igt_output_set_pipe(output2, PIPE_NONE);
	}
	igt_display_commit2(display, display->is_atomic ? COMMIT_ATOMIC : COMMIT_LEGACY);

	igt_remove_fb(display->drm_fd, &fb_info);
	if (flip_pipe != cursor_pipe)
		igt_remove_fb(display->drm_fd, &fb_info2);
	igt_remove_fb(display->drm_fd, &cursor_fb);
	if (argb_fb.gem_handle)
		igt_remove_fb(display->drm_fd, &argb_fb);
	if (cursor_fb2.gem_handle)
		igt_remove_fb(display->drm_fd, &cursor_fb2);
}

enum basic_flip_cursor {
	FLIP_BEFORE_CURSOR,
	FLIP_AFTER_CURSOR
};

#define BASIC_BUSY 0x1

static void basic_flip_cursor(igt_display_t *display,
			      enum flip_test mode,
			      enum basic_flip_cursor order,
			      unsigned flags)
{
	struct drm_mode_cursor arg[2];
	struct drm_event_vblank vbl;
	struct igt_fb fb_info, cursor_fb, cursor_fb2, argb_fb;
	unsigned vblank_start;
	enum pipe pipe = find_connected_pipe(display, false);
	uint64_t ahnd = 0;
	igt_spin_t *spin;
	int i, miss1 = 0, miss2 = 0, delta;
	igt_output_t *output;
	igt_plane_t *cursor;

	if (flags & BASIC_BUSY)
	{
		igt_require_intel(display->drm_fd);
		ahnd = get_reloc_ahnd(display->drm_fd, 0);
	}

	if (mode >= flip_test_atomic)
		igt_require(display->is_atomic);

	igt_require((output = set_fb_on_crtc(display, pipe, &fb_info)));

	igt_create_color_fb(display->drm_fd, 64, 64, DRM_FORMAT_ARGB8888, 0, 1., 1., 1., &cursor_fb);
	cursor = set_cursor_on_pipe(display, pipe, &cursor_fb);
	populate_cursor_args(display, pipe, arg, &cursor_fb);

	prepare_flip_test(display, mode, pipe, pipe, arg, &fb_info, &argb_fb, &cursor_fb2);

	igt_display_commit2(display, display->is_atomic ? COMMIT_ATOMIC : COMMIT_LEGACY);

	/* Quick sanity check that we can update a cursor in a single vblank */
	vblank_start = kmstest_get_vblank(display->drm_fd, pipe, DRM_VBLANK_NEXTONMISS);
	igt_assert_eq(kmstest_get_vblank(display->drm_fd, pipe, 0), vblank_start);
	do_ioctl(display->drm_fd, DRM_IOCTL_MODE_CURSOR, &arg[0]);
	igt_assert_eq(kmstest_get_vblank(display->drm_fd, pipe, 0), vblank_start);

	for (i = 0; i < 25; i++) {
		bool miss;

		/* Bind the cursor first to warm up */
		do_ioctl(display->drm_fd, DRM_IOCTL_MODE_CURSOR, &arg[0]);

		spin = NULL;
		if (flags & BASIC_BUSY)
			spin = igt_spin_new(display->drm_fd,
					    .ahnd = ahnd,
					    .dependency = fb_info.gem_handle);

		/* Start with a synchronous query to align with the vblank */
		vblank_start = kmstest_get_vblank(display->drm_fd, pipe, DRM_VBLANK_NEXTONMISS);

		switch (order) {
		case FLIP_BEFORE_CURSOR:
			switch (mode) {
			default:
				flip_nonblocking(display, pipe, mode >= flip_test_atomic, &fb_info, NULL);
				break;
			case flip_test_atomic_transitions:
			case flip_test_atomic_transitions_varying_size:
				transition_nonblocking(display, pipe, &fb_info, &argb_fb, 0);
				break;
			}

			delta = kmstest_get_vblank(display->drm_fd, pipe, 0) - vblank_start;
			miss = delta != 0;

			do_ioctl(display->drm_fd, DRM_IOCTL_MODE_CURSOR, &arg[0]);
			break;

		case FLIP_AFTER_CURSOR:
			do_ioctl(display->drm_fd, DRM_IOCTL_MODE_CURSOR, &arg[0]);

			delta = kmstest_get_vblank(display->drm_fd, pipe, 0) - vblank_start;
			miss = delta != 0;

			switch (mode) {
			default:
				flip_nonblocking(display, pipe, mode >= flip_test_atomic, &fb_info, NULL);
				break;
			case flip_test_atomic_transitions:
			case flip_test_atomic_transitions_varying_size:
				transition_nonblocking(display, pipe, &fb_info, &argb_fb, 0);
				break;
			}
		}

		delta = kmstest_get_vblank(display->drm_fd, pipe, 0) - vblank_start;

		if (spin) {
			struct pollfd pfd = { display->drm_fd, POLLIN };
			igt_assert(poll(&pfd, 1, 0) == 0);
			igt_spin_free(display->drm_fd, spin);
		}

		if (miss)
			{ /* compare nothing, already failed */ }
		else if (!cursor_slowpath(display, mode))
			miss = delta != 0;
		else
			miss = delta != 0 && delta != 1;

		miss1 += miss;

		igt_set_timeout(1, "Stuck page flip");
		igt_ignore_warn(read(display->drm_fd, &vbl, sizeof(vbl)));
		igt_reset_timeout();

		if (miss1)
			continue;

		delta = kmstest_get_vblank(display->drm_fd, pipe, 0) - vblank_start;

		if (!mode_requires_extra_vblank(mode))
			miss2 += delta != 1;
		else
			miss2 += delta != 1 && delta != 2;
	}

	igt_fail_on_f(miss1 > 2 || miss1 + miss2 > 5, "Failed to evade %i vblanks and missed %i page flips\n", miss1, miss2);
	if (miss1 || miss2)
		igt_info("Failed to evade %i vblanks and missed %i page flips\n", miss1, miss2);

	/* Clean-up */
	igt_plane_set_fb(igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY),
			 NULL);
	igt_plane_set_fb(cursor, NULL);
	igt_output_set_pipe(output, PIPE_NONE);
	igt_display_commit2(display, display->is_atomic ? COMMIT_ATOMIC : COMMIT_LEGACY);

	igt_remove_fb(display->drm_fd, &fb_info);
	igt_remove_fb(display->drm_fd, &cursor_fb);

	if (argb_fb.gem_handle)
		igt_remove_fb(display->drm_fd, &argb_fb);
	if (cursor_fb2.gem_handle)
		igt_remove_fb(display->drm_fd, &cursor_fb2);
	put_ahnd(ahnd);
}

static int
get_cursor_updates_per_vblank(igt_display_t *display, enum pipe pipe,
			      struct drm_mode_cursor *arg)
{
	int target;

	for (target = 65536; target; target /= 2) {
		unsigned vblank_start = kmstest_get_vblank(display->drm_fd, pipe, DRM_VBLANK_NEXTONMISS);

		igt_assert_eq(kmstest_get_vblank(display->drm_fd, pipe, 0), vblank_start);

		for (int n = 0; n < target; n++)
			do_ioctl(display->drm_fd, DRM_IOCTL_MODE_CURSOR, arg);
		if (kmstest_get_vblank(display->drm_fd, pipe, 0) == vblank_start)
			break;
	}

	/*
	  * Divide by 4, to handle variations in amount of vblanks
	  * caused by cpufreq throttling.
	  */
	target /= 4;
	igt_require(target > 1);

	igt_info("Using a target of %d cursor updates per quarter-vblank\n", target);

	return target;
}

static void flip_vs_cursor(igt_display_t *display, enum flip_test mode, int nloops)
{
	struct drm_mode_cursor arg[2];
	struct drm_event_vblank vbl;
	struct igt_fb fb_info, cursor_fb, cursor_fb2, argb_fb;
	unsigned vblank_start;
	int target, cpu;
	enum pipe pipe = find_connected_pipe(display, false);
	volatile unsigned long *shared;
	cpu_set_t mask, oldmask;
	igt_output_t *output;
	igt_plane_t *cursor;

	if (mode >= flip_test_atomic)
		igt_require(display->is_atomic);

	igt_require((output = set_fb_on_crtc(display, pipe, &fb_info)));

	igt_create_color_fb(display->drm_fd, 64, 64, DRM_FORMAT_ARGB8888, 0, 1., 1., 1., &cursor_fb);
	cursor = set_cursor_on_pipe(display, pipe, &cursor_fb);
	populate_cursor_args(display, pipe, arg, &cursor_fb);

	prepare_flip_test(display, mode, pipe, pipe, arg, &fb_info, &argb_fb, &cursor_fb2);

	igt_display_commit2(display, display->is_atomic ? COMMIT_ATOMIC : COMMIT_LEGACY);

	if (nloops)
		target = get_cursor_updates_per_vblank(display, pipe, &arg[0]);
	else
		target = 1;

	vblank_start = kmstest_get_vblank(display->drm_fd, pipe, DRM_VBLANK_NEXTONMISS);
	igt_assert_eq(kmstest_get_vblank(display->drm_fd, pipe, 0), vblank_start);
	for (int n = 0; n < target; n++)
		do_ioctl(display->drm_fd, DRM_IOCTL_MODE_CURSOR, &arg[0]);
	igt_assert_eq(kmstest_get_vblank(display->drm_fd, pipe, 0), vblank_start);

	/*
	 * There are variations caused by using cpu frequency changing. To
	 * eliminate those we force this test to run on the same cpu as an
	 * idle thread that does a busy loop of sched_yield(); The effect is
	 * that we don't throttle the cpu to a lower frequency, and the
	 * variations caused by cpu speed changing are eliminated.
	 */
	if (target > 1) {
		shared = mmap(NULL, PAGE_SIZE, PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
		igt_assert(shared != MAP_FAILED);

		cpu = sched_getcpu();
		igt_assert(cpu >= 0);

		CPU_ZERO(&mask);
		CPU_SET(cpu, &mask);
		sched_getaffinity(0, sizeof(oldmask), &oldmask);
		sched_setaffinity(0, sizeof(mask), &mask);

		shared[0] = 0;

		igt_fork(child, 1) {
			struct sched_param parm = { .sched_priority = 0 };

			igt_assert(sched_setscheduler(0, SCHED_IDLE, &parm) == 0);

			while (!shared[0])
				sched_yield();
		}
	}

	do {
		/* Bind the cursor first to warm up */
		do_ioctl(display->drm_fd, DRM_IOCTL_MODE_CURSOR, &arg[nloops & 1]);

		/* Start with a synchronous query to align with the vblank */
		vblank_start = kmstest_get_vblank(display->drm_fd, pipe, DRM_VBLANK_NEXTONMISS);
		switch (mode) {
		default:
			flip_nonblocking(display, pipe, mode >= flip_test_atomic, &fb_info, NULL);
			break;
		case flip_test_atomic_transitions:
		case flip_test_atomic_transitions_varying_size:
			transition_nonblocking(display, pipe, &fb_info, &argb_fb, (nloops & 2) /2);
			break;
		}

		/* The nonblocking flip should not have delayed us */
		igt_assert_eq(kmstest_get_vblank(display->drm_fd, pipe, 0), vblank_start);
		for (int n = 0; n < target; n++)
			do_ioctl(display->drm_fd, DRM_IOCTL_MODE_CURSOR, &arg[nloops & 1]);

		/* Nor should it have delayed the following cursor update */
		if (!cursor_slowpath(display, mode))
			igt_assert_eq(kmstest_get_vblank(display->drm_fd, pipe, 0), vblank_start);
		else if (mode_requires_extra_vblank(mode))
			igt_assert_lte(kmstest_get_vblank(display->drm_fd, pipe, 0), vblank_start + 2);
		else
			igt_assert_lte(kmstest_get_vblank(display->drm_fd, pipe, 0), vblank_start + 1);

		igt_set_timeout(1, "Stuck page flip");
		igt_ignore_warn(read(display->drm_fd, &vbl, sizeof(vbl)));

		if (!mode_requires_extra_vblank(mode))
			igt_assert_eq(kmstest_get_vblank(display->drm_fd, pipe, 0), vblank_start + 1);
		else
			igt_assert_lte(kmstest_get_vblank(display->drm_fd, pipe, 0), vblank_start + 2);

		igt_reset_timeout();
	} while (nloops--);

	if (target > 1) {
		shared[0] = 1;
		igt_waitchildren();
		munmap((void *)shared, PAGE_SIZE);
		sched_setaffinity(0, sizeof(oldmask), &oldmask);
	}

	/* Clean-up */
	igt_plane_set_fb(igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY),
			 NULL);
	igt_plane_set_fb(cursor, NULL);
	igt_output_set_pipe(output, PIPE_NONE);
	igt_display_commit2(display, display->is_atomic ? COMMIT_ATOMIC : COMMIT_LEGACY);

	igt_remove_fb(display->drm_fd, &fb_info);
	igt_remove_fb(display->drm_fd, &cursor_fb);

	if (argb_fb.gem_handle)
		igt_remove_fb(display->drm_fd, &argb_fb);
	if (cursor_fb2.gem_handle)
		igt_remove_fb(display->drm_fd, &cursor_fb2);
}

static void nonblocking_modeset_vs_cursor(igt_display_t *display, int loops)
{
	struct igt_fb fb_info, cursor_fb;
	igt_output_t *output;
	enum pipe pipe = find_connected_pipe(display, false);
	struct drm_mode_cursor arg[2];
	igt_plane_t *primary, *cursor = NULL;

	igt_require(display->is_atomic);
	igt_require((output = set_fb_on_crtc(display, pipe, &fb_info)));
	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
	igt_create_color_fb(display->drm_fd, 64, 64, DRM_FORMAT_ARGB8888, 0, 1., 1., 1., &cursor_fb);
	cursor = set_cursor_on_pipe(display, pipe, &cursor_fb);
	populate_cursor_args(display, pipe, arg, &cursor_fb);
	arg[0].flags |= DRM_MODE_CURSOR_BO;

	/*
	 * Start disabled. No way around it, since the first atomic
	 * commit may be unreliable with amount of events sent.
	 */
	igt_output_set_pipe(output, PIPE_NONE);
	igt_display_commit2(display, COMMIT_ATOMIC);

	while (loops--) {
		unsigned flags;
		struct pollfd pfd = { display->drm_fd, POLLIN };
		struct drm_event_vblank vbl;

		flags = DRM_MODE_ATOMIC_ALLOW_MODESET;
		flags |= DRM_MODE_ATOMIC_NONBLOCK;
		flags |= DRM_MODE_PAGE_FLIP_EVENT;

		/*
		 * Test that a cursor update after a nonblocking modeset
		 * works as intended. It should block until the modeset completes.
		 */

		igt_output_set_pipe(output, pipe);
		igt_plane_set_fb(cursor, NULL);
		igt_display_commit_atomic(display, flags, NULL);

		igt_assert_eq(0, poll(&pfd, 1, 0));
		igt_assert_eq(0, pfd.revents);

		do_ioctl(display->drm_fd, DRM_IOCTL_MODE_CURSOR, &arg[0]);

		igt_assert_eq(1, poll(&pfd, 1, 0));
		igt_assert_eq(POLLIN, pfd.revents);

		igt_set_timeout(1, "Stuck page flip");
		igt_ignore_warn(read(display->drm_fd, &vbl, sizeof(vbl)));
		igt_reset_timeout();

		igt_output_set_pipe(output, PIPE_NONE);
		igt_display_commit_atomic(display, flags, NULL);

		igt_assert_eq(0, poll(&pfd, 1, 0));
		igt_assert_eq(0, pfd.revents);

		/* Same for cursor on disabled crtc. */
		do_ioctl(display->drm_fd, DRM_IOCTL_MODE_CURSOR, &arg[0]);

		igt_assert_eq(1, poll(&pfd, 1, 0));
		igt_assert_eq(POLLIN, pfd.revents);

		igt_set_timeout(1, "Stuck page flip");
		igt_ignore_warn(read(display->drm_fd, &vbl, sizeof(vbl)));
		igt_reset_timeout();
	}

	igt_plane_set_fb(primary, NULL);
	igt_plane_set_fb(cursor, NULL);
	igt_output_set_pipe(output, PIPE_NONE);
	igt_display_commit2(display, COMMIT_ATOMIC);

	igt_remove_fb(display->drm_fd, &fb_info);
	igt_remove_fb(display->drm_fd, &cursor_fb);
}

static void wait_for_modeset(igt_display_t *display, unsigned flags, int timeout,
			     const char *info)
{
	int ret;

	igt_set_timeout(timeout, info);
	do {
		ret = igt_display_try_commit_atomic(display, flags, NULL);
	} while (ret == -EBUSY);
	igt_assert(!ret);
	igt_reset_timeout();
}

static void two_screens_flip_vs_cursor(igt_display_t *display, int nloops, bool modeset, bool atomic)
{
	struct drm_mode_cursor arg1[2], arg2[2];
	struct igt_fb fb_info, fb2_info, cursor_fb;
	enum pipe pipe = find_connected_pipe(display, false);
	enum pipe pipe2 = find_connected_pipe(display, true);
	igt_output_t *output, *output2;
	bool enabled = false;
	volatile unsigned long *shared;
	unsigned flags = 0, vblank_start;
	struct drm_event_vblank vbl;
	int ret;
	igt_plane_t *cursor, *cursor2;

	if (modeset) {
		uint64_t val;

		igt_fail_on(!atomic);
		igt_require(drmGetCap(display->drm_fd, DRM_CAP_CRTC_IN_VBLANK_EVENT, &val) == 0);
	}

	shared = mmap(NULL, PAGE_SIZE, PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
	igt_assert(shared != MAP_FAILED);

	igt_fail_on(modeset && !atomic);

	if (atomic)
		igt_require(display->is_atomic);

	igt_require((output = set_fb_on_crtc(display, pipe, &fb_info)));
	igt_require((output2 = set_fb_on_crtc(display, pipe2, &fb2_info)));

	if (try_commit(display)) {
		override_output_modes(display, output, output2);

		igt_require((output = set_fb_on_crtc(display, pipe, &fb_info)));
		igt_require((output2 = set_fb_on_crtc(display, pipe2, &fb2_info)));
	}

	igt_create_color_fb(display->drm_fd, 64, 64, DRM_FORMAT_ARGB8888, 0, 1., 1., 1., &cursor_fb);
	cursor = set_cursor_on_pipe(display, pipe, &cursor_fb);
	populate_cursor_args(display, pipe, arg1, &cursor_fb);

	arg1[1].x = arg1[1].y = 192;

	cursor2 = set_cursor_on_pipe(display, pipe2, &cursor_fb);
	populate_cursor_args(display, pipe2, arg2, &cursor_fb);

	arg2[1].x = arg2[1].y = 192;

	igt_display_commit2(display, display->is_atomic ? COMMIT_ATOMIC : COMMIT_LEGACY);

	igt_fork(child, 2) {
		struct drm_mode_cursor *arg = child ? arg2 : arg1;

		while (!shared[0])
			do_ioctl(display->drm_fd, DRM_IOCTL_MODE_CURSOR,
				 &arg[!shared[1]]);
	}

	if (modeset) {
		igt_plane_t *plane = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);

		flags = DRM_MODE_ATOMIC_ALLOW_MODESET |
			DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT;

		/* Disable pipe2 */
		igt_output_set_pipe(output2, PIPE_NONE);
		igt_display_commit_atomic(display, flags, NULL);
		enabled = false;

		/*
		 * Try a page flip on crtc 1, if we succeed pump page flips and
		 * modesets interleaved, else do a single atomic commit with both.
		 */
		vblank_start = kmstest_get_vblank(display->drm_fd, pipe, DRM_VBLANK_NEXTONMISS);
		igt_plane_set_fb(plane, &fb_info);
		ret = igt_display_try_commit_atomic(display, flags, (void*)(ptrdiff_t)vblank_start);
		igt_assert(!ret || ret == -EBUSY);

		if (ret == -EBUSY) {
			/* Force completion on both pipes, and generate event. */
			wait_for_modeset(display, flags, 5, "Stuck with -EBUSY");

			while (nloops--) {
				shared[1] = nloops & 1;

				igt_set_timeout(35, "Stuck modeset");
				igt_assert_eq(read(display->drm_fd, &vbl, sizeof(vbl)), sizeof(vbl));
				igt_assert_eq(read(display->drm_fd, &vbl, sizeof(vbl)), sizeof(vbl));
				igt_reset_timeout();

				if (!nloops)
					break;

				/* Commit page flip and modeset simultaneously. */
				igt_plane_set_fb(plane, &fb_info);
				igt_output_set_pipe(output2, enabled ? PIPE_NONE : pipe2);
				enabled = !enabled;

				wait_for_modeset(display, flags, 5, "Scheduling modeset");
			}

			goto done;
		}
	} else {
		vblank_start = kmstest_get_vblank(display->drm_fd, pipe, DRM_VBLANK_NEXTONMISS);
		flip_nonblocking(display, pipe, atomic, &fb_info, (void*)(ptrdiff_t)vblank_start);

		vblank_start = kmstest_get_vblank(display->drm_fd, pipe2, DRM_VBLANK_NEXTONMISS);
		flip_nonblocking(display, pipe2, atomic, &fb2_info, (void*)(ptrdiff_t)vblank_start);
	}

	while (nloops) {
		shared[1] = nloops & 1;

		if (!modeset || nloops > 1)
			igt_set_timeout(1, "Stuck page flip");
		else
			igt_set_timeout(35, "Stuck modeset");
		igt_assert_eq(read(display->drm_fd, &vbl, sizeof(vbl)), sizeof(vbl));
		igt_reset_timeout();

		vblank_start = vbl.user_data;
		if (!modeset)
			igt_assert_eq(vbl.sequence, vblank_start + 1);

		/* Do not requeue on the last 2 events. */
		if (nloops <= 2) {
			nloops--;
			continue;
		}

		if (vbl.crtc_id == display->pipes[pipe].crtc_id) {
			vblank_start = kmstest_get_vblank(display->drm_fd, pipe, DRM_VBLANK_NEXTONMISS);
			flip_nonblocking(display, pipe, atomic, &fb_info, (void*)(ptrdiff_t)vblank_start);
		} else {
			igt_assert(vbl.crtc_id == display->pipes[pipe2].crtc_id);

			nloops--;

			if (!modeset) {
				vblank_start = kmstest_get_vblank(display->drm_fd, pipe2, DRM_VBLANK_NEXTONMISS);
				flip_nonblocking(display, pipe2, atomic, &fb2_info, (void*)(ptrdiff_t)vblank_start);
			} else {
				igt_output_set_pipe(output2, enabled ? PIPE_NONE : pipe2);

				igt_set_timeout(1, "Scheduling modeset\n");
				do {
					ret = igt_display_try_commit_atomic(display, flags, NULL);
				} while (ret == -EBUSY);
				igt_assert(!ret);
				igt_reset_timeout();

				enabled = !enabled;
			}
		}
	}

done:
	shared[0] = 1;
	igt_waitchildren();

	/* Clean-up */
	igt_plane_set_fb(igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY),
			 NULL);
	if (enabled)
		igt_plane_set_fb(igt_output_get_plane_type(output2, DRM_PLANE_TYPE_PRIMARY),
			 NULL);
	igt_plane_set_fb(cursor, NULL);
	igt_plane_set_fb(cursor2, NULL);
	igt_output_set_pipe(output, PIPE_NONE);
	igt_output_set_pipe(output2, PIPE_NONE);
	igt_display_commit2(display, display->is_atomic ? COMMIT_ATOMIC : COMMIT_LEGACY);

	igt_remove_fb(display->drm_fd, &fb_info);
	igt_remove_fb(display->drm_fd, &fb2_info);
	igt_remove_fb(display->drm_fd, &cursor_fb);
	munmap((void *)shared, PAGE_SIZE);
}

static void cursor_vs_flip(igt_display_t *display, enum flip_test mode, int nloops)
{
	struct drm_mode_cursor arg[2];
	struct drm_event_vblank vbl;
	struct igt_fb fb_info, cursor_fb, cursor_fb2, argb_fb;
	unsigned vblank_start, vblank_last;
	volatile unsigned long *shared;
	long target;
	enum pipe pipe = find_connected_pipe(display, false);
	igt_output_t *output;
	uint32_t vrefresh;
	int fail_count;
	igt_plane_t *cursor;

	if (mode >= flip_test_atomic)
		igt_require(display->is_atomic);

	shared = mmap(NULL, PAGE_SIZE, PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
	igt_assert(shared != MAP_FAILED);

	igt_require((output = set_fb_on_crtc(display, pipe, &fb_info)));
	vrefresh = igt_output_get_mode(output)->vrefresh;

	igt_create_color_fb(display->drm_fd, 64, 64, DRM_FORMAT_ARGB8888, 0, 1., 1., 1., &cursor_fb);
	cursor = set_cursor_on_pipe(display, pipe, &cursor_fb);
	populate_cursor_args(display, pipe, arg, &cursor_fb);

	prepare_flip_test(display, mode, pipe, pipe, arg, &fb_info, &argb_fb, &cursor_fb2);

	igt_display_commit2(display, display->is_atomic ? COMMIT_ATOMIC : COMMIT_LEGACY);

	target = get_cursor_updates_per_vblank(display, pipe, &arg[0]);

	fail_count = 0;

	for (int i = 0; i < nloops; i++) {
		shared[0] = 0;
		igt_fork(child, 1) {
			unsigned long count = 0;
			while (!shared[0]) {
				do_ioctl(display->drm_fd, DRM_IOCTL_MODE_CURSOR, &arg[i & 1]);
				count++;
			}
			igt_debug("child: %lu cursor updates\n", count);
			shared[0] = count;
		}

		switch (mode) {
		default:
			flip_nonblocking(display, pipe, mode >= flip_test_atomic, &fb_info, NULL);
			break;
		case flip_test_atomic_transitions:
		case flip_test_atomic_transitions_varying_size:
			transition_nonblocking(display, pipe, &fb_info, &argb_fb, (i & 2) >> 1);
			break;
		}

		igt_assert_eq(read(display->drm_fd, &vbl, sizeof(vbl)), sizeof(vbl));
		vblank_start = vblank_last = vbl.sequence;
		for (int n = 0; n < vrefresh / 2; n++) {
			flip_nonblocking(display, pipe, mode >= flip_test_atomic, &fb_info, NULL);

			igt_assert_eq(read(display->drm_fd, &vbl, sizeof(vbl)), sizeof(vbl));
			if (vbl.sequence != vblank_last + 1) {
				igt_info("page flip %d was delayed, missed %d frames\n",
					 n, vbl.sequence - vblank_last - 1);
			}
			vblank_last = vbl.sequence;
		}

		if (!cursor_slowpath(display, mode))
			igt_assert_lte(vbl.sequence, vblank_start + 5 * vrefresh / 8);

		shared[0] = 1;
		igt_waitchildren();
		if (shared[0] <= vrefresh*target / 2) {
			fail_count++;
			igt_critical("completed %lu cursor updated in a period of %u flips, "
				     "we expect to complete approximately %lu updates, "
				     "with the threshold set at %lu\n",
				     shared[0], vrefresh / 2,
				     vrefresh*target, vrefresh*target / 2);
		}
	}

	igt_assert_f(fail_count == 0,
		     "Failed to meet cursor update expectations in %d out of %d iterations\n",
		     fail_count, nloops);

	/* Clean-up */
	igt_plane_set_fb(igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY),
			 NULL);
	igt_plane_set_fb(cursor, NULL);
	igt_output_set_pipe(output, PIPE_NONE);
	igt_display_commit2(display, display->is_atomic ? COMMIT_ATOMIC : COMMIT_LEGACY);

	igt_remove_fb(display->drm_fd, &fb_info);
	igt_remove_fb(display->drm_fd, &cursor_fb);
	munmap((void *)shared, PAGE_SIZE);
	if (argb_fb.gem_handle)
		igt_remove_fb(display->drm_fd, &argb_fb);
	if (cursor_fb2.gem_handle)
		igt_remove_fb(display->drm_fd, &cursor_fb2);
}

static void two_screens_cursor_vs_flip(igt_display_t *display, int nloops, bool atomic)
{
	struct drm_mode_cursor arg[2][2];
	struct drm_event_vblank vbl;
	struct igt_fb fb_info[2], cursor_fb;
	volatile unsigned long *shared;
	int target[2];
	enum pipe pipe[2] = {
		find_connected_pipe(display, false),
		find_connected_pipe(display, true)
	};
	igt_output_t *outputs[2];
	igt_plane_t *cursors[2];

	shared = mmap(NULL, PAGE_SIZE, PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
	igt_assert(shared != MAP_FAILED);

	if (atomic)
		igt_require(display->is_atomic);

	igt_require((outputs[0] = set_fb_on_crtc(display, pipe[0], &fb_info[0])));
	igt_require((outputs[1] = set_fb_on_crtc(display, pipe[1], &fb_info[1])));

	if (try_commit(display)) {
		override_output_modes(display, outputs[0], outputs[1]);

		igt_require((outputs[0] = set_fb_on_crtc(display, pipe[0], &fb_info[0])));
		igt_require((outputs[1] = set_fb_on_crtc(display, pipe[1], &fb_info[1])));
	}

	igt_create_color_fb(display->drm_fd, 64, 64, DRM_FORMAT_ARGB8888, 0, 1., 1., 1., &cursor_fb);

	cursors[0] = set_cursor_on_pipe(display, pipe[0], &cursor_fb);
	populate_cursor_args(display, pipe[0], arg[0], &cursor_fb);
	arg[0][1].x = arg[0][1].y = 192;

	cursors[1] = set_cursor_on_pipe(display, pipe[1], &cursor_fb);
	populate_cursor_args(display, pipe[1], arg[1], &cursor_fb);
	arg[1][1].x =  arg[1][1].y = 192;

	igt_display_commit2(display, display->is_atomic ? COMMIT_ATOMIC : COMMIT_LEGACY);

	target[0] = get_cursor_updates_per_vblank(display, pipe[0], &arg[0][0]);
	target[1] = get_cursor_updates_per_vblank(display, pipe[1], &arg[1][0]);

	for (int i = 0; i < nloops; i++) {
		unsigned long vrefresh[2];
		unsigned vblank_start[2], vblank_last[2];
		int done[2] = {};

		vrefresh[0] = igt_output_get_mode(outputs[0])->vrefresh;
		vrefresh[1] = igt_output_get_mode(outputs[1])->vrefresh;

		shared[0] = 0;
		shared[1] = 0;
		igt_fork(child, 2) {
			unsigned long count = 0;

			while (!shared[child]) {
				do_ioctl(display->drm_fd, DRM_IOCTL_MODE_CURSOR, &arg[child][(i >> child) & 1]);
				count++;
			}
			igt_debug("child %i: %lu cursor updates\n", child, count);
			shared[child] = count;
		}

		flip_nonblocking(display, pipe[0], atomic, &fb_info[0], (void *)0UL);
		flip_nonblocking(display, pipe[1], atomic, &fb_info[1], (void *)1UL);

		for (int n = 0; n < vrefresh[0] / 2 + vrefresh[1] / 2; n++) {
			unsigned long child;

			igt_assert_eq(read(display->drm_fd, &vbl, sizeof(vbl)), sizeof(vbl));
			child = vbl.user_data;

			if (!done[child]++)
				vblank_start[child] = vbl.sequence;
			else if (vbl.sequence != vblank_last[child] + 1)
				igt_info("page flip %d was delayed, missed %d frames\n",
					 done[child], vbl.sequence - vblank_last[child] - 1);

			vblank_last[child] = vbl.sequence;

			if (done[child] < vrefresh[child] / 2) {
				flip_nonblocking(display, pipe[child], atomic, &fb_info[child], (void *)child);
			} else {
				igt_assert_lte(vbl.sequence, vblank_start[child] + 5 * vrefresh[child] / 8);

				shared[child] = 1;
			}
		}

		igt_assert_eq(done[0], vrefresh[0] / 2);
		igt_assert_eq(done[1], vrefresh[1] / 2);

		igt_waitchildren();
		for (int child = 0; child < 2; child++)
			igt_assert_f(shared[child] > vrefresh[child]*target[child] / 2,
				    "completed %lu cursor updated in a period of %lu flips, "
				    "we expect to complete approximately %lu updates, "
				    "with the threshold set at %lu\n",
				    shared[child], vrefresh[child] / 2,
				    vrefresh[child]*target[child], vrefresh[child]*target[child] / 2);
	}

	/* Clean-up */
	igt_plane_set_fb(igt_output_get_plane_type(outputs[0], DRM_PLANE_TYPE_PRIMARY),
			 NULL);
	igt_plane_set_fb(igt_output_get_plane_type(outputs[1], DRM_PLANE_TYPE_PRIMARY),
			 NULL);
	igt_plane_set_fb(cursors[0], NULL);
	igt_plane_set_fb(cursors[1], NULL);
	igt_output_set_pipe(outputs[0], PIPE_NONE);
	igt_output_set_pipe(outputs[1], PIPE_NONE);
	igt_display_commit2(display, display->is_atomic ? COMMIT_ATOMIC : COMMIT_LEGACY);

	igt_remove_fb(display->drm_fd, &fb_info[0]);
	igt_remove_fb(display->drm_fd, &fb_info[1]);
	igt_remove_fb(display->drm_fd, &cursor_fb);
	munmap((void *)shared, PAGE_SIZE);
}

static void flip_vs_cursor_crc(igt_display_t *display, bool atomic)
{
	struct drm_mode_cursor arg[2];
	struct drm_event_vblank vbl;
	struct igt_fb fb_info, cursor_fb;
	unsigned vblank_start;
	enum pipe pipe = find_connected_pipe(display, false);
	igt_crc_t crcs[3];
	igt_output_t *output;
	igt_plane_t *cursor;

	if (atomic)
		igt_require(display->is_atomic);

	igt_require((output = set_fb_on_crtc(display, pipe, &fb_info)));

	igt_create_color_fb(display->drm_fd, 64, 64, DRM_FORMAT_ARGB8888, 0, 1., 1., 1., &cursor_fb);
	populate_cursor_args(display, pipe, arg, &cursor_fb);

	igt_display_commit2(display, display->is_atomic ? COMMIT_ATOMIC : COMMIT_LEGACY);

	pipe_crc = igt_pipe_crc_new(display->drm_fd, pipe,
				    IGT_PIPE_CRC_SOURCE_AUTO);

	cursor = set_cursor_on_pipe(display, pipe, &cursor_fb);
	igt_display_commit2(display, COMMIT_UNIVERSAL);

	/* Collect reference crcs, crcs[0] last. */
	do_ioctl(display->drm_fd, DRM_IOCTL_MODE_CURSOR, &arg[1]);
	igt_pipe_crc_collect_crc(pipe_crc, &crcs[1]);

	do_ioctl(display->drm_fd, DRM_IOCTL_MODE_CURSOR, &arg[0]);
	igt_pipe_crc_collect_crc(pipe_crc, &crcs[0]);

	/* Disable cursor, and immediately queue a flip. Check if resulting crc is correct. */
	for (int i = 1; i >= 0; i--) {
		vblank_start = kmstest_get_vblank(display->drm_fd, pipe, DRM_VBLANK_NEXTONMISS);

		flip_nonblocking(display, pipe, atomic, &fb_info, NULL);
		do_ioctl(display->drm_fd, DRM_IOCTL_MODE_CURSOR, &arg[i]);

		igt_assert_eq(kmstest_get_vblank(display->drm_fd, pipe, 0), vblank_start);

		igt_set_timeout(1, "Stuck page flip");
		igt_ignore_warn(read(display->drm_fd, &vbl, sizeof(vbl)));
		igt_reset_timeout();

		igt_assert_eq(kmstest_get_vblank(display->drm_fd, pipe, 0), vblank_start + 1);

		igt_pipe_crc_collect_crc(pipe_crc, &crcs[2]);

		igt_assert_crc_equal(&crcs[i], &crcs[2]);
	}

	/* Clean-up */
	igt_plane_set_fb(igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY),
			 NULL);
	igt_plane_set_fb(cursor, NULL);
	igt_output_set_pipe(output, PIPE_NONE);
	igt_display_commit2(display, display->is_atomic ? COMMIT_ATOMIC : COMMIT_LEGACY);

	igt_remove_fb(display->drm_fd, &fb_info);
	igt_remove_fb(display->drm_fd, &cursor_fb);
}

static void flip_vs_cursor_busy_crc(igt_display_t *display, bool atomic)
{
	struct drm_mode_cursor arg[2];
	struct drm_event_vblank vbl;
	struct igt_fb fb_info[2], cursor_fb;
	unsigned vblank_start;
	enum pipe pipe = find_connected_pipe(display, false);
	igt_pipe_t *pipe_connected = &display->pipes[pipe];
	igt_plane_t *plane_primary = igt_pipe_get_plane_type(pipe_connected, DRM_PLANE_TYPE_PRIMARY);
	igt_crc_t crcs[2], test_crc;
	uint64_t ahnd;
	igt_output_t *output;
	igt_plane_t *cursor;

	igt_require_intel(display->drm_fd);
	ahnd = get_reloc_ahnd(display->drm_fd, 0);

	if (atomic)
		igt_require(display->is_atomic);

	igt_require((output = set_fb_on_crtc(display, pipe, &fb_info[0])));
	igt_create_color_pattern_fb(display->drm_fd, fb_info[0].width, fb_info[0].height,
				    DRM_FORMAT_XRGB8888, I915_FORMAT_MOD_X_TILED, .1, .1, .1, &fb_info[1]);

	igt_create_color_fb(display->drm_fd, 64, 64, DRM_FORMAT_ARGB8888, 0, 1., 1., 1., &cursor_fb);
	populate_cursor_args(display, pipe, arg, &cursor_fb);

	igt_display_commit2(display, display->is_atomic ? COMMIT_ATOMIC : COMMIT_LEGACY);

	pipe_crc = igt_pipe_crc_new(display->drm_fd, pipe,
				    IGT_PIPE_CRC_SOURCE_AUTO);

	cursor = set_cursor_on_pipe(display, pipe, &cursor_fb);
	igt_display_commit2(display, COMMIT_UNIVERSAL);

	/* Collect reference crcs, crc[0] last for the loop. */
	do_ioctl(display->drm_fd, DRM_IOCTL_MODE_CURSOR, &arg[1]);
	igt_pipe_crc_collect_crc(pipe_crc, &crcs[1]);

	do_ioctl(display->drm_fd, DRM_IOCTL_MODE_CURSOR, &arg[0]);
	igt_pipe_crc_collect_crc(pipe_crc, &crcs[0]);

	/*
	  * Set fb 1 on primary at least once before flipping to force
	  * setting the correct cache level, else we get a stall in the
	  * page flip handler.
	  */
	igt_plane_set_fb(plane_primary, &fb_info[1]);
	igt_display_commit2(display, COMMIT_UNIVERSAL);

	igt_plane_set_fb(plane_primary, &fb_info[0]);
	igt_display_commit2(display, COMMIT_UNIVERSAL);

	/*
	 * We must enable CRC collecting here since this may force
	 * a modeset, and this loop is timing sensitive.
	 */
	igt_pipe_crc_start(pipe_crc);

	/* Disable cursor, and immediately queue a flip. Check if resulting crc is correct. */
	for (int i = 1; i >= 0; i--) {
		igt_spin_t *spin;

		spin = igt_spin_new(display->drm_fd,
				    .ahnd = ahnd,
				    .dependency = fb_info[1].gem_handle);

		vblank_start = kmstest_get_vblank(display->drm_fd, pipe, DRM_VBLANK_NEXTONMISS);

		flip_nonblocking(display, pipe, atomic, &fb_info[1], NULL);
		do_ioctl(display->drm_fd, DRM_IOCTL_MODE_CURSOR, &arg[i]);

		igt_assert_eq(kmstest_get_vblank(display->drm_fd, pipe, 0), vblank_start);

		igt_pipe_crc_get_current(display->drm_fd, pipe_crc, &test_crc);

		igt_spin_free(display->drm_fd, spin);

		igt_set_timeout(1, "Stuck page flip");
		igt_ignore_warn(read(display->drm_fd, &vbl, sizeof(vbl)));
		igt_reset_timeout();

		igt_assert_lte(vblank_start + 1, kmstest_get_vblank(display->drm_fd, pipe, 0));

		igt_plane_set_fb(plane_primary, &fb_info[0]);
		igt_display_commit2(display, COMMIT_UNIVERSAL);

		igt_assert_crc_equal(&crcs[i], &test_crc);
	}

	/* Clean-up */
	igt_plane_set_fb(plane_primary, NULL);
	igt_plane_set_fb(cursor, NULL);
	igt_output_set_pipe(output, PIPE_NONE);
	igt_display_commit2(display, display->is_atomic ? COMMIT_ATOMIC : COMMIT_LEGACY);

	igt_pipe_crc_stop(pipe_crc);
	igt_remove_fb(display->drm_fd, &fb_info[1]);
	igt_remove_fb(display->drm_fd, &fb_info[0]);
	igt_remove_fb(display->drm_fd, &cursor_fb);
	put_ahnd(ahnd);
}

igt_main
{
	const int ncpus = sysconf(_SC_NPROCESSORS_ONLN);
	igt_display_t display = { .drm_fd = -1 };
	bool intel_psr2_restore = false;
	int i;
	const char *modes[flip_test_last+1] = {
		"legacy",
		"varying-size",
		"toggle",
		"atomic",
		"atomic-transitions",
		"atomic-transitions-varying-size"
	};
	const char *prefix[2] = {"basic", "short"};

	igt_fixture {
		display.drm_fd = drm_open_driver_master(DRIVER_ANY);
		kmstest_set_vt_graphics_mode();

		igt_display_require(&display, display.drm_fd);
		/*
		 * Not possible to evade vblank after a primary or sprite plane
		 * page flip with cursor legacy APIS when Intel's PSR2 selective
		 * fetch is enabled, so switching PSR1 for this whole test.
		 */
		intel_psr2_restore = i915_psr2_sel_fetch_to_psr1(display.drm_fd);
	}

	igt_describe("Test checks how many cursor updates we can fit between vblanks "
		     "on single/all pipes with different modes, priority and number of processes");
	igt_subtest_group {
		enum pipe n;
		struct {
			const char *name;
			int ncpus;
			unsigned flags;
		} tests[] = {
			{ "single-bo", 1, DRM_MODE_CURSOR_BO },
			{ "single-move", 1, DRM_MODE_CURSOR_MOVE },
			{ "forked-bo", ncpus, DRM_MODE_CURSOR_BO },
			{ "forked-move", ncpus, DRM_MODE_CURSOR_MOVE },
			{ "torture-bo", -ncpus, DRM_MODE_CURSOR_BO },
			{ "torture-move", -ncpus, DRM_MODE_CURSOR_MOVE },
		};

		for (i = 0; i < ARRAY_SIZE(tests); i++) {
			igt_subtest_with_dynamic(tests[i].name) {
				for_each_pipe(&display, n) {
					errno = 0;

					igt_dynamic_f("pipe-%s", kmstest_pipe_name(n))
						stress(&display, n, tests[i].ncpus, tests[i].flags, 20);
				}

				errno = 0;
				igt_dynamic("all-pipes")
					stress(&display, -1, tests[i].ncpus, tests[i].flags, 20);
			}
		}
	}

	igt_describe("Test checks how many cursor updates we can fit between vblanks "
		    "on all pipes with different modes, priority and number of processes");
	igt_subtest_group {
		igt_fixture
			igt_display_require_output(&display);

		igt_subtest("nonblocking-modeset-vs-cursor-atomic")
			nonblocking_modeset_vs_cursor(&display, 1);

		igt_subtest("long-nonblocking-modeset-vs-cursor-atomic")
			nonblocking_modeset_vs_cursor(&display, 16);
	}

	igt_describe("This test executes flips on both CRTCs "
		     "while running cursor updates in parallel");
	igt_subtest_group {
		struct {
			const char *name;
			int nloops;
			bool modeset;
			bool atomic;
		} tests[] = {
			{ "2x-flip-vs-cursor-legacy", 8, false, false },
			{ "2x-flip-vs-cursor-atomic", 8, false, true },
			{ "2x-long-flip-vs-cursor-legacy", 150, false, false },
			{ "2x-long-flip-vs-cursor-atomic", 150, false, true },
			{ "2x-nonblocking-modeset-vs-cursor-atomic", 4, true, true },
			{ "2x-long-nonblocking-modeset-vs-cursor-atomic", 15, true, true },
		};

		igt_fixture
			igt_display_require_output(&display);

		for (i = 0; i < ARRAY_SIZE(tests); i++) {
			igt_subtest(tests[i].name)
				two_screens_flip_vs_cursor(&display,
							   tests[i].nloops,
							   tests[i].modeset,
							   tests[i].atomic);
		}
	}

	igt_describe("This test executes flips on both CRTCs "
		     "while running cursor updates in parallel");
	igt_subtest_group {
		struct {
			const char *name;
			int nloops;
			bool atomic;
		} tests[] = {
			{ "2x-cursor-vs-flip-legacy", 8, false },
			{ "2x-long-cursor-vs-flip-legacy", 50, false },
			{ "2x-cursor-vs-flip-atomic", 8, true },
			{ "2x-long-cursor-vs-flip-atomic", 50, true },
		};

		igt_fixture
			igt_display_require_output(&display);

		for (i = 0; i < ARRAY_SIZE(tests); i++) {
			igt_subtest(tests[i].name)
				two_screens_cursor_vs_flip(&display,
							   tests[i].nloops,
							   tests[i].atomic);
		}
	}

	igt_describe("Test will first does a page flip and then cursor update");
	igt_subtest_group {
		igt_fixture {
			igt_require_pipe_crc(display.drm_fd);
			igt_display_require_output(&display);
		}

		igt_subtest("flip-vs-cursor-crc-legacy")
			flip_vs_cursor_crc(&display, false);

		igt_subtest("flip-vs-cursor-crc-atomic")
			flip_vs_cursor_crc(&display, true);
	}

	igt_describe("this test perform a busy bo update followed by a cursor update");
	igt_subtest_group {
		igt_fixture {
			igt_require_pipe_crc(display.drm_fd);
			igt_display_require_output(&display);
		}

		igt_subtest("flip-vs-cursor-busy-crc-legacy")
			flip_vs_cursor_busy_crc(&display, false);

		igt_subtest("flip-vs-cursor-busy-crc-atomic")
			flip_vs_cursor_busy_crc(&display, true);
	}

	for (i = 0; i < ARRAY_SIZE(prefix); i++) {
		int j;

		igt_describe("Adds variety of tests:\n"
			"* varying-size: change the size of cursor b/w 64*64 to maxw x maxh.\n"
			"* atomic-transition: alternates between a full screen sprite plane "
				"and full screen primary plane.\n"
			"* toggle: which toggles cursor visibility and make sure cursor moves between updates.");
		igt_subtest_group {
			igt_fixture
				igt_display_require_output(&display);

			igt_subtest_with_dynamic_f("%s-flip-before-cursor", prefix[i]) {
				for (j = 0; j <= flip_test_last; j++) {
					igt_dynamic_f("%s", modes[j])
						basic_flip_cursor(&display, j, FLIP_BEFORE_CURSOR, 0);
				}
			}

			igt_subtest_with_dynamic_f("%s-busy-flip-before-cursor", prefix[i]) {
				igt_require(!cursor_slowpath(&display, i));
				igt_require_gem(display.drm_fd);

				for (j = 0; j <= flip_test_last; j++) {
					igt_dynamic_f("%s", modes[j])
						basic_flip_cursor(&display, j, FLIP_BEFORE_CURSOR, BASIC_BUSY);
				}
			}

			igt_subtest_with_dynamic_f("%s-flip-after-cursor", prefix[i]) {
				for (j = 0; j <= flip_test_last; j++) {
					igt_dynamic_f("%s", modes[j])
						basic_flip_cursor(&display, j, FLIP_AFTER_CURSOR, 0);
				}
			}
		}
	}

	igt_describe("The essence of the basic test is that neither the cursor nor the "
		     "nonblocking flip stall the application of the next");
	igt_subtest_group {
		igt_fixture
			igt_display_require_output(&display);

		igt_subtest_with_dynamic("flip-vs-cursor") {
			for (i = 0; i <= flip_test_last; i++) {
				igt_dynamic_f("%s", modes[i])
					flip_vs_cursor(&display, i, 150);
			}
		}

		igt_subtest_with_dynamic("cursor-vs-flip") {
			for (i = 0; i <= flip_test_last; i++) {
				igt_dynamic_f("%s", modes[i])
					cursor_vs_flip(&display, i, 50);
			}
		}

		igt_subtest_with_dynamic("cursorA-vs-flipA") {
			for (i = 0; i <= flip_test_last; i++) {
				igt_dynamic_f("%s", modes[i])
					flip(&display, 0, 0, 10, i);
			}
		}

		igt_subtest_with_dynamic("cursorA-vs-flipB") {
			for (i = 0; i <= flip_test_last; i++) {
				igt_dynamic_f("%s", modes[i])
					flip(&display, 0, 1, 10, i);
			}
		}

		igt_subtest_with_dynamic("cursorB-vs-flipA") {
			for (i = 0; i <= flip_test_last; i++) {
				igt_dynamic_f("%s", modes[i])
					flip(&display, 1, 0, 10, i);
			}
		}

		igt_subtest_with_dynamic("cursorB-vs-flipB") {
			for (i = 0; i <= flip_test_last; i++) {
				igt_dynamic_f("%s", modes[i])
					flip(&display, 1, 1, 10, i);
			}
		}
	}

	igt_fixture {
		if (intel_psr2_restore)
			i915_psr2_sel_fetch_restore(display.drm_fd);
		igt_display_fini(&display);
		close(display.drm_fd);
	}
}
