/*
 * Copyright © 2021 Intel Corporation
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

#include "config.h"

#include <stdio.h>
#include <unistd.h>
#include <sched.h>

#include "i915/gem.h"
#include "igt.h"
#include "igt_params.h"
#include "sw_sync.h"

#define EWATCHDOG EINTR

static struct drm_i915_query_engine_info *__engines__;

static int __i915_query(int fd, struct drm_i915_query *q)
{
	if (igt_ioctl(fd, DRM_IOCTL_I915_QUERY, q))
		return -errno;
	return 0;
}

static int
__i915_query_items(int fd, struct drm_i915_query_item *items, uint32_t n_items)
{
	struct drm_i915_query q = {
		.num_items = n_items,
		.items_ptr = to_user_pointer(items),
	};
	return __i915_query(fd, &q);
}

#define i915_query_items(fd, items, n_items) do { \
		igt_assert_eq(__i915_query_items(fd, items, n_items), 0); \
		errno = 0; \
	} while (0)

static unsigned int default_timeout_wait_s;
static const unsigned int watchdog_us = 500 * 1000;

static unsigned int
wait_timeout(int i915, igt_spin_t **spin, unsigned int num_engines,
	     unsigned int wait_us, unsigned int expect)
{
	unsigned int count_idle = 0, count_fence = 0, count_started = 0, i;
	bool started[num_engines];

	memset(started, 0, sizeof(started));

	while (count_started < num_engines) {
		for (i = 0; i < num_engines; i++) {
			if (started[i])
				continue;

			if (igt_spin_has_started(spin[i])) {
				started[i] = true;
				count_started++;
			}
		}
	}

	igt_until_timeout(DIV_ROUND_UP(wait_us, USEC_PER_SEC)) {
		usleep(watchdog_us / 2);

		for (i = 0, count_idle = 0; i < num_engines; i++) {
			if (!gem_bo_busy(i915, spin[i]->handle))
				count_idle++;
		}

		for (i = 0, count_fence = 0; i < num_engines; i++) {
			if (sync_fence_status(spin[i]->out_fence))
				count_fence++;
		}

		if (count_idle == num_engines)
			break;
	}

	if (count_idle < expect) {
		for (i = 0; i < num_engines; i++) {
			if (gem_bo_busy(i915, spin[i]->handle))
				igt_warn("Request %u/%u not cancelled!\n",
					 i + 1, num_engines);
		}
	}

	if (count_fence < expect) {
		for (i = 0; i < num_engines; i++) {
			if (!sync_fence_status(spin[i]->out_fence))
				igt_warn("Fence %u/%u not timed out!\n",
					 i + 1, num_engines);
		}
	}

	igt_assert_eq(count_idle, count_fence);

	return count_fence;
}

static unsigned int spin_flags(void)
{
	return IGT_SPIN_POLL_RUN | IGT_SPIN_FENCE_OUT;
}

static void physical(int i915)
{
	const unsigned int wait_us = default_timeout_wait_s * USEC_PER_SEC;
	unsigned int num_engines = __engines__->num_engines, i, count;
	const struct intel_execution_engine2 *e;
	unsigned int expect = num_engines;
	igt_spin_t *spin[num_engines];

	i = 0;
	__for_each_physical_engine(i915, e) {
		spin[i] = igt_spin_new(i915,
				       .engine = e->flags,
				       .flags = spin_flags());
		i++;
	}

	count = wait_timeout(i915, spin, num_engines, wait_us, expect);

	for (i = 0; i < num_engines; i++)
		igt_spin_free(i915, spin[i]);

	igt_assert_eq(count, expect);
}

static struct i915_engine_class_instance *
list_engines(unsigned int class, unsigned int *out)
{
	struct i915_engine_class_instance *ci;
	unsigned int count = 0, size = 64, i;

	ci = malloc(size * sizeof(*ci));
	igt_assert(ci);

	for (i = 0; i < __engines__->num_engines; i++) {
		struct drm_i915_engine_info *engine =
			(struct drm_i915_engine_info *)&__engines__->engines[i];

		if (class != engine->engine.engine_class)
			continue;

		if (count == size) {
			size *= 2;
			ci = realloc(ci, size * sizeof(*ci));
			igt_assert(ci);
		}

		ci[count++] = (struct i915_engine_class_instance){
			.engine_class = class,
			.engine_instance = engine->engine.engine_instance,
		};
	}

	if (!count) {
		free(ci);
		ci = NULL;
	}

	*out = count;
	return ci;
}

static size_t sizeof_load_balance(int count)
{
	return offsetof(struct i915_context_engines_load_balance,
			engines[count]);
}

static size_t sizeof_param_engines(int count)
{
	return offsetof(struct i915_context_param_engines,
			engines[count]);
}

#define alloca0(sz) ({ size_t sz__ = (sz); memset(alloca(sz__), 0, sz__); })

static int __set_load_balancer(int i915, uint32_t ctx,
			       const struct i915_engine_class_instance *ci,
			       unsigned int count,
			       void *ext)
{
	struct i915_context_engines_load_balance *balancer =
		alloca0(sizeof_load_balance(count));
	struct i915_context_param_engines *engines =
		alloca0(sizeof_param_engines(count + 1));
	struct drm_i915_gem_context_param p = {
		.ctx_id = ctx,
		.param = I915_CONTEXT_PARAM_ENGINES,
		.size = sizeof_param_engines(count + 1),
		.value = to_user_pointer(engines)
	};

	balancer->base.name = I915_CONTEXT_ENGINES_EXT_LOAD_BALANCE;
	balancer->base.next_extension = to_user_pointer(ext);

	igt_assert(count);
	balancer->num_siblings = count;
	memcpy(balancer->engines, ci, count * sizeof(*ci));

	engines->extensions = to_user_pointer(balancer);
	engines->engines[0].engine_class =
		I915_ENGINE_CLASS_INVALID;
	engines->engines[0].engine_instance =
		I915_ENGINE_CLASS_INVALID_NONE;
	memcpy(engines->engines + 1, ci, count * sizeof(*ci));

	return __gem_context_set_param(i915, &p);
}

static void set_load_balancer(int i915, uint32_t ctx,
			      const struct i915_engine_class_instance *ci,
			      unsigned int count,
			      void *ext)
{
	igt_assert_eq(__set_load_balancer(i915, ctx, ci, count, ext), 0);
}

static void ctx_set_vm(int i915, uint32_t ctx, uint32_t vm)
{
	struct drm_i915_gem_context_param arg = {
		.param = I915_CONTEXT_PARAM_VM,
		.ctx_id = ctx,
		.value = vm,
	};

	gem_context_set_param(i915, &arg);
}

static uint32_t ctx_get_vm(int i915, uint32_t ctx)
{
        struct drm_i915_gem_context_param arg;

        memset(&arg, 0, sizeof(arg));
        arg.param = I915_CONTEXT_PARAM_VM;
        arg.ctx_id = ctx;
        gem_context_get_param(i915, &arg);
        igt_assert(arg.value);

        return arg.value;
}

static void virtual(int i915)
{
	const unsigned int wait_us = default_timeout_wait_s * USEC_PER_SEC;
	unsigned int num_engines = __engines__->num_engines, i, count;
	igt_spin_t *spin[num_engines];
	unsigned int expect = num_engines;
	uint32_t ctx[num_engines];
	uint32_t vm;

	igt_require(gem_has_execlists(i915));

	igt_debug("%u virtual engines\n", num_engines);
	igt_require(num_engines);

	i = 0;
	for (int class = 0; class < 32; class++) {
		struct i915_engine_class_instance *ci;

		ci = list_engines(class, &count);
		if (!ci)
			continue;

		for (int pass = 0; pass < count; pass++) {
			igt_assert(sizeof(*ci) == sizeof(int));
			igt_permute_array(ci, count, igt_exchange_int);

			igt_assert(i < num_engines);

			ctx[i] = gem_context_create(i915);

			if (!i)
				vm = ctx_get_vm(i915, ctx[i]);
			else
				ctx_set_vm(i915, ctx[i], vm);

			set_load_balancer(i915, ctx[i], ci, count, NULL);

			spin[i] = igt_spin_new(i915,
					       .ctx = ctx[i],
					       .flags = spin_flags());
			i++;
		}

		free(ci);
	}

	count = wait_timeout(i915, spin, num_engines, wait_us, expect);

	for (i = 0; i < num_engines && spin[i]; i++) {
		gem_context_destroy(i915, ctx[i]);
		igt_spin_free(i915, spin[i]);
	}

	igt_assert_eq(count, expect);
}

igt_main
{
	int i915 = -1;

	igt_fixture {
		struct drm_i915_query_item item;
		char *tmp;

		i915 = drm_open_driver_master(DRIVER_INTEL);
		gem_submission_print_method(i915);
		gem_scheduler_print_capability(i915);

		igt_require_gem(i915);

		tmp = __igt_params_get(i915, "request_timeout_ms");
		if (tmp) {
			const unsigned int timeout = 1;

			igt_params_save_and_set(i915, "request_timeout_ms",
						"%u", timeout * 1000);
			default_timeout_wait_s = timeout * 5;
			free(tmp);
		} else {
			default_timeout_wait_s = 12;
		}

		i915 = gem_reopen_driver(i915); /* Apply modparam. */

		__engines__ = malloc(4096);
		igt_assert(__engines__);
		memset(__engines__, 0, 4096);
		memset(&item, 0, sizeof(item));
		item.query_id = DRM_I915_QUERY_ENGINE_INFO;
		item.data_ptr = to_user_pointer(__engines__);
		item.length = 4096;
		i915_query_items(i915, &item, 1);
		igt_assert(item.length >= 0);
		igt_assert(item.length <= 4096);
		igt_assert(__engines__->num_engines > 0);
	}

	igt_subtest_group {
		igt_subtest("default-physical")
			physical(i915);

		igt_subtest("default-virtual")
			virtual(i915);
	}

	igt_fixture {
		close(i915);
	}
}
