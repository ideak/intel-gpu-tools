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
#include <stddef.h>
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

static int create_ext_ioctl(int i915,
			    struct drm_i915_gem_context_create_ext *arg)
{
	int err;

	err = 0;
	if (igt_ioctl(i915, DRM_IOCTL_I915_GEM_CONTEXT_CREATE_EXT, arg)) {
		err = -errno;
		igt_assume(err);
	}

	errno = 0;
	return err;
}

/**
 * gem_has_contexts:
 * @fd: open i915 drm file descriptor
 *
 * Queries whether context creation is supported or not.
 *
 * Returns: Context creation availability.
 */
bool gem_has_contexts(int fd)
{
	uint32_t ctx_id = 0;
	int err;

	err = __gem_context_create(fd, &ctx_id);
	if (!err)
		gem_context_destroy(fd, ctx_id);

	return !err;
}

/**
 * gem_require_contexts:
 * @fd: open i915 drm file descriptor
 *
 * This helper will automatically skip the test on platforms where context
 * support is not available.
 */
void gem_require_contexts(int fd)
{
	igt_require(gem_has_contexts(fd));
}

int __gem_context_create(int fd, uint32_t *ctx_id)
{
       struct drm_i915_gem_context_create create;
       int err = 0;

       memset(&create, 0, sizeof(create));
       if (igt_ioctl(fd, DRM_IOCTL_I915_GEM_CONTEXT_CREATE, &create) == 0) {
               *ctx_id = create.ctx_id;
       } else {
	       err = -errno;
	       igt_assume(err != 0);
       }

       errno = 0;
       return err;
}

/**
 * __gem_context_create_ext:
 * @fd: open i915 drm file descriptor
 * @flags: context create flags
 * @extensions: first extension struct, or 0 for no extensions
 * @ctx_id: on success, the context ID is written here
 *
 * Creates a new GEM context with flags and extensions.  If no flags or
 * extensions are required, it's the same as __gem_context_create and works
 * on older kernels.
 */
int __gem_context_create_ext(int fd, uint32_t flags, uint64_t extensions,
			     uint32_t *ctx_id)
{
	struct drm_i915_gem_context_create_ext ctx_create;
	int err = 0;

	if (!flags && !extensions)
		return __gem_context_create(fd, ctx_id);

	memset(&ctx_create, 0, sizeof(ctx_create));
	ctx_create.flags = flags;
	if (extensions) {
		ctx_create.flags |= I915_CONTEXT_CREATE_FLAGS_USE_EXTENSIONS;
		ctx_create.extensions = extensions;
	}

	err = create_ext_ioctl(fd, &ctx_create);
	if (!err)
		*ctx_id = ctx_create.ctx_id;

	return err;
}

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
	uint32_t ctx_id;

	igt_assert_eq(__gem_context_create(fd, &ctx_id), 0);
	igt_assert(ctx_id != 0);

	return ctx_id;
}

/**
 * gem_context_create_ext:
 * @fd: open i915 drm file descriptor
 * @flags: context create flags
 * @extensions: first extension struct, or 0 for no extensions
 *
 * Creates a new GEM context with flags and extensions.  If no flags or
 * extensions are required, it's the same as gem_context_create and works
 * on older kernels.
 *
 * Returns: The id of the allocated context.
 */
uint32_t gem_context_create_ext(int fd, uint32_t flags, uint64_t extensions)
{
	uint32_t ctx_id;

	igt_assert_eq(__gem_context_create_ext(fd, flags, extensions, &ctx_id), 0);
	igt_assert(ctx_id != 0);

	return ctx_id;
}

int __gem_context_destroy(int fd, uint32_t ctx_id)
{
	struct drm_i915_gem_context_destroy destroy = { ctx_id };
	int err = 0;

	if (igt_ioctl(fd, DRM_IOCTL_I915_GEM_CONTEXT_DESTROY, &destroy)) {
		err = -errno;
		igt_assume(err);
	}

	errno = 0;
	return err;
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
	igt_assert_eq(__gem_context_destroy(fd, ctx_id), 0);
}

static bool __gem_context_has_flag(int i915, unsigned int flags)
{
	uint32_t ctx = 0;

	__gem_context_create_ext(i915, flags, 0, &ctx);
	if (ctx)
		gem_context_destroy(i915, ctx);

	errno = 0;
	return ctx;
}

bool gem_context_has_single_timeline(int i915)
{
	return __gem_context_has_flag(i915, I915_CONTEXT_CREATE_FLAGS_SINGLE_TIMELINE);
}

int __gem_context_get_param(int fd, struct drm_i915_gem_context_param *p)
{
	int err = 0;

	if (igt_ioctl(fd, DRM_IOCTL_I915_GEM_CONTEXT_GETPARAM, p)) {
		err = -errno;
		igt_assume(err);
	}

	errno = 0;
	return err;
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
	igt_assert_eq(__gem_context_get_param(fd, p), 0);
}

int __gem_context_set_param(int fd, struct drm_i915_gem_context_param *p)
{
	int err = 0;

	if (igt_ioctl(fd, DRM_IOCTL_I915_GEM_CONTEXT_SETPARAM, p)) {
		err = -errno;
		igt_assume(err);
	}

	errno = 0;
	return err;
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
	igt_assert_eq(__gem_context_set_param(fd, p), 0);
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
	struct drm_i915_gem_context_param p = { .param = param };

	igt_require(__gem_context_get_param(fd, &p) == 0);
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
	struct drm_i915_gem_context_param p = {
		.ctx_id = ctx_id,
		.param = DRM_I915_CONTEXT_PARAM_PRIORITY,
		.value = prio,
	};

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
	igt_assert_eq(__gem_context_set_priority(fd, ctx_id, prio), 0);
}

