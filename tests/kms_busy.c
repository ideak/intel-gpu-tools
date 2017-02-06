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

		primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
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

static void sighandler(int sig)
{
}

static void flip_to_fb(igt_display_t *dpy, int pipe,
		       igt_output_t *output,
		       struct igt_fb *fb, unsigned ring,
		       const char *name, bool modeset)
{
	struct pollfd pfd = { .fd = dpy->drm_fd, .events = POLLIN };
	struct timespec tv = { 1, 0 };
	struct drm_event_vblank ev;

	igt_spin_t *t = igt_spin_batch_new(dpy->drm_fd, ring, fb->gem_handle);

	if (modeset) {
		/*
		 * We want to check that a modeset actually waits for the
		 * spin batch to complete, but we keep a bigger timeout for
		 * disable than required for flipping.
		 *
		 * As a result, the GPU reset code may kick in, which we neuter
		 * here to be sure there's no premature completion.
		 */
		igt_set_module_param_int("enable_hangcheck", 0);
	}

	igt_fork(child, 1) {
		igt_assert(gem_bo_busy(dpy->drm_fd, fb->gem_handle));
		if (!modeset)
			do_or_die(drmModePageFlip(dpy->drm_fd,
						  dpy->pipes[pipe].crtc_id, fb->fb_id,
						  DRM_MODE_PAGE_FLIP_EVENT, fb));
		else {
			igt_plane_set_fb(igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY), fb);
			igt_output_set_pipe(output, PIPE_NONE);
			igt_display_commit_atomic(dpy,
						  DRM_MODE_ATOMIC_NONBLOCK |
						  DRM_MODE_PAGE_FLIP_EVENT |
						  DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
		}

		kill(getppid(), SIGALRM);
		igt_assert(gem_bo_busy(dpy->drm_fd, fb->gem_handle));
		igt_assert_f(poll(&pfd, 1, modeset ? 8500 : TIMEOUT) == 0,
			     "flip completed whilst %s was busy [%d]\n",
			     name, gem_bo_busy(dpy->drm_fd, fb->gem_handle));
	}

	igt_assert_f(nanosleep(&tv, NULL) == -1,
		     "flip to %s blocked waiting for busy fb", name);

	igt_waitchildren();

	if (!modeset) {
		tv.tv_sec = 0;
		tv.tv_nsec = (2 * TIMEOUT) * 1000000ULL;
		nanosleep(&tv, NULL);
	}

	igt_spin_batch_end(t);

	igt_assert(read(dpy->drm_fd, &ev, sizeof(ev)) == sizeof(ev));
	igt_assert(poll(&pfd, 1, 0) == 0);

	if (modeset) {
		igt_set_module_param_int("enable_hangcheck", 1);
		dpy->pipes[pipe].mode_blob = 0;
		igt_output_set_pipe(output, pipe);
		igt_display_commit2(dpy, COMMIT_ATOMIC);
	}

	igt_spin_batch_free(dpy->drm_fd, t);
}

static void test_flip(igt_display_t *dpy, unsigned ring, int pipe, bool modeset)
{
	struct igt_fb fb[2];
	int warmup[] = { 0, 1, 0, -1 };
	igt_output_t *output;

	if (modeset)
		igt_require(dpy->is_atomic);

	signal(SIGALRM, sighandler);

	igt_require((output = set_fb_on_crtc(dpy, pipe, &fb[0])));
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
	flip_to_fb(dpy, pipe, output, &fb[0], ring, "fb[0]", modeset);

	/* Repeat for flip to second buffer */
	flip_to_fb(dpy, pipe, output, &fb[1], ring, "fb[1]", modeset);

	do_cleanup_display(dpy);
	igt_remove_fb(dpy->drm_fd, &fb[1]);
	igt_remove_fb(dpy->drm_fd, &fb[0]);

	signal(SIGALRM, SIG_DFL);
}

static void test_atomic_commit_hang(igt_display_t *dpy, igt_plane_t *primary,
				    struct igt_fb *busy_fb, unsigned ring,
				    bool completes_early)
{
	igt_spin_t *t = igt_spin_batch_new(dpy->drm_fd, ring, busy_fb->gem_handle);
	struct pollfd pfd = { .fd = dpy->drm_fd, .events = POLLIN };
	unsigned flags = 0;
	struct drm_event_vblank ev;

	flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;
	flags |= DRM_MODE_ATOMIC_NONBLOCK;
	flags |= DRM_MODE_PAGE_FLIP_EVENT;

	igt_display_commit_atomic(dpy, flags, NULL);

	igt_fork(child, 1) {
		/*
		 * bit of a hack, just set atomic commit to NULL fb to make sure
		 * that we don't wait for the new update to complete.
		 */
		igt_plane_set_fb(primary, NULL);
		igt_display_commit_atomic(dpy, 0, NULL);

		if (completes_early)
			igt_assert(gem_bo_busy(dpy->drm_fd, busy_fb->gem_handle));
		else
			igt_fail_on(gem_bo_busy(dpy->drm_fd, busy_fb->gem_handle));

		igt_assert_f(poll(&pfd, 1, 1) > 0,
			    "nonblocking update completed whilst fb[%d] was still busy [%d]\n",
			    busy_fb->fb_id, gem_bo_busy(dpy->drm_fd, busy_fb->gem_handle));
	}

	igt_waitchildren();

	igt_assert(read(dpy->drm_fd, &ev, sizeof(ev)) == sizeof(ev));

	igt_spin_batch_end(t);
}

