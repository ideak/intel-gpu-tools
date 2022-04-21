/*
 * Copyright © 2021 Google, Inc.
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
#include "igt_msm.h"

igt_main
{
	struct msm_device *dev = NULL;
	struct msm_pipe *pipe = NULL;
	struct msm_bo *a = NULL, *b = NULL;

	igt_fixture {
		dev = igt_msm_dev_open();
		pipe = igt_msm_pipe_open(dev, 0);
		a = igt_msm_bo_new(dev, 0x1000, MSM_BO_WC);
		b = igt_msm_bo_new(dev, 0x1000, MSM_BO_WC);
	}

	igt_describe("Check that a valid empty submit succeeds");
	igt_subtest("empty-submit") {
		struct drm_msm_gem_submit req = {
				.flags   = pipe->pipe,
				.queueid = pipe->submitqueue_id,
		};
		do_ioctl(dev->fd, DRM_IOCTL_MSM_GEM_SUBMIT, &req);
	}

	igt_describe("Check that submit with invalid submitqueue id fails");
	igt_subtest("invalid-queue-submit") {
		struct drm_msm_gem_submit req = {
				.flags   = pipe->pipe,
				.queueid = 0x1234,
		};
		do_ioctl_err(dev->fd, DRM_IOCTL_MSM_GEM_SUBMIT, &req, ENOENT);
	}

	igt_describe("Check that submit with invalid flags fails");
	igt_subtest("invalid-flags-submit") {
		struct drm_msm_gem_submit req = {
				.flags   = 0x1234,
				.queueid = pipe->submitqueue_id,
		};
		do_ioctl_err(dev->fd, DRM_IOCTL_MSM_GEM_SUBMIT, &req, EINVAL);
	}

	igt_describe("Check that submit with invalid in-fence fd fails");
	igt_subtest("invalid-in-fence-submit") {
		struct drm_msm_gem_submit req = {
				.flags   = pipe->pipe | MSM_SUBMIT_FENCE_FD_IN,
				.queueid = pipe->submitqueue_id,
				.fence_fd = dev->fd,  /* This is not a fence fd! */
		};
		do_ioctl_err(dev->fd, DRM_IOCTL_MSM_GEM_SUBMIT, &req, EINVAL);
	}

	igt_describe("Check that submit with duplicate bo fails");
	igt_subtest("invalid-duplicate-bo-submit") {
		struct drm_msm_gem_submit_bo bos[] = {
			[0] = {
				.handle     = a->handle,
				.flags      = MSM_SUBMIT_BO_READ,
			},
			[1] = {
				.handle     = b->handle,
				.flags      = MSM_SUBMIT_BO_READ,
			},
			[2] = {
				/* this is invalid.. there should not be two entries
				 * for the same bo, instead a single entry w/ all
				 * usage flags OR'd together should be used.  Kernel
				 * should catch this, and return an error code after
				 * cleaning up properly (not leaking any bo's)
				 */
				.handle     = a->handle,
				.flags      = MSM_SUBMIT_BO_WRITE,
			},
		};
		struct drm_msm_gem_submit req = {
				.flags   = pipe->pipe,
				.queueid = pipe->submitqueue_id,
				.nr_bos  = ARRAY_SIZE(bos),
				.bos     = VOID2U64(bos),
		};
		do_ioctl_err(dev->fd, DRM_IOCTL_MSM_GEM_SUBMIT, &req, EINVAL);
	}

	igt_describe("Check that submit with cmdstream referencing an invalid bo fails");
	igt_subtest("invalid-cmd-idx-submit") {
		struct drm_msm_gem_submit_cmd cmds[] = {
			[0] = {
				.type       = MSM_SUBMIT_CMD_BUF,
				.submit_idx = 0,      /* bos[0] does not exist */
				.size       = 4 * 4,  /* 4 dwords in cmdbuf */
			},
		};
		struct drm_msm_gem_submit req = {
				.flags   = pipe->pipe,
				.queueid = pipe->submitqueue_id,
				.nr_cmds    = ARRAY_SIZE(cmds),
				.cmds       = VOID2U64(cmds),
		};
		do_ioctl_err(dev->fd, DRM_IOCTL_MSM_GEM_SUBMIT, &req, EINVAL);
	}

	igt_describe("Check that submit with invalid cmdstream type fails");
	igt_subtest("invalid-cmd-type-submit") {
		struct drm_msm_gem_submit_bo bos[] = {
			[0] = {
				.handle     = a->handle,
				.flags      = MSM_SUBMIT_BO_READ,
			},
		};
		struct drm_msm_gem_submit_cmd cmds[] = {
			[0] = {
				.type       = 0x1234,
				.submit_idx = 0,
				.size       = 4 * 4,  /* 4 dwords in cmdbuf */
			},
		};
		struct drm_msm_gem_submit req = {
				.flags   = pipe->pipe,
				.queueid = pipe->submitqueue_id,
				.nr_cmds    = ARRAY_SIZE(cmds),
				.cmds       = VOID2U64(cmds),
				.nr_bos  = ARRAY_SIZE(bos),
				.bos     = VOID2U64(bos),
		};
		do_ioctl_err(dev->fd, DRM_IOCTL_MSM_GEM_SUBMIT, &req, EINVAL);
	}

	igt_describe("Check that a valid non-empty submit succeeds");
	igt_subtest("valid-submit") {
		struct drm_msm_gem_submit_bo bos[] = {
			[0] = {
				.handle     = a->handle,
				.flags      = MSM_SUBMIT_BO_READ,
			},
		};
		struct drm_msm_gem_submit_cmd cmds[] = {
			[0] = {
				.type       = MSM_SUBMIT_CMD_BUF,
				.submit_idx = 0,
				.size       = 4 * 4,  /* 4 dwords in cmdbuf */
			},
		};
		struct drm_msm_gem_submit req = {
				.flags   = pipe->pipe,
				.queueid = pipe->submitqueue_id,
				.nr_cmds    = ARRAY_SIZE(cmds),
				.cmds       = VOID2U64(cmds),
				.nr_bos  = ARRAY_SIZE(bos),
				.bos     = VOID2U64(bos),
		};
		uint32_t *cmdstream = igt_msm_bo_map(a);
		if (dev->gen >= 5) {
			*(cmdstream++) = pm4_pkt7_hdr(CP_NOP, 3);
		} else {
			*(cmdstream++) = pm4_pkt3_hdr(CP_NOP, 3);
		}
		*(cmdstream++) = 0;
		*(cmdstream++) = 0;
		*(cmdstream++) = 0;

		do_ioctl(dev->fd, DRM_IOCTL_MSM_GEM_SUBMIT, &req);
	}

	igt_fixture {
		igt_msm_bo_free(a);
		igt_msm_bo_free(b);
		igt_msm_pipe_close(pipe);
		igt_msm_dev_close(dev);
	}
}
