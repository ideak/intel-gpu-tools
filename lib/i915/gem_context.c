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
 */

#include <errno.h>
#include <string.h>

#include "ioctl_wrappers.h"
#include "drmtest.h"

#include "i915/gem_context.h"

/**
 * SECTION:gem_context
 * @short_description: Helpers for dealing with contexts
 * @title: GEM Context
 *
 * This helper library contains functions used for handling gem contexts.
 * Conceptually, gem contexts are similar to their CPU counterparts, in that
 * they are a mix of software and hardware features allowing to isolate some
 * aspects of task execution. Initially it was just a matter of maintaining
 * separate state for each context, but more features were added, some
 * improving contexts isolation (per-context address space), some are just
 * software features improving submission model (context priority).
 */

/**
 * gem_context_create:
 * @fd: open i915 drm file descriptor
 *
 * This wraps the CONTEXT_CREATE ioctl, which is used to allocate a new
 * context. Note that similarly to gem_set_caching() this wrapper skips on
 * kernels and platforms where context support is not available.
 *
 * Returns: The id of the allocated context.
 */
uint32_t gem_context_create(int fd)
{
	struct drm_i915_gem_context_create create;

	memset(&create, 0, sizeof(create));
	if (igt_ioctl(fd, DRM_IOCTL_I915_GEM_CONTEXT_CREATE, &create)) {
		int err = -errno;
		igt_skip_on(err == -ENODEV || errno == -EINVAL);
		igt_assert_eq(err, 0);
	}
	igt_assert(create.ctx_id != 0);
	errno = 0;

	return create.ctx_id;
}

int __gem_context_destroy(int fd, uint32_t ctx_id)
{
	struct drm_i915_gem_context_destroy destroy;
	int ret;

	memset(&destroy, 0, sizeof(destroy));
	destroy.ctx_id = ctx_id;

	ret = igt_ioctl(fd, DRM_IOCTL_I915_GEM_CONTEXT_DESTROY, &destroy);
	if (ret)
		return -errno;
	return 0;
}

/**
 * gem_context_destroy:
 * @fd: open i915 drm file descriptor
 * @ctx_id: i915 context id
 *
 * This wraps the CONTEXT_DESTROY ioctl, which is used to free a context.
 */
void gem_context_destroy(int fd, uint32_t ctx_id)
{
	struct drm_i915_gem_context_destroy destroy;

	memset(&destroy, 0, sizeof(destroy));
	destroy.ctx_id = ctx_id;

	do_ioctl(fd, DRM_IOCTL_I915_GEM_CONTEXT_DESTROY, &destroy);
}

int __gem_context_get_param(int fd, struct drm_i915_gem_context_param *p)
{
	if (igt_ioctl(fd, DRM_IOCTL_I915_GEM_CONTEXT_GETPARAM, p))
		return -errno;

	errno = 0;
	return 0;
}

/**
 * gem_context_get_param:
 * @fd: open i915 drm file descriptor
 * @p: i915 context parameter
 *
 * This wraps the CONTEXT_GET_PARAM ioctl, which is used to get a context
 * parameter.
 */
void gem_context_get_param(int fd, struct drm_i915_gem_context_param *p)
{
	igt_assert(__gem_context_get_param(fd, p) == 0);
}


int __gem_context_set_param(int fd, struct drm_i915_gem_context_param *p)
{
	if (igt_ioctl(fd, DRM_IOCTL_I915_GEM_CONTEXT_SETPARAM, p))
		return -errno;

	errno = 0;
	return 0;
}
/**
 * gem_context_set_param:
 * @fd: open i915 drm file descriptor
 * @p: i915 context parameter
 *
 * This wraps the CONTEXT_SET_PARAM ioctl, which is used to set a context
 * parameter.
 */
void gem_context_set_param(int fd, struct drm_i915_gem_context_param *p)
{
	igt_assert(__gem_context_set_param(fd, p) == 0);
}

/**
 * gem_context_require_param:
 * @fd: open i915 drm file descriptor
 * @param: i915 context parameter
 *
 * Feature test macro to query whether context parameter support for @param
 * is available. Automatically skips through igt_require() if not.
 */
void gem_context_require_param(int fd, uint64_t param)
{
	struct drm_i915_gem_context_param p;

	p.ctx_id = 0;
	p.param = param;
	p.value = 0;
	p.size = 0;

	igt_require(igt_ioctl(fd, DRM_IOCTL_I915_GEM_CONTEXT_GETPARAM, &p) == 0);
}

void gem_context_require_bannable(int fd)
{
	static int has_ban_period = -1;
	static int has_bannable = -1;

	if (has_bannable < 0) {
		struct drm_i915_gem_context_param p;

		p.ctx_id = 0;
		p.param = I915_CONTEXT_PARAM_BANNABLE;
		p.value = 0;
		p.size = 0;

		has_bannable = igt_ioctl(fd, DRM_IOCTL_I915_GEM_CONTEXT_GETPARAM, &p) == 0;
	}

	if (has_ban_period < 0) {
		struct drm_i915_gem_context_param p;

		p.ctx_id = 0;
		p.param = I915_CONTEXT_PARAM_BAN_PERIOD;
		p.value = 0;
		p.size = 0;

		has_ban_period = igt_ioctl(fd, DRM_IOCTL_I915_GEM_CONTEXT_GETPARAM, &p) == 0;
	}

	igt_require(has_ban_period || has_bannable);
}

#define DRM_I915_CONTEXT_PARAM_PRIORITY 0x6

/**
 * __gem_context_set_priority:
 * @fd: open i915 drm file descriptor
 * @ctx_id: i915 context id
 * @prio: desired context priority
 *
 * This function modifies priority property of the context.
 * It is used by the scheduler to decide on the ordering of requests submitted
 * to the hardware.
 *
 * Returns: An integer equal to zero for success and negative for failure
 */
int __gem_context_set_priority(int fd, uint32_t ctx_id, int prio)
{
	struct drm_i915_gem_context_param p;

	memset(&p, 0, sizeof(p));
	p.ctx_id = ctx_id;
	p.size = 0;
	p.param = DRM_I915_CONTEXT_PARAM_PRIORITY;
	p.value = prio;

	return __gem_context_set_param(fd, &p);
}

/**
 * gem_context_set_priority:
 * @fd: open i915 drm file descriptor
 * @ctx_id: i915 context id
 * @prio: desired context priority
 *
 * Like __gem_context_set_priority(), except we assert on failure.
 */
void gem_context_set_priority(int fd, uint32_t ctx_id, int prio)
{
	igt_assert(__gem_context_set_priority(fd, ctx_id, prio) == 0);
}