static void test_hang(igt_display_t *dpy, unsigned ring,
		      enum pipe pipe, bool modeset, bool hang_newfb)
{
	struct igt_fb fb[2];
	igt_output_t *output;
	igt_plane_t *primary;

	igt_require((output = set_fb_on_crtc(dpy, pipe, &fb[0])));
	igt_display_commit2(dpy, COMMIT_ATOMIC);
	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);

	igt_create_pattern_fb(dpy->drm_fd,
			      fb[0].width, fb[0].height,
			      DRM_FORMAT_XRGB8888,
			      LOCAL_I915_FORMAT_MOD_X_TILED,
			      &fb[1]);

	if (modeset) {
		/* Test modeset disable with hang */
		igt_output_set_pipe(output, PIPE_NONE);
		igt_plane_set_fb(primary, &fb[1]);
		test_atomic_commit_hang(dpy, primary, &fb[hang_newfb], ring, hang_newfb);

		/* Test modeset enable with hang */
		igt_plane_set_fb(primary, &fb[0]);
		igt_output_set_pipe(output, pipe);
		test_atomic_commit_hang(dpy, primary, &fb[!hang_newfb], ring, hang_newfb);
	} else {
		/*
		 * Test what happens with a single hanging pageflip.
		 * This always completes early, because we have some
		 * timeouts taking care of it.
		 */
		igt_plane_set_fb(primary, &fb[1]);
		test_atomic_commit_hang(dpy, primary, &fb[hang_newfb], ring, true);
	}

	do_cleanup_display(dpy);
	igt_remove_fb(dpy->drm_fd, &fb[1]);
	igt_remove_fb(dpy->drm_fd, &fb[0]);
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

				test_flip(&display, e->exec_id | e->flags, n, false);
			}
			igt_subtest_f("%smodeset-%s-%s",
				      e->exec_id == 0 ? "basic-" : "",
				      e->name, kmstest_pipe_name(n)) {
				igt_require(gem_has_ring(display.drm_fd,
							e->exec_id | e->flags));

				test_flip(&display, e->exec_id | e->flags, n, true);
			}

			igt_subtest_group {
				igt_hang_t hang;

				igt_fixture {
					igt_require(display.is_atomic);

					hang = igt_allow_hang(display.drm_fd, 0, 0);
				}

				igt_subtest_f("extended-pageflip-hang-oldfb-%s-%s",
					      e->name, kmstest_pipe_name(n)) {
					igt_require(gem_has_ring(display.drm_fd,
								e->exec_id | e->flags));

					test_hang(&display, e->exec_id | e->flags, n, false, false);
				}

				igt_subtest_f("extended-pageflip-hang-newfb-%s-%s",
					      e->name, kmstest_pipe_name(n)) {
					igt_require(gem_has_ring(display.drm_fd,
								e->exec_id | e->flags));

					test_hang(&display, e->exec_id | e->flags, n, false, true);
				}

				igt_subtest_f("extended-modeset-hang-oldfb-%s-%s",
					      e->name, kmstest_pipe_name(n)) {
					igt_require(gem_has_ring(display.drm_fd,
								e->exec_id | e->flags));

					test_hang(&display, e->exec_id | e->flags, n, true, false);
				}

				igt_subtest_f("extended-modeset-hang-newfb-%s-%s",
					      e->name, kmstest_pipe_name(n)) {
					igt_require(gem_has_ring(display.drm_fd,
								e->exec_id | e->flags));

					test_hang(&display, e->exec_id | e->flags, n, true, true);
				}

				igt_subtest_f("extended-modeset-hang-oldfb-with-reset-%s-%s",
					      e->name, kmstest_pipe_name(n)) {
					igt_require(gem_has_ring(display.drm_fd,
								e->exec_id | e->flags));
					igt_set_module_param_int("force_reset_modeset_test", 1);

					test_hang(&display, e->exec_id | e->flags, n, true, false);

					igt_set_module_param_int("force_reset_modeset_test", 0);
				}

				igt_subtest_f("extended-modeset-hang-newfb-with-reset-%s-%s",
					      e->name, kmstest_pipe_name(n)) {
					igt_require(gem_has_ring(display.drm_fd,
								e->exec_id | e->flags));
					igt_set_module_param_int("force_reset_modeset_test", 1);

					test_hang(&display, e->exec_id | e->flags, n, true, true);

					igt_set_module_param_int("force_reset_modeset_test", 0);
				}

				igt_fixture {
					igt_disallow_hang(display.drm_fd, hang);
				}
			}
		}
	}

	igt_fixture {
		igt_display_fini(&display);
	}
}
