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
#include "igt_collection.h"

#include "i915/gem_create.h"

IGT_TEST_DESCRIPTION("Basic sanity check of execbuf-ioctl rings.");

static uint32_t batch_create(int fd, uint32_t batch_size, uint32_t region)
{
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	uint32_t handle;

	handle = gem_create_in_memory_regions(fd, batch_size, region);
	gem_write(fd, handle, 0, &bbe, sizeof(bbe));

	return handle;
}

igt_main
{
	const struct intel_execution_engine2 *e;
	struct drm_i915_query_memory_regions *query_info;
	struct igt_collection *regions, *set;
	uint32_t batch_size;
	const intel_ctx_t *ctx;
	int fd = -1;

	igt_fixture {
		fd = drm_open_driver(DRIVER_INTEL);
		ctx = intel_ctx_create_all_physical(fd);

		/* igt_require_gem(fd); // test is mandatory */
		igt_fork_hang_detector(fd);

		query_info = gem_get_query_memory_regions(fd);
		igt_assert(query_info);

		set = get_memory_region_set(query_info,
					    I915_SYSTEM_MEMORY,
					    I915_DEVICE_MEMORY);
	}

	igt_subtest_with_dynamic("basic") {
		for_each_combination(regions, 1, set) {
			char *sub_name = memregion_dynamic_subtest_name(regions);
			struct drm_i915_gem_exec_object2 exec;
			uint32_t region = igt_collection_get_value(regions, 0);

			batch_size = gem_get_batch_size(fd, MEMORY_TYPE_FROM_REGION(region));
			memset(&exec, 0, sizeof(exec));
			exec.handle = batch_create(fd, batch_size, region);

			for_each_ctx_engine(fd, ctx, e) {
				igt_dynamic_f("%s-%s", e->name, sub_name) {
					struct drm_i915_gem_execbuffer2 execbuf = {
						.buffers_ptr = to_user_pointer(&exec),
						.buffer_count = 1,
						.flags = e->flags,
						.rsvd1 = ctx->id,
					};

					gem_execbuf(fd, &execbuf);
				}
			}
			gem_sync(fd, exec.handle); /* catch any GPU hang */
			gem_close(fd, exec.handle);
			free(sub_name);
		}
	}

	igt_fixture {
		free(query_info);
		igt_collection_destroy(set);
		igt_stop_hang_detector();
		intel_ctx_destroy(fd, ctx);
		close(fd);
	}
}
