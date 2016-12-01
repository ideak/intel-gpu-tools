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

#include <sys/poll.h>
#include <signal.h>
#include <time.h>

IGT_TEST_DESCRIPTION("Basic check of KMS ABI with busy framebuffers.");

#define FRAME_TIME 16 /* milleseconds */
#define TIMEOUT (6*16)

static igt_output_t *
set_fb_on_crtc(igt_display_t *dpy, int pipe, struct igt_fb *fb)
{
	igt_output_t *output;

	for_each_valid_output_on_pipe(dpy, pipe, output) {
		drmModeModeInfoPtr mode;
		igt_plane_t *primary;

		if (output->pending_crtc_idx_mask)
			continue;

		igt_output_set_pipe(output, pipe);
		mode = igt_output_get_mode(output);

		igt_create_pattern_fb(dpy->drm_fd,
				      mode->hdisplay, mode->vdisplay,
				      DRM_FORMAT_XRGB8888,
				      LOCAL_I915_FORMAT_MOD_X_TILED,
				      fb);

		primary = igt_output_get_plane(output, IGT_PLANE_PRIMARY);
		igt_plane_set_fb(primary, fb);

		return output;
	}

	return NULL;
}

static void do_cleanup_display(igt_display_t *dpy)
{
	enum pipe pipe;
	igt_output_t *output;
	igt_plane_t *plane;

	for_each_pipe(dpy, pipe)
		for_each_plane_on_pipe(dpy, pipe, plane)
			igt_plane_set_fb(plane, NULL);

	for_each_connected_output(dpy, output)
		igt_output_set_pipe(output, PIPE_NONE);

	igt_display_commit2(dpy, dpy->is_atomic ? COMMIT_ATOMIC : COMMIT_LEGACY);
}

static void finish_fb_busy(igt_spin_t *spin, int msecs)
{
	struct timespec tv = { 0, msecs * 1000 * 1000 };
	nanosleep(&tv, NULL);
	igt_spin_batch_end(spin);
}

static void sighandler(int sig)
{
}

static void flip_to_fb(igt_display_t *dpy, int pipe,
		       struct igt_fb *fb, unsigned ring,
		       const char *name)
{
	struct pollfd pfd = { .fd = dpy->drm_fd, .events = POLLIN };
	struct timespec tv = { 1, 0 };
	struct drm_event_vblank ev;
	igt_spin_t *t = igt_spin_batch_new(dpy->drm_fd, ring, fb->gem_handle);
	igt_fork(child, 1) {
		igt_assert(gem_bo_busy(dpy->drm_fd, fb->gem_handle));
		do_or_die(drmModePageFlip(dpy->drm_fd,
					  dpy->pipes[pipe].crtc_id, fb->fb_id,
					  DRM_MODE_PAGE_FLIP_EVENT, fb));
		kill(getppid(), SIGALRM);
		igt_assert(gem_bo_busy(dpy->drm_fd, fb->gem_handle));
		igt_assert_f(poll(&pfd, 1, TIMEOUT) == 0,
			     "flip completed whilst %s was busy [%d]\n",
			     name, gem_bo_busy(dpy->drm_fd, fb->gem_handle));
	}
	igt_assert_f(nanosleep(&tv, NULL) == -1,
		     "flip to %s blocked waiting for busy fb", name);
	finish_fb_busy(t, 2*TIMEOUT);
	igt_waitchildren();
	igt_assert(read(dpy->drm_fd, &ev, sizeof(ev)) == sizeof(ev));
	igt_assert(poll(&pfd, 1, 0) == 0);

	igt_spin_batch_free(dpy->drm_fd, t);
}

static void test_flip(igt_display_t *dpy, unsigned ring, int pipe)
{
	struct igt_fb fb[2];
	int warmup[] = { 0, 1, 0, -1 };

	signal(SIGALRM, sighandler);

	igt_require(set_fb_on_crtc(dpy, pipe, &fb[0]));
	igt_display_commit2(dpy, COMMIT_LEGACY);

	igt_create_pattern_fb(dpy->drm_fd,
			      fb[0].width, fb[0].height,
			      DRM_FORMAT_XRGB8888,
			      LOCAL_I915_FORMAT_MOD_X_TILED,
			      &fb[1]);

	/* Bind both fb to the display (such that they are ready for future
	 * flips without stalling for the bind) leaving fb[0] as bound.
	 */
	for (int i = 0; warmup[i] != -1; i++) {
		struct drm_event_vblank ev;

		do_or_die(drmModePageFlip(dpy->drm_fd,
					  dpy->pipes[pipe].crtc_id,
					  fb[warmup[i]].fb_id,
					  DRM_MODE_PAGE_FLIP_EVENT,
					  &fb[warmup[i]]));
		igt_assert(read(dpy->drm_fd, &ev, sizeof(ev)) == sizeof(ev));
	}

	/* Make the frontbuffer busy and try to flip to itself */
	flip_to_fb(dpy, pipe, &fb[0], ring, "fb[0]");

	/* Repeat for flip to second buffer */
	flip_to_fb(dpy, pipe, &fb[1], ring, "fb[1]");

	do_cleanup_display(dpy);
	igt_remove_fb(dpy->drm_fd, &fb[1]);
	igt_remove_fb(dpy->drm_fd, &fb[0]);

	signal(SIGALRM, SIG_DFL);
}

igt_main
{
	igt_display_t display = { .drm_fd = -1, .n_pipes = I915_MAX_PIPES };
	const struct intel_execution_engine *e;

	igt_skip_on_simulation();

	igt_fixture {
		int fd = drm_open_driver_master(DRIVER_INTEL);

		gem_require_mmap_wc(fd);

		kmstest_set_vt_graphics_mode();
		igt_display_init(&display, fd);
		igt_require(display.n_pipes > 0);
	}

	/* XXX Extend to cover atomic rendering tests to all planes + legacy */

	for (int n = 0; n < I915_MAX_PIPES; n++) {
		errno = 0;

		igt_fixture {
			igt_skip_on(n >= display.n_pipes);
		}

		for (e = intel_execution_engines; e->name; e++) {
			igt_subtest_f("%sflip-%s-%s",
				      e->exec_id == 0 ? "basic-" : "",
				      e->name, kmstest_pipe_name(n)) {
				igt_require(gem_has_ring(display.drm_fd,
							e->exec_id | e->flags));

				test_flip(&display, e->exec_id | e->flags, n);
			}
		}
	}

	igt_fixture {
		igt_display_fini(&display);
	}
}
