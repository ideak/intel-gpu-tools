/*
 * Copyright © 2014 Intel Corporation
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

#include <stdbool.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <inttypes.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/time.h>

#include <drm.h>

#include "i915/gem.h"
#include "i915/gem_create.h"
#include "igt.h"
#include "igt_debugfs.h"
/**
 * TEST: gem ppgtt
 * Run type: FULL
 *
 * SUBTEST: blt-vs-render-ctx0
 * Feature: mapping
 *
 * SUBTEST: blt-vs-render-ctxN
 * Feature: mapping
 *
 * SUBTEST: flink-and-close-vma-leak
 * Category: Desktop client
 * Feature: mapping, xorg_dri2
 * Functionality: buffer management
 * Sub-category: i915
 * Test category: GEM_Legacy
 *
 * SUBTEST: shrink-vs-evict-any
 * Description: Regression test to verify GTT eviction can't randomly fail due to object lock contention
 * Feature: mapping
 * Test category: GEM_Legacy
 *
 * SUBTEST: shrink-vs-evict-pinned
 * Description: Regression test to verify GTT eviction can't randomly fail due to object lock contention
 * Feature: mapping
 * Test category: GEM_Legacy
 */

#define WIDTH 512
#define STRIDE (WIDTH*4)
#define HEIGHT 512
#define SIZE (HEIGHT*STRIDE)

static struct intel_buf *create_bo(struct buf_ops *bops, uint32_t pixel)
{
	uint64_t value = (uint64_t)pixel << 32 | pixel, *v;
	struct intel_buf *buf;

	buf = intel_buf_create(bops, WIDTH, HEIGHT, 32, 0, I915_TILING_NONE, 0);
	v = intel_buf_device_map(buf, true);

	for (int i = 0; i < SIZE / sizeof(value); i += 8) {
		v[i + 0] = value; v[i + 1] = value;
		v[i + 2] = value; v[i + 3] = value;
		v[i + 4] = value; v[i + 5] = value;
		v[i + 6] = value; v[i + 7] = value;
	}
	intel_buf_unmap(buf);

	return buf;
}

static void cleanup_bufs(struct intel_buf **buf, int count)
{
	for (int child = 0; child < count; child++) {
		struct buf_ops *bops = buf[child]->bops;
		int fd = buf_ops_get_fd(buf[child]->bops);

		intel_buf_destroy(buf[child]);
		buf_ops_destroy(bops);
		close(fd);
	}
}

static void fork_rcs_copy(int timeout, uint32_t final,
			  struct intel_buf **dst, int count,
			  unsigned flags)
#define CREATE_CONTEXT 0x1
{
	igt_render_copyfunc_t render_copy;
	int devid;

	for (int child = 0; child < count; child++) {
		int fd = drm_open_driver(DRIVER_INTEL);
		struct buf_ops *bops;

		devid = intel_get_drm_devid(fd);

		bops = buf_ops_create(fd);

		dst[child] = create_bo(bops, ~0);

		render_copy = igt_get_render_copyfunc(devid);
		igt_require_f(render_copy,
			      "no render-copy function\n");
	}

	igt_fork(child, count) {
		struct intel_bb *ibb;
		uint32_t ctx = 0;
		struct intel_buf *src;
		unsigned long i;

		/* Standalone allocator */
		intel_allocator_init();

		if (flags & CREATE_CONTEXT)
			ctx = gem_context_create(buf_ops_get_fd(dst[child]->bops));

		ibb = intel_bb_create_with_context(buf_ops_get_fd(dst[child]->bops),
						   ctx, 0, NULL, 4096);
		i = 0;
		igt_until_timeout(timeout) {
			src = create_bo(dst[child]->bops,
					i++ | child << 16);
			render_copy(ibb,
				    src, 0, 0,
				    WIDTH, HEIGHT,
				    dst[child], 0, 0);

			intel_buf_destroy(src);
		}

		src = create_bo(dst[child]->bops,
				final | child << 16);
		render_copy(ibb,
			    src, 0, 0,
			    WIDTH, HEIGHT,
			    dst[child], 0, 0);

		intel_buf_destroy(src);

		intel_bb_destroy(ibb);
	}
}

static void fork_bcs_copy(int timeout, uint32_t final,
			  struct intel_buf **dst, int count)
{
	for (int child = 0; child < count; child++) {
		struct buf_ops *bops;
		int fd = drm_open_driver(DRIVER_INTEL);

		bops = buf_ops_create(fd);
		dst[child] = create_bo(bops, ~0);
	}