/**
 * __gem_context_set_persistence:
 * @i915: open i915 drm file descriptor
 * @ctx: i915 context id
 * @state: desired persistence
 *
 * Declare whether this context is allowed to persist after closing until
 * its requests are complete (persistent=true) or if it should be
 * immediately reaped on closing and its requests cancelled
 * (persistent=false).
 *
 * Returns: An integer equal to zero for success and negative for failure
 */
int __gem_context_set_persistence(int i915, uint32_t ctx, bool state)
{
	struct drm_i915_gem_context_param p = {
		.ctx_id = ctx,
		.param = I915_CONTEXT_PARAM_PERSISTENCE,
		.value = state,
	};

	return __gem_context_set_param(i915, &p);
}

/**
 * __gem_context_set_persistence:
 * @i915: open i915 drm file descriptor
 * @ctx: i915 context id
 * @state: desired persistence
 *
 * Like __gem_context_set_persistence(), except we assert on failure.
 */
void gem_context_set_persistence(int i915, uint32_t ctx, bool state)
{
	igt_assert_eq(__gem_context_set_persistence(i915, ctx, state), 0);
}

bool gem_context_has_persistence(int i915)
{
	struct drm_i915_gem_context_param param = {
		.param = I915_CONTEXT_PARAM_PERSISTENCE,
	};

	return __gem_context_get_param(i915, &param) == 0;
}

bool gem_context_has_engine(int fd, uint32_t ctx, uint64_t engine)
{
	struct drm_i915_gem_exec_object2 exec = {};
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(&exec),
		.buffer_count = 1,
		.flags = engine,
		.rsvd1 = ctx,
	};

	/*
	 * 'engine' value can either store an execbuf engine selector
	 * or a context map index; for the latter case we do not expect
	 * to have any value at bit 13 and 14 (BSD1/2 selector),
	 * therefore, we assume that the following check is safe and it
	 * wouldn't produce any result.
	 */
	if ((engine & ~(3<<13)) == I915_EXEC_BSD) {
		if (engine & (2 << 13) && !gem_has_bsd2(fd))
			return false;
	}

	return __gem_execbuf(fd, &execbuf) == -ENOENT;
}

uint32_t gem_context_create_for_engine(int i915, unsigned int class, unsigned int inst)
{
	I915_DEFINE_CONTEXT_PARAM_ENGINES(engines, 1) = {
		.engines = { { .engine_class = class, .engine_instance = inst } }
	};
	struct drm_i915_gem_context_create_ext_setparam p_engines = {
		.base = {
			.name = I915_CONTEXT_CREATE_EXT_SETPARAM,
			.next_extension = 0, /* end of chain */
		},
		.param = {
			.param = I915_CONTEXT_PARAM_ENGINES,
			.value = to_user_pointer(&engines),
			.size = sizeof(engines),
		},
	};
	struct drm_i915_gem_context_create_ext create = {
		.flags = I915_CONTEXT_CREATE_FLAGS_USE_EXTENSIONS,
		.extensions = to_user_pointer(&p_engines),
	};

	igt_assert_eq(create_ext_ioctl(i915, &create), 0);
	igt_assert_neq(create.ctx_id, 0);
	return create.ctx_id;
}

static size_t sizeof_param_engines(int count)
{
	return offsetof(struct i915_context_param_engines, engines[count]);
}

static size_t sizeof_load_balance(int i)
{
	return offsetof(struct i915_context_engines_load_balance, engines[i]);
}

#define alloca0(sz) ({ size_t sz__ = (sz); memset(alloca(sz__), 0, sz__); })

uint32_t gem_context_create_for_class(int i915,
				      unsigned int class,
				      unsigned int *count)
{
	I915_DEFINE_CONTEXT_PARAM_ENGINES(engines, I915_EXEC_RING_MASK + 1);
	struct drm_i915_gem_context_param p = {
		.ctx_id = gem_context_create(i915),
		.param = I915_CONTEXT_PARAM_ENGINES,
		.value = to_user_pointer(&engines)
	};
	int i;

	memset(&engines, 0, sizeof(engines));
	for (i = 0; i < I915_EXEC_RING_MASK + 1; i++) {
		engines.engines[i].engine_class = class;
		engines.engines[i].engine_instance = i;
		p.size = sizeof_param_engines(i + 1);
		if (__gem_context_set_param(i915, &p))
			break;
	}
	if (i == 0) {
		gem_context_destroy(i915, p.ctx_id);
		return 0;
	}
	if (i > 1) {
		struct i915_context_engines_load_balance *balancer =
			alloca0(sizeof_load_balance(i));

		balancer->base.name = I915_CONTEXT_ENGINES_EXT_LOAD_BALANCE;
		balancer->num_siblings = i;
		memcpy(balancer->engines,
		       engines.engines,
		       i * sizeof(*engines.engines));

		engines.extensions = to_user_pointer(balancer);
		engines.engines[0].engine_class = I915_ENGINE_CLASS_INVALID;
		engines.engines[0].engine_instance = I915_ENGINE_CLASS_INVALID_NONE;

		p.size = sizeof_param_engines(1);
		p.value = to_user_pointer(&engines);
		gem_context_set_param(i915, &p);
	}

	*count = i;
	return p.ctx_id;
}
