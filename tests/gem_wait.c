/*
 * Copyright © 2012 Intel Corporation
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
 *    Ben Widawsky <ben@bwidawsk.net>
 *
 */

#include "igt.h"
#include "igt_vgem.h"

static int __gem_wait(int fd, struct drm_i915_gem_wait *w)
{
	int err;

	err = 0;
	if (igt_ioctl(fd, DRM_IOCTL_I915_GEM_WAIT, w))
		err = -errno;

	return err;
}

static void invalid_flags(int fd)
{
	struct drm_i915_gem_wait wait;

	memset(&wait, 0, sizeof(wait));
	wait.bo_handle = gem_create(fd, 4096);
	wait.timeout_ns = 1;
	/* NOTE: This test intentionally tests for just the next available flag.
	 * Don't "fix" this testcase without the ABI testcases for new flags
	 * first. */
	wait.flags = 1;

	igt_assert_eq(__gem_wait(fd, &wait), -EINVAL);

	gem_close(fd, wait.bo_handle);
}

static void invalid_buf(int fd)
{
	struct drm_i915_gem_wait wait;

	memset(&wait, 0, sizeof(wait));
	igt_assert_eq(__gem_wait(fd, &wait), -ENOENT);
}

#define BUSY 1
#define HANG 2
#define AWAIT 4
#define WRITE 8

struct cork {
	int device;
	uint32_t handle;
	uint32_t fence;
};

static struct cork plug(int fd, unsigned flags)
{
	struct cork c;
	struct vgem_bo bo;
	int dmabuf;

	if ((flags & (WRITE | AWAIT)) == 0)
		return (struct cork){0};

	c.device = drm_open_driver(DRIVER_VGEM);

	bo.width = bo.height = 1;
	bo.bpp = 4;
	vgem_create(c.device, &bo);
	c.fence = vgem_fence_attach(c.device, &bo, VGEM_FENCE_WRITE);

	dmabuf = prime_handle_to_fd(c.device, bo.handle);
	c.handle = prime_fd_to_handle(fd, dmabuf);
	close(dmabuf);

	return c;
}

static void unplug(struct cork *c)
{
	if (!c->device)
		return;

	vgem_fence_signal(c->device, c->fence);
	close(c->device);
}

static void basic(int fd, unsigned engine, unsigned flags)
{
	struct cork cork = plug(fd, flags);
	igt_spin_t *spin = igt_spin_batch_new(fd, 0, engine, cork.handle);
	struct drm_i915_gem_wait wait = {
	       	flags & WRITE ? cork.handle : spin->handle
       	};

	igt_assert_eq(__gem_wait(fd, &wait), -ETIME);

	if (flags & BUSY) {
		struct timespec tv = {};
		int timeout;

		timeout = 120;
		if ((flags & HANG) == 0) {
			igt_spin_batch_set_timeout(spin, NSEC_PER_SEC/2);
			timeout = 1;
		}

		unplug(&cork);
		igt_assert_eq(__gem_wait(fd, &wait), -ETIME);

		while (__gem_wait(fd, &wait) == -ETIME)
			igt_assert(igt_seconds_elapsed(&tv) < timeout);
	} else {
		wait.timeout_ns = NSEC_PER_SEC / 2; /* 0.5s */
		igt_assert_eq(__gem_wait(fd, &wait), -ETIME);
		igt_assert_eq_s64(wait.timeout_ns, 0);

		unplug(&cork);
		wait.timeout_ns = 0;
		igt_assert_eq(__gem_wait(fd, &wait), -ETIME);

		if ((flags & HANG) == 0) {
			igt_spin_batch_set_timeout(spin, NSEC_PER_SEC/2);
			wait.timeout_ns = NSEC_PER_SEC; /* 1.0s */
			igt_assert_eq(__gem_wait(fd, &wait), 0);
			igt_assert(wait.timeout_ns > 0);
		} else {
			wait.timeout_ns = -1;
			igt_assert_eq(__gem_wait(fd, &wait), 0);
			igt_assert(wait.timeout_ns == -1);
		}

		wait.timeout_ns = 0;
		igt_assert_eq(__gem_wait(fd, &wait), 0);
		igt_assert(wait.timeout_ns == 0);
	}

	igt_spin_batch_free(fd, spin);
}