	igt_fork(child, count) {
		struct intel_buf *src[2];
		struct intel_bb *ibb;
		unsigned long i;

		/* Standalone allocator */
		intel_allocator_init();

		ibb = intel_bb_create(buf_ops_get_fd(dst[child]->bops), 4096);

		i = 0;
		igt_until_timeout(timeout) {
			src[0] = create_bo(dst[child]->bops,
					   ~0);
			src[1] = create_bo(dst[child]->bops,
					   i++ | child << 16);

			intel_bb_copy_intel_buf(ibb, src[1], src[0], SIZE);
			intel_bb_copy_intel_buf(ibb, src[0], dst[child], SIZE);

			intel_buf_destroy(src[1]);
			intel_buf_destroy(src[0]);
		}

		src[0] = create_bo(dst[child]->bops, ~0);
		src[1] = create_bo(dst[child]->bops,
				   final | child << 16);

		intel_bb_copy_intel_buf(ibb, src[1], src[0], SIZE);
		intel_bb_copy_intel_buf(ibb, src[0], dst[child], SIZE);

		intel_buf_destroy(src[1]);
		intel_buf_destroy(src[0]);

		intel_bb_destroy(ibb);
	}
}

static void surfaces_check(struct intel_buf **buf, int count, uint32_t expected)
{
	for (int child = 0; child < count; child++) {
		uint32_t *ptr;

		ptr = intel_buf_cpu_map(buf[child], 0);
		for (int j = 0; j < SIZE/4; j++)
			igt_assert_eq(ptr[j], expected | child << 16);
		intel_buf_unmap(buf[child]);
	}
}

static uint64_t exec_and_get_offset(int fd, uint32_t batch)
{
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 exec[1];
	uint32_t batch_data[2] = { MI_BATCH_BUFFER_END };

	gem_write(fd, batch, 0, batch_data, sizeof(batch_data));

	memset(exec, 0, sizeof(exec));
	exec[0].handle = batch;

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(exec);
	execbuf.buffer_count = 1;

	gem_execbuf(fd, &execbuf);
	igt_assert_neq(exec[0].offset, -1);

	return exec[0].offset;
}

static void flink_and_close(void)
{
	uint32_t fd, fd2;
	uint32_t bo, flinked_bo, new_bo, name;
	uint64_t offset, offset_new;

	fd = drm_open_driver(DRIVER_INTEL);
	igt_require(gem_uses_full_ppgtt(fd));

	bo = gem_create(fd, 4096);
	name = gem_flink(fd, bo);

	fd2 = drm_open_driver(DRIVER_INTEL);

	flinked_bo = gem_open(fd2, name);
	offset = exec_and_get_offset(fd2, flinked_bo);
	gem_sync(fd2, flinked_bo);
	gem_close(fd2, flinked_bo);

	igt_drop_caches_set(fd, DROP_RETIRE | DROP_IDLE);

	/* the flinked bo VMA should have been cleared now, so a new bo of the
	 * same size should get the same offset
	 */
	new_bo = gem_create(fd2, 4096);
	offset_new = exec_and_get_offset(fd2, new_bo);
	gem_close(fd2, new_bo);

	igt_assert_eq(offset, offset_new);

	gem_close(fd, bo);
	close(fd);
	close(fd2);
}

#define PAGE_SIZE 4096

static uint32_t batch_create(int fd)
{
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	uint32_t handle;

	handle = gem_create(fd, sizeof(bbe));
	gem_write(fd, handle, 0, &bbe, sizeof(bbe));

	return handle;
}

#define IGT_USE_ANY	0x1
#define IGT_USE_PINNED	0x2
static void upload(int fd, uint32_t handle, uint32_t in_fence, uint32_t ctx_id,
		   unsigned int flags)
{
	struct drm_i915_gem_exec_object2 exec[2] = {};
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(&exec),
		.buffer_count = 1,
		.rsvd1 = ctx_id,
	};

	if (in_fence) {
		execbuf.rsvd2 = in_fence;
		execbuf.flags = I915_EXEC_FENCE_IN;
	}

	exec[0].handle = handle;
	exec[0].flags = EXEC_OBJECT_SUPPORTS_48B_ADDRESS;

	if (flags & IGT_USE_PINNED)
		exec[0].flags |= EXEC_OBJECT_PINNED; /* offset = 0 */

	if (flags & IGT_USE_ANY) {
		exec[0].flags |= EXEC_OBJECT_PAD_TO_SIZE;
		exec[0].pad_to_size = gem_aperture_size(fd);
	}

	gem_execbuf(fd, &execbuf);
}

