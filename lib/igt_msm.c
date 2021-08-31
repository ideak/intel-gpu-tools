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

#include <assert.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>

#include "drmtest.h"
#include "igt_aux.h"
#include "igt_core.h"
#include "igt_msm.h"
#include "ioctl_wrappers.h"

/**
 * SECTION:igt_msm
 * @short_description: msm support library
 * @title: msm
 * @include: igt_msm.h
 *
 * This library provides various auxiliary helper functions for writing msm
 * tests.
 */

static uint64_t
get_param(struct msm_device *dev, uint32_t pipe, uint32_t param)
{
	struct drm_msm_param req = {
			.pipe = pipe,
			.param = param,
	};

	do_ioctl(dev->fd, DRM_IOCTL_MSM_GET_PARAM, &req);

	return req.value;
}

/**
 * igt_msm_dev_open:
 *
 * Open the msm drm device.
 */
struct msm_device *
igt_msm_dev_open(void)
{
	struct msm_device *dev = calloc(1, sizeof(*dev));

	dev->fd = drm_open_driver_render(DRIVER_MSM);
	dev->gen = (get_param(dev, MSM_PIPE_3D0, MSM_PARAM_CHIP_ID) >> 24) & 0xff;

	return dev;
}

/**
 * igt_msm_dev_close:
 * @dev: the device to close
 *
 * Close the msm drm device.
 */
void
igt_msm_dev_close(struct msm_device *dev)
{
	if (!dev)
		return;
	close(dev->fd);
	free(dev);
}

/**
 * igt_msm_bo_new:
 * @dev: the device to allocate the BO from
 * @size: the requested BO size in bytes
 * @flags: bitmask of MSM_BO_x
 *
 * Allocate a buffer object of the requested size.
 */
struct msm_bo *
igt_msm_bo_new(struct msm_device *dev, size_t size, uint32_t flags)
{
	struct msm_bo *bo = calloc(1, sizeof(*bo));

	struct drm_msm_gem_new req = {
			.size = size,
			.flags = flags,
	};

	bo->dev = dev;
	bo->size = size;

	do_ioctl(dev->fd, DRM_IOCTL_MSM_GEM_NEW, &req);

	bo->handle = req.handle;

	return bo;
}

/**
 * igt_msm_bo_free:
 * @bo: the BO to free
 *
 * Free a buffer object
 */
void
igt_msm_bo_free(struct msm_bo *bo)
{
	if (!bo)
		return;
	if (bo->map)
		munmap(bo->map, bo->size);
	gem_close(bo->dev->fd, bo->handle);
	free(bo);
}

/**
 * igt_msm_bo_map:
 * @bo: the BO to map
 *
 * Returns a pointer to mmap'd buffer.
 */
void *
igt_msm_bo_map(struct msm_bo *bo)
{
	if (!bo->map) {
		struct drm_msm_gem_info req = {
				.handle = bo->handle,
				.info = MSM_INFO_GET_OFFSET,
		};
		void *ptr;

		do_ioctl(bo->dev->fd, DRM_IOCTL_MSM_GEM_INFO, &req);

		ptr = mmap(0, bo->size, PROT_READ | PROT_WRITE, MAP_SHARED,
				bo->dev->fd, req.value);
		if (ptr == MAP_FAILED)
			return NULL;

		bo->map = ptr;
	}
	return bo->map;
}

/**
 * igt_msm_pipe_open:
 * @dev: the device to create a submitqueue/pipe against
 * @prio: the requested priority, from 0 (highest) to MSM_PARAM_PRIORITIES-1
 *        (lowest)
 *
 * Allocate a pipe/submitqueue against which cmdstream may be submitted.
 */
struct msm_pipe *
igt_msm_pipe_open(struct msm_device *dev, uint32_t prio)
{
	struct msm_pipe *pipe = calloc(1, sizeof(*pipe));
	struct drm_msm_submitqueue req = {
			.flags = 0,
			.prio = prio,
	};

	pipe->dev = dev;
	pipe->pipe = MSM_PIPE_3D0;

	/* Note that kernels prior to v4.15 did not support submitqueues.
	 * Mesa maintains support for older kernels, but IGT does not need
	 * to.
	 */
	do_ioctl(dev->fd, DRM_IOCTL_MSM_SUBMITQUEUE_NEW, &req);

	pipe->submitqueue_id = req.id;

	return pipe;
}

/**
 * igt_msm_pipe_close:
 * @pipe: the pipe to close
 *
 * Close a pipe
 */
void
igt_msm_pipe_close(struct msm_pipe *pipe)
{
	if (!pipe)
		return;
	do_ioctl(pipe->dev->fd, DRM_IOCTL_MSM_SUBMITQUEUE_CLOSE, &pipe->submitqueue_id);
	free(pipe);
}