igt_main
{
	const struct intel_execution_engine *e;
	int fd = -1;

	igt_skip_on_simulation();

	igt_fixture {
		fd = drm_open_driver_master(DRIVER_INTEL);
		igt_require_gem(fd);
	}

	igt_subtest("invalid-flags")
		invalid_flags(fd);

	igt_subtest("invalid-buf")
		invalid_buf(fd);

	igt_subtest_group {
		igt_fixture {
			igt_fork_hang_detector(fd);
			igt_fork_signal_helper();
		}

		igt_subtest("basic-busy-all") {
			gem_quiescent_gpu(fd);
			basic(fd, -1, BUSY);
		}
		igt_subtest("basic-wait-all") {
			gem_quiescent_gpu(fd);
			basic(fd, -1, 0);
		}
		igt_subtest("basic-await-all") {
			gem_quiescent_gpu(fd);
			basic(fd, -1, AWAIT);
		}
		igt_subtest("basic-busy-write-all") {
			gem_quiescent_gpu(fd);
			basic(fd, -1, BUSY | WRITE);
		}
		igt_subtest("basic-wait-write-all") {
			gem_quiescent_gpu(fd);
			basic(fd, -1, WRITE);
		}

		for (e = intel_execution_engines; e->name; e++) {
			igt_subtest_group {
				igt_subtest_f("busy-%s", e->name) {
					gem_quiescent_gpu(fd);
					basic(fd, e->exec_id | e->flags, BUSY);
				}
				igt_subtest_f("wait-%s", e->name) {
					gem_quiescent_gpu(fd);
					basic(fd, e->exec_id | e->flags, 0);
				}
				igt_subtest_f("await-%s", e->name) {
					gem_quiescent_gpu(fd);
					basic(fd, e->exec_id | e->flags, AWAIT);
				}
				igt_subtest_f("write-busy-%s", e->name) {
					gem_quiescent_gpu(fd);
					basic(fd, e->exec_id | e->flags, BUSY | WRITE);
				}
				igt_subtest_f("write-wait-%s", e->name) {
					gem_quiescent_gpu(fd);
					basic(fd, e->exec_id | e->flags, WRITE);
				}
			}
		}

		igt_fixture {
			igt_stop_signal_helper();
			igt_stop_hang_detector();
		}
	}

	igt_subtest_group {
		igt_hang_t hang;

		igt_fixture {
			hang = igt_allow_hang(fd, 0, 0);
			igt_fork_signal_helper();
		}

		igt_subtest("hang-busy-all") {
			gem_quiescent_gpu(fd);
			basic(fd, -1, BUSY | HANG);
		}
		igt_subtest("hang-wait-all") {
			gem_quiescent_gpu(fd);
			basic(fd, -1, HANG);
		}

		igt_subtest("hang-busy-write-all") {
			gem_quiescent_gpu(fd);
			basic(fd, -1, BUSY | WRITE | HANG);
		}
		igt_subtest("hang-wait-write-all") {
			gem_quiescent_gpu(fd);
			basic(fd, -1, WRITE | HANG);
		}

		for (e = intel_execution_engines; e->name; e++) {
			igt_subtest_f("hang-busy-%s", e->name) {
				gem_quiescent_gpu(fd);
				basic(fd, e->exec_id | e->flags, HANG | BUSY);
			}
			igt_subtest_f("hang-wait-%s", e->name) {
				gem_quiescent_gpu(fd);
				basic(fd, e->exec_id | e->flags, HANG);
			}
			igt_subtest_f("hang-busy-write-%s", e->name) {
				gem_quiescent_gpu(fd);
				basic(fd, e->exec_id | e->flags, HANG | WRITE | BUSY);
			}
			igt_subtest_f("hang-wait-write-%s", e->name) {
				gem_quiescent_gpu(fd);
				basic(fd, e->exec_id | e->flags, HANG | WRITE);
			}
		}

		igt_fixture {
			igt_stop_signal_helper();
			igt_disallow_hang(fd, hang);
		}
	}

	igt_fixture {
		close(fd);
	}
}