static void shrink_vs_evict(unsigned int flags)
{
	const unsigned int nproc = sysconf(_SC_NPROCESSORS_ONLN) + 1;
	const uint64_t timeout_5s = 5LL * NSEC_PER_SEC;
	int fd = drm_open_driver(DRIVER_INTEL);
	uint64_t ahnd = get_reloc_ahnd(fd, 0);
	const intel_ctx_t *ctx_arr[nproc];
	igt_spin_t *spinner;
	uint32_t shared;

	/*
	 * Try to simulate some nasty object lock contention during GTT
	 * eviction. Create a BO and bind across several different VMs.  Invoke
	 * the shrinker on that shared BO, followed by triggering GTT eviction
	 * across all VMs.  Both require the object lock to make forward
	 * progress when trying to unbind the BO, but the shrinker will be
	 * blocked by the spinner (until killed).  Once the spinner is killed
	 * the shrinker should be able to unbind the object and drop the object
	 * lock, and GTT eviction should eventually succeed. At no point should
	 * we see -ENOSPC from the execbuf, even if we can't currently grab the
	 * object lock.
	 */

	igt_require(gem_uses_full_ppgtt(fd));

	igt_drop_caches_set(fd, DROP_ALL);

	shared = batch_create(fd);

	spinner = igt_spin_new(fd,
			       .ahnd = ahnd,
			       .flags = IGT_SPIN_FENCE_OUT);
	igt_spin_set_timeout(spinner, timeout_5s);

	/*
	 * Create several VMs to ensure we don't block on the same vm lock. The
	 * goal of the test is to ensure that object lock contention doesn't
	 * somehow result in -ENOSPC from execbuf, if we need to trigger GTT
	 * eviction.
	 */
	for (int i = 0; i < nproc; i++) {
		ctx_arr[i] = intel_ctx_create(fd, NULL);

		upload(fd, shared, spinner->execbuf.rsvd2 >> 32,
		       ctx_arr[i]->id, flags);
	}

	igt_fork(child, 1)
		igt_drop_caches_set(fd, DROP_ALL);

	sleep(2); /* Give the shrinker time to find shared */

	igt_fork(child, nproc) {
		uint32_t isolated;

		/*
		 * One of these forks will be stuck on the vm mutex, since the
		 * shrinker is holding it (along with the object lock) while
		 * trying to unbind the chosen vma, but is blocked by the
		 * spinner. The rest should only block waiting to grab the
		 * object lock for shared, before then trying to GTT evict it
		 * from their respective vm. In either case the contention of
		 * the vm->mutex or object lock should never result in -ENOSPC
		 * or some other error.
		 */
		isolated = batch_create(fd);
		upload(fd, isolated, 0, ctx_arr[child]->id, flags);
		gem_close(fd, isolated);
	}

	igt_waitchildren();
	igt_spin_free(fd, spinner);

	for (int i = 0; i < nproc; i++)
		intel_ctx_destroy(fd, ctx_arr[i]);

	gem_close(fd, shared);
}

static bool has_contexts(void)
{
	bool result;
	int fd;

	fd = drm_open_driver(DRIVER_INTEL);
	result = gem_has_contexts(fd);
	close(fd);

	return result;
}

igt_main
{
	const int ncpus = sysconf(_SC_NPROCESSORS_ONLN);

	igt_fixture {
		int fd = drm_open_driver(DRIVER_INTEL);
		igt_require_gem(fd);
		gem_require_blitter(fd);
		close(fd);
	}

	igt_subtest("blt-vs-render-ctx0") {
		struct intel_buf *bcs[1], **rcs;
		int nchild = ncpus + 1;
		uint64_t mem_per_test;

		mem_per_test = SIZE;
		igt_require_memory(nchild + 1, mem_per_test, CHECK_RAM);

		rcs = calloc(sizeof(*rcs), nchild);
		igt_assert(rcs);

		fork_bcs_copy(30, 0x4000, bcs, 1);
		fork_rcs_copy(30, 0x8000 / nchild, rcs, nchild, 0);

		igt_waitchildren();

		surfaces_check(bcs, 1, 0x4000);
		surfaces_check(rcs, nchild, 0x8000 / nchild);

		cleanup_bufs(bcs, 1);
		cleanup_bufs(rcs, nchild);
		free(rcs);
	}

	igt_subtest("blt-vs-render-ctxN") {
		struct intel_buf *bcs[1], **rcs;
		uint64_t mem_per_ctx = 2 * 128 * 1024; /* rough context sizes */
		uint64_t mem_per_test;
		int nchild = ncpus + 1;

		igt_require(has_contexts());

		mem_per_test = SIZE + mem_per_ctx;
		igt_require_memory(1 + nchild, mem_per_test, CHECK_RAM);

		rcs = calloc(sizeof(*rcs), nchild);
		igt_assert(rcs);

		fork_rcs_copy(30, 0x8000 / nchild, rcs, nchild, CREATE_CONTEXT);
		fork_bcs_copy(30, 0x4000, bcs, 1);

		igt_waitchildren();

		surfaces_check(bcs, 1, 0x4000);
		surfaces_check(rcs, nchild, 0x8000 / nchild);

		cleanup_bufs(bcs, 1);
		cleanup_bufs(rcs, nchild);
		free(rcs);
	}

	igt_subtest("flink-and-close-vma-leak")
		flink_and_close();

	igt_describe("Regression test to verify GTT eviction can't randomly fail due to object lock contention");
	igt_subtest_group {
		igt_subtest("shrink-vs-evict-any")
			shrink_vs_evict(IGT_USE_ANY);
		igt_subtest("shrink-vs-evict-pinned")
			shrink_vs_evict(IGT_USE_PINNED);
	}
}
