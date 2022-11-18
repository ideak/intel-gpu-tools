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

#include <string.h>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <sys/signal.h>
#include "i915/gem.h"
#include "i915/gem_create.h"
#include "igt.h"
#include "igt_dummyload.h"
#include "igt_store.h"
#include "igt_syncobj.h"
#include "igt_sysfs.h"
#include "igt_vgem.h"
#include "intel_ctx.h"
#include "sw_sync.h"

IGT_TEST_DESCRIPTION("Check that execbuf waits for explicit fences");

#ifndef SYNC_IOC_MERGE
struct sync_merge_data {
	char     name[32];
	int32_t  fd2;
	int32_t  fence;
	uint32_t flags;
	uint32_t pad;
};
#define SYNC_IOC_MAGIC '>'
#define SYNC_IOC_MERGE _IOWR(SYNC_IOC_MAGIC, 3, struct sync_merge_data)
#endif

#define MI_SEMAPHORE_WAIT		(0x1c << 23)
#define   MI_SEMAPHORE_POLL             (1 << 15)
#define   MI_SEMAPHORE_SAD_GT_SDD       (0 << 12)
#define   MI_SEMAPHORE_SAD_GTE_SDD      (1 << 12)
#define   MI_SEMAPHORE_SAD_LT_SDD       (2 << 12)
#define   MI_SEMAPHORE_SAD_LTE_SDD      (3 << 12)
#define   MI_SEMAPHORE_SAD_EQ_SDD       (4 << 12)
#define   MI_SEMAPHORE_SAD_NEQ_SDD      (5 << 12)

static bool fence_busy(int fence)
{
	return poll(&(struct pollfd){fence, POLLIN}, 1, 0) == 0;
}

#define HANG 0x1
#define NONBLOCK 0x2
#define WAIT 0x4

static void test_fence_busy(int fd, const intel_ctx_t *ctx,
			    const struct intel_execution_engine2 *e,
			    unsigned flags)
{
	uint64_t ahnd = get_reloc_ahnd(fd, ctx->id);
	struct timespec tv;
	int fence, timeout;
	igt_spin_t *spin;

	gem_quiescent_gpu(fd);

	spin = igt_spin_new(fd, .ahnd = ahnd, .ctx = ctx, .engine = e->flags,
			    .flags = IGT_SPIN_FENCE_OUT |
			    ((flags & HANG) ? IGT_SPIN_NO_PREEMPTION : 0));

	fence = spin->out_fence;
	igt_assert(gem_bo_busy(fd, spin->handle));
	igt_assert(fence_busy(fence));

	timeout = 120;
	if ((flags & HANG) == 0) {
		igt_spin_end(spin);
		timeout = SLOW_QUICK(120, 1);
	}

	if (flags & WAIT) {
		struct pollfd pfd = { .fd = fence, .events = POLLIN };
		igt_assert(poll(&pfd, 1, timeout*1000) == 1);
	} else {
		int elapsed_time;
		memset(&tv, 0, sizeof(tv));
		while (fence_busy(fence)) {
			elapsed_time = igt_seconds_elapsed(&tv);
			if (elapsed_time >= timeout)
				igt_info("Elapsed time (%ds)\n", elapsed_time);
			igt_assert(igt_seconds_elapsed(&tv) < timeout);
		}
		igt_info("Elapsed time (%ds)\n", igt_seconds_elapsed(&tv));
	}
	igt_assert(!gem_bo_busy(fd, spin->handle));
	igt_assert_eq(sync_fence_status(fence),
		      flags & HANG ? -EIO : SYNC_FENCE_OK);

	igt_spin_free(fd, spin);
	put_ahnd(ahnd);
	gem_quiescent_gpu(fd);
}

static void test_fence_busy_all(int fd, const intel_ctx_t *ctx, unsigned flags)
{
	const struct intel_execution_engine2 *e;
	const unsigned int gen = intel_gen(intel_get_drm_devid(fd));
	struct drm_i915_gem_exec_object2 obj;
	struct drm_i915_gem_relocation_entry reloc;
	struct drm_i915_gem_execbuffer2 execbuf;
	struct timespec tv;
	uint32_t *batch;
	uint64_t ahnd = get_reloc_ahnd(fd, ctx->id);
	int all, i, timeout;

	gem_quiescent_gpu(fd);

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(&obj);
	execbuf.buffer_count = 1;

	memset(&obj, 0, sizeof(obj));
	obj.handle = gem_create(fd, 4096);
	obj.offset = get_offset(ahnd, obj.handle, 4096, 0);
	igt_assert(obj.offset != -1);

	batch = gem_mmap__device_coherent(fd, obj.handle, 0, 4096, PROT_WRITE);
	gem_set_domain(fd, obj.handle,
		       I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);

	i = 0;
	if ((flags & HANG) == 0)
		batch[i++] = 0x5 << 23;

	if (!ahnd) {
		obj.relocs_ptr = to_user_pointer(&reloc);
		obj.relocation_count = 1;
		memset(&reloc, 0, sizeof(reloc));
		reloc.target_handle = obj.handle; /* recurse */
		reloc.presumed_offset = obj.offset;
		reloc.offset = (i + 1) * sizeof(uint32_t);
		reloc.delta = 0;
		reloc.read_domains = I915_GEM_DOMAIN_COMMAND;
		reloc.write_domain = 0;
	} else {
		obj.flags |= EXEC_OBJECT_PINNED;
	}

	batch[i] = MI_BATCH_BUFFER_START;
	if (gen >= 8) {
		batch[i] |= 1 << 8 | 1;
		batch[++i] = obj.offset;
		batch[++i] = obj.offset >> 32;
	} else if (gen >= 6) {
		batch[i] |= 1 << 8;
		batch[++i] = obj.offset;
	} else {
		batch[i] |= 2 << 6;
		batch[++i] = obj.offset;
		if (gen < 4) {
			batch[i] |= 1;
			reloc.delta = 1;
		}
	}
	i++;

	all = -1;
	for_each_ctx_engine(fd, ctx, e) {
		int fence, new;

		if ((flags & HANG) == 0 &&
		    !gem_class_has_mutable_submission(fd, e->class))
			continue;

		execbuf.flags = e->flags | I915_EXEC_FENCE_OUT;
		execbuf.rsvd1 = ctx->id;
		execbuf.rsvd2 = -1;
		gem_execbuf_wr(fd, &execbuf);
		fence = execbuf.rsvd2 >> 32;
		igt_assert(fence != -1);

		if (all < 0) {
			all = fence;
			continue;
		}

		new = sync_fence_merge(all, fence);
		igt_assert_lte(0, new);
		close(all);
		close(fence);

		all = new;
	}

	igt_assert(gem_bo_busy(fd, obj.handle));
	igt_assert(fence_busy(all));

	timeout = 120;
	if ((flags & HANG) == 0) {
		*batch = MI_BATCH_BUFFER_END;
		__sync_synchronize();
		timeout = 1;
	}
	munmap(batch, 4096);

	if (flags & WAIT) {
		struct pollfd pfd = { .fd = all, .events = POLLIN };
		igt_assert(poll(&pfd, 1, timeout*1000) == 1);
	} else {
		memset(&tv, 0, sizeof(tv));
		while (fence_busy(all))
			igt_assert(igt_seconds_elapsed(&tv) < timeout);
	}

	igt_assert(!gem_bo_busy(fd, obj.handle));
	igt_assert_eq(sync_fence_status(all),
		      flags & HANG ? -EIO : SYNC_FENCE_OK);

	close(all);
	gem_close(fd, obj.handle);
	put_offset(ahnd, obj.handle);
	put_ahnd(ahnd);

	gem_quiescent_gpu(fd);
}

static unsigned int spin_hang(unsigned int flags)
{
	if (!(flags & HANG))
		return 0;

	return IGT_SPIN_NO_PREEMPTION | IGT_SPIN_INVALID_CS;
}

static void test_fence_await(int fd, const intel_ctx_t *ctx,
			     const struct intel_execution_engine2 *e,
			     unsigned flags)
{
	uint64_t scratch_offset, ahnd = get_reloc_ahnd(fd, ctx->id);
	const struct intel_execution_engine2 *e2;
	uint32_t scratch = gem_create(fd, 4096);
	uint32_t *out, tmp[4096 / sizeof(*out)];
	igt_spin_t *spin;
	int i, n;

	scratch_offset = get_offset(ahnd, scratch, 4096, 0);

	out = gem_mmap__device_coherent(fd, scratch, 0, 4096, PROT_WRITE);
	gem_set_domain(fd, scratch,
			I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);

	spin = igt_spin_new(fd,
			    .ahnd = ahnd,
			    .ctx = ctx,
			    .engine = e->flags,
			    .flags = IGT_SPIN_FENCE_OUT |
				     IGT_SPIN_POLL_RUN |
				     spin_hang(flags));
	igt_assert(spin->out_fence != -1);

	i = 0;
	for_each_ctx_engine(fd, ctx, e2) {
		if (!gem_class_can_store_dword(fd, e2->class))
			continue;

		i++;

		if (flags & NONBLOCK) {
			igt_store_word(fd, ahnd, ctx, e2, spin->out_fence,
				       scratch, scratch_offset, i, i);
		} else {
			igt_fork(child, 1) {
				ahnd = get_reloc_ahnd(fd, ctx->id);
				igt_store_word(fd, ahnd, ctx, e2, spin->out_fence,
					       scratch, scratch_offset, i, i);
				put_ahnd(ahnd);
			}
		}
	}

	igt_spin_busywait_until_started(spin);
	/* Long, but not too long to anger preemption disable checks */
	usleep(50 * 1000); /* 50 ms, typical preempt reset is 150+ms */

	/*
	 * Check for invalidly completing the task early.
	 * In -hang variants, invalid spin batch may trigger an immediate reset,
	 * then we are able to verify if store batches haven't been started yet
	 * only if the fence of the spin batch is still busy.
	 * Just run *await-hang subtest multiple times to build confidence.
	 */
	memcpy(tmp, out, (i + 1) * sizeof(*out));
	if (fence_busy(spin->out_fence)) {
		for (n = 0; n <= i; n++)
			igt_assert_eq_u32(tmp[n], 0);
	}
	if ((flags & HANG) == 0) {
		igt_assert(fence_busy(spin->out_fence));

		igt_spin_end(spin);
	}

	igt_waitchildren();

	gem_set_domain(fd, scratch, I915_GEM_DOMAIN_GTT, 0);
	igt_assert(!fence_busy(spin->out_fence));
	if ((flags & HANG) == 0) {
		do
			igt_assert_eq_u32(out[i], i);
		while (i--);
	}
	munmap(out, 4096);

	igt_spin_free(fd, spin);
	gem_close(fd, scratch);
	put_offset(ahnd, scratch);
	put_ahnd(ahnd);
}

static uint32_t timeslicing_batches(int i915, uint32_t *offset)
{
        uint32_t handle = gem_create(i915, 4096);
        uint32_t cs[256];

	*offset += 4000;
	for (int pair = 0; pair <= 1; pair++) {
		int x = 1;
		int i = 0;

		for (int step = 0; step < 8; step++) {
			if (pair) {
				cs[i++] =
					MI_SEMAPHORE_WAIT |
					MI_SEMAPHORE_POLL |
					MI_SEMAPHORE_SAD_EQ_SDD |
					(4 - 2);
				cs[i++] = x++;
				cs[i++] = *offset;
				cs[i++] = 0;
			}

			cs[i++] = MI_STORE_DWORD_IMM;
			cs[i++] = *offset;
			cs[i++] = 0;
			cs[i++] = x++;

			if (!pair) {
				cs[i++] =
					MI_SEMAPHORE_WAIT |
					MI_SEMAPHORE_POLL |
					MI_SEMAPHORE_SAD_EQ_SDD |
					(4 - 2);
				cs[i++] = x++;
				cs[i++] = *offset;
				cs[i++] = 0;
			}
		}

		cs[i++] = MI_BATCH_BUFFER_END;
		igt_assert(i < ARRAY_SIZE(cs));
		gem_write(i915, handle, pair * sizeof(cs), cs, sizeof(cs));
	}

	*offset = sizeof(cs);
        return handle;
}

static void test_submit_fence(int i915, const intel_ctx_t *ctx,
			      const struct intel_execution_engine2 *e)
{
	const struct intel_execution_engine2 *e2;

	/*
	 * Create a pair of interlocking batches, that ping pong
	 * between each other, and only advance one step at a time.
	 * We require the kernel to preempt at each semaphore and
	 * switch to the other batch in order to advance.
	 */

	for_each_ctx_engine(i915, ctx, e2) {
		unsigned int offset = 24 << 20;
		const intel_ctx_t *tmp_ctx;
		struct drm_i915_gem_exec_object2 obj = {
			.offset = offset,
			.flags = EXEC_OBJECT_PINNED,
		};
		struct drm_i915_gem_execbuffer2 execbuf  = {
			.buffers_ptr = to_user_pointer(&obj),
			.buffer_count = 1,
		};
		uint32_t *result;
		int out;

		obj.handle = timeslicing_batches(i915, &offset);
		result = gem_mmap__device_coherent(i915, obj.handle,
						   0, 4096, PROT_READ);

		execbuf.flags = e->flags | I915_EXEC_FENCE_OUT;
		execbuf.batch_start_offset = 0;
		execbuf.rsvd1 = ctx->id;
		gem_execbuf_wr(i915, &execbuf);

		tmp_ctx = intel_ctx_create(i915, &ctx->cfg);
		execbuf.rsvd1 = tmp_ctx->id;
		execbuf.rsvd2 >>= 32;
		execbuf.flags = e2->flags;
		execbuf.flags |= I915_EXEC_FENCE_SUBMIT | I915_EXEC_FENCE_OUT;
		execbuf.batch_start_offset = offset;
		gem_execbuf_wr(i915, &execbuf);
		intel_ctx_destroy(i915, tmp_ctx);

		gem_sync(i915, obj.handle);
		gem_close(i915, obj.handle);

		/* no hangs! */
		out = execbuf.rsvd2;
		igt_assert_eq(sync_fence_status(out), 1);
		close(out);

		out = execbuf.rsvd2 >> 32;
		igt_assert_eq(sync_fence_status(out), 1);
		close(out);

		igt_assert_eq(result[1000], 16);
		munmap(result, 4096);
	}
}

static uint32_t submitN_batches(int i915, uint32_t offset, int count)
{
        uint32_t handle = gem_create(i915, (count + 1) * 1024);
        uint32_t cs[256];

	for (int pair = 0; pair < count; pair++) {
		int x = pair;
		int i = 0;

		for (int step = 0; step < 8; step++) {
			cs[i++] =
				MI_SEMAPHORE_WAIT |
				MI_SEMAPHORE_POLL |
				MI_SEMAPHORE_SAD_EQ_SDD |
				(4 - 2);
			cs[i++] = x;
			cs[i++] = offset;
			cs[i++] = 0;

			cs[i++] = MI_STORE_DWORD_IMM;
			cs[i++] = offset;
			cs[i++] = 0;
			cs[i++] = x + 1;

			x += count;
		}

		cs[i++] = MI_BATCH_BUFFER_END;
		igt_assert(i < ARRAY_SIZE(cs));
		gem_write(i915, handle, (pair + 1) * sizeof(cs),
			  cs, sizeof(cs));
	}

        return handle;
}

static void test_submitN(int i915, const intel_ctx_t *ctx,
			 const struct intel_execution_engine2 *e,
			 int count)
{
	unsigned int offset = 24 << 20;
	unsigned int sz = ALIGN((count + 1) * 1024, 4096);
	struct drm_i915_gem_exec_object2 obj = {
		.handle = submitN_batches(i915, offset, count),
		.offset = offset,
		.flags = EXEC_OBJECT_PINNED,
	};
	struct drm_i915_gem_execbuffer2 execbuf  = {
		.buffers_ptr = to_user_pointer(&obj),
		.buffer_count = 1,
		.flags = e->flags | I915_EXEC_FENCE_OUT,
		.rsvd1 = ctx->id,
	};
	uint32_t *result =
		gem_mmap__device_coherent(i915, obj.handle, 0, sz, PROT_READ);
	int fence[count];

	igt_require(gem_scheduler_has_semaphores(i915));
	igt_require(gem_scheduler_has_preemption(i915));
	igt_require(intel_gen(intel_get_drm_devid(i915)) >= 8);

	for (int i = 0; i < count; i++) {
		const intel_ctx_t *tmp_ctx = intel_ctx_create(i915, &ctx->cfg);
		execbuf.rsvd1 = tmp_ctx->id;
		execbuf.batch_start_offset = (i + 1) * 1024;
		gem_execbuf_wr(i915, &execbuf);
		intel_ctx_destroy(i915, tmp_ctx);

		execbuf.flags |= I915_EXEC_FENCE_SUBMIT;
		execbuf.rsvd2 >>= 32;
		fence[i] = execbuf.rsvd2;
	}

	gem_sync(i915, obj.handle);
	gem_close(i915, obj.handle);

	/* no hangs! */
	for (int i = 0; i < count; i++) {
		igt_assert_eq(sync_fence_status(fence[i]), 1);
		close(fence[i]);
	}

	igt_assert_eq(*result, 8 * count);
	munmap(result, sz);
}

static void alarm_handler(int sig)
{
}

static int __execbuf(int fd, struct drm_i915_gem_execbuffer2 *execbuf)
{
	int err;

	err = 0;
	if (ioctl(fd, DRM_IOCTL_I915_GEM_EXECBUFFER2_WR, execbuf))
		err = -errno;

	errno = 0;
	return err;
}

static void test_parallel(int i915, const intel_ctx_t *ctx,
			  const struct intel_execution_engine2 *e)
{
	const struct intel_execution_engine2 *e2;
	const unsigned int gen = intel_gen(intel_get_drm_devid(i915));
	uint32_t scratch = gem_create(i915, 4096);
	uint32_t *out = gem_mmap__device_coherent(i915, scratch, 0, 4096, PROT_READ);
	uint32_t handle[I915_EXEC_RING_MASK];
	IGT_CORK_FENCE(cork);
	igt_spin_t *spin;
	int fence;
	int x = 0;
	uint64_t ahnd = get_reloc_ahnd(i915, ctx->id), bb_offset;
	uint64_t scratch_offset = get_offset(ahnd, scratch, 4096, 0);

	fence = igt_cork_plug(&cork, i915),
	spin = igt_spin_new(i915,
			    .ahnd = ahnd,
			    .ctx = ctx,
			    .engine = e->flags,
			    .fence = fence,
			    .flags = (IGT_SPIN_FENCE_OUT |
				      IGT_SPIN_FENCE_IN));
	close(fence);

	/* Queue all secondaries */
	for_each_ctx_engine(i915, ctx, e2) {
		struct drm_i915_gem_relocation_entry reloc = {
			.target_handle = scratch,
			.offset = sizeof(uint32_t),
			.delta = sizeof(uint32_t) * x
		};
		struct drm_i915_gem_exec_object2 obj[] = {
			{ .handle = scratch, },
			{
				.relocs_ptr = to_user_pointer(&reloc),
				.relocation_count = !ahnd ? 1 : 0,
			}
		};
		struct drm_i915_gem_execbuffer2 execbuf = {
			.buffers_ptr = to_user_pointer(obj),
			.buffer_count = ARRAY_SIZE(obj),
			.flags = e2->flags | I915_EXEC_FENCE_SUBMIT,
			.rsvd1 = ctx->id,
			.rsvd2 = spin->out_fence,
		};
		uint32_t batch[16];
		int i;

		if (e2->flags == e->flags)
			continue;

		obj[1].handle = gem_create(i915, 4096);

		if (ahnd) {
			bb_offset = get_offset(ahnd, obj[1].handle, 4096, 0);
			obj[1].offset = bb_offset;
			obj[1].flags = EXEC_OBJECT_PINNED;
			obj[0].offset = scratch_offset;
			obj[0].flags = EXEC_OBJECT_PINNED | EXEC_OBJECT_WRITE;
		}

		i = 0;
		batch[i] = MI_STORE_DWORD_IMM | (gen < 6 ? 1 << 22 : 0);
		if (gen >= 8) {
			batch[++i] = scratch_offset + reloc.delta;
			batch[++i] = scratch_offset >> 32;
		} else if (gen >= 4) {
			batch[++i] = 0;
			batch[++i] = reloc.delta;
			reloc.offset += sizeof(uint32_t);
		} else {
			batch[i]--;
			batch[++i] = reloc.delta;
		}
		batch[++i] = ~x;
		batch[++i] = MI_BATCH_BUFFER_END;
		gem_write(i915, obj[1].handle, 0, batch, sizeof(batch));

		if (gen < 6)
			execbuf.flags |= I915_EXEC_SECURE;

		gem_execbuf(i915, &execbuf);
		handle[x++] = obj[1].handle;
	}
	igt_assert(gem_bo_busy(i915, spin->handle));
	gem_close(i915, scratch);
	put_offset(ahnd, scratch);
	igt_require(x);

	/*
	 * No secondary should be executed since master is stalled. If there
	 * was no dependency chain at all, the secondaries would start
	 * immediately.
	 */
	for (int i = 0; i < x; i++) {
		igt_assert_eq_u32(out[i], 0);
		igt_assert(gem_bo_busy(i915, handle[i]));
	}
	igt_cork_unplug(&cork);

	/*
	 * Wait for all secondaries to complete. If we used a regular fence
	 * then the secondaries would not start until the master was complete.
	 * In this case that can only happen with a GPU reset, and so we run
	 * under the hang detector and double check that the master is still
	 * running afterwards.
	 */
	for (int i = 0; i < x; i++) {
		while (gem_bo_busy(i915, handle[i]))
			sleep(0);

		igt_assert_eq_u32(out[i], ~i);
		gem_close(i915, handle[i]);
		put_offset(ahnd, handle[i]);
	}
	munmap(out, 4096);

	/* Master should still be spinning, but all output should be written */
	igt_assert(gem_bo_busy(i915, spin->handle));
	igt_spin_free(i915, spin);
	put_ahnd(ahnd);
}

static void test_concurrent(int i915, const intel_ctx_t *ctx,
			    const struct intel_execution_engine2 *e)
{
	const unsigned int gen = intel_gen(intel_get_drm_devid(i915));
	uint64_t ahnd = get_reloc_ahnd(i915, ctx->id);
	struct drm_i915_gem_relocation_entry reloc = {
		.target_handle =  gem_create(i915, 4096),
		.write_domain = I915_GEM_DOMAIN_RENDER,
		.offset = sizeof(uint32_t),
	};
	struct drm_i915_gem_exec_object2 obj[] = {
		{ .handle = reloc.target_handle, },
		{
			.handle = gem_create(i915, 4096),
			.relocs_ptr = to_user_pointer(&reloc),
			.relocation_count = !ahnd ? 1 : 0,
		}
	};
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(obj),
		.buffer_count = ARRAY_SIZE(obj),
		.flags = e->flags | I915_EXEC_FENCE_SUBMIT,
		.rsvd1 = ctx->id,
	};
	IGT_CORK_FENCE(cork);
	uint32_t batch[16];
	igt_spin_t *spin;
	const intel_ctx_t *tmp_ctx;
	uint32_t result;
	uint64_t bb_offset, target_offset;
	int fence;
	int i;

	bb_offset = get_offset(ahnd, obj[1].handle, 4096, 0);
	target_offset = get_offset(ahnd, obj[0].handle, 4096, 0);
	if (ahnd) {
		obj[1].offset = bb_offset;
		obj[1].flags = EXEC_OBJECT_PINNED;
		obj[0].offset = target_offset;
		obj[0].flags = EXEC_OBJECT_PINNED | EXEC_OBJECT_WRITE;
	}

	/*
	 * A variant of test_parallel() that runs a bonded pair on a single
	 * engine and ensures that the secondary batch cannot start before
	 * the master is ready.
	 */

	fence = igt_cork_plug(&cork, i915),
	      spin = igt_spin_new(i915,
				  .ahnd = ahnd,
				  .ctx = ctx,
				  .engine = e->flags,
				  .fence = fence,
				  .flags = (IGT_SPIN_FENCE_OUT |
					    IGT_SPIN_FENCE_IN));
	close(fence);

	i = 0;
	batch[i] = MI_STORE_DWORD_IMM | (gen < 6 ? 1 << 22 : 0);
	if (gen >= 8) {
		batch[++i] = target_offset + reloc.delta;
		batch[++i] = target_offset >> 32;
	} else if (gen >= 4) {
		batch[++i] = 0;
		batch[++i] = reloc.delta;
		reloc.offset += sizeof(uint32_t);
	} else {
		batch[i]--;
		batch[++i] = reloc.delta;
	}
	batch[++i] = 0xd0df0d;
	batch[++i] = MI_BATCH_BUFFER_END;
	gem_write(i915, obj[1].handle, 0, batch, sizeof(batch));

	tmp_ctx = intel_ctx_create(i915, &ctx->cfg);
	execbuf.rsvd1 = tmp_ctx->id;
	execbuf.rsvd2 = spin->out_fence;
	if (gen < 6)
		execbuf.flags |= I915_EXEC_SECURE;

	gem_execbuf(i915, &execbuf);
	intel_ctx_destroy(i915, tmp_ctx);
	gem_close(i915, obj[1].handle);
	put_offset(ahnd, obj[1].handle);

	/*
	 * No secondary should be executed since master is stalled. If there
	 * was no dependency chain at all, the secondaries would start
	 * immediately.
	 */
	usleep(20000);
	igt_assert(gem_bo_busy(i915, spin->handle));
	igt_assert(gem_bo_busy(i915, obj[0].handle));
	igt_cork_unplug(&cork);

	/*
	 * Wait for all secondaries to complete. If we used a regular fence
	 * then the secondaries would not start until the master was complete.
	 * In this case that can only happen with a GPU reset, and so we run
	 * under the hang detector and double check that the master is still
	 * running afterwards.
	 */
	gem_read(i915, obj[0].handle, 0, &result, sizeof(result));
	igt_assert_eq_u32(result, 0xd0df0d);
	gem_close(i915, obj[0].handle);
	put_offset(ahnd, obj[0].handle);

	/* Master should still be spinning, but all output should be written */
	igt_assert(gem_bo_busy(i915, spin->handle));
	igt_spin_free(i915, spin);
	put_ahnd(ahnd);
}

static void test_submit_chain(int i915, const intel_ctx_t *ctx)
{
	const struct intel_execution_engine2 *e;
	igt_spin_t *spin, *sn;
	IGT_LIST_HEAD(list);
	IGT_CORK_FENCE(cork);
	int fence;
	uint64_t ahnd = get_reloc_ahnd(i915, ctx->id);

	/* Check that we can simultaneously launch spinners on each engine */

	fence = igt_cork_plug(&cork, i915);
	for_each_ctx_engine(i915, ctx, e) {
		spin = igt_spin_new(i915,
				    .ahnd = ahnd,
				    .ctx = ctx,
				    .engine = e->flags,
				    .fence = fence,
				    .flags = (IGT_SPIN_POLL_RUN |
					      IGT_SPIN_FENCE_OUT |
					      IGT_SPIN_FENCE_SUBMIT));

		fence = spin->out_fence;
		igt_list_move(&spin->link, &list);
	}

	/* Nothing shall run until we pop the cork */
	igt_list_for_each_entry(spin, &list, link) {
		igt_assert(gem_bo_busy(i915, spin->handle));
		igt_assert(!igt_spin_has_started(spin));
	}

	igt_cork_unplug(&cork);

	/* Then everything shall run in parallel */
	igt_list_for_each_entry_safe(spin, sn, &list, link) {
		igt_spin_busywait_until_started(spin);
		igt_spin_end(spin);
		igt_assert_eq(sync_fence_wait(spin->out_fence, 50), 0);
		igt_assert_eq(sync_fence_status(spin->out_fence), 1);
		igt_spin_free(i915, spin);
	}
	put_ahnd(ahnd);
}

static uint32_t batch_create(int fd)
{
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	uint32_t handle;

	handle = gem_create(fd, 4096);
	gem_write(fd, handle, 0, &bbe, sizeof(bbe));

	return handle;
}

static void test_keep_in_fence(int fd, const intel_ctx_t *ctx,
			       const struct intel_execution_engine2 *e)
{
	struct sigaction sa = { .sa_handler = alarm_handler };
	struct drm_i915_gem_exec_object2 obj = {
		.handle = batch_create(fd),
	};
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(&obj),
		.buffer_count = 1,
		.flags = e->flags | I915_EXEC_FENCE_OUT,
		.rsvd1 = ctx->id,
	};
	unsigned long count, last;
	struct itimerval itv;
	igt_spin_t *spin;
	uint64_t ahnd = get_reloc_ahnd(fd, ctx->id);
	int fence;

	spin = igt_spin_new(fd, .ahnd = ahnd, .ctx = ctx, .engine = e->flags);

	gem_execbuf_wr(fd, &execbuf);
	fence = upper_32_bits(execbuf.rsvd2);

	sigaction(SIGALRM, &sa, NULL);
	itv.it_interval.tv_sec = 0;
	itv.it_interval.tv_usec = 1000;
	itv.it_value.tv_sec = 0;
	itv.it_value.tv_usec = 10000;
	setitimer(ITIMER_REAL, &itv, NULL);

	execbuf.flags |= I915_EXEC_FENCE_IN;
	execbuf.rsvd2 = fence;

	last = -1;
	count = 0;
	do {
		int err = __execbuf(fd, &execbuf);

		igt_assert_eq(lower_32_bits(execbuf.rsvd2), fence);

		if (err == 0) {
			close(fence);

			fence = upper_32_bits(execbuf.rsvd2);
			execbuf.rsvd2 = fence;

			count++;
			continue;
		}

		igt_assert_eq(err, -EINTR);
		igt_assert_eq(upper_32_bits(execbuf.rsvd2), 0);

		if (last == count)
			break;

		last = count;
	} while (1);

	memset(&itv, 0, sizeof(itv));
	setitimer(ITIMER_REAL, &itv, NULL);

	gem_close(fd, obj.handle);
	close(fence);

	igt_spin_free(fd, spin);
	gem_quiescent_gpu(fd);
	put_ahnd(ahnd);
}

#define EXPIRED 0x10000
static void test_long_history(int fd, const intel_ctx_t *ctx,
			      long ring_size, unsigned flags)
{
	const uint32_t sz = 1 << 20;
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	struct drm_i915_gem_exec_object2 obj[2];
	struct drm_i915_gem_execbuffer2 execbuf;
	const struct intel_execution_engine2 *e;
	unsigned int engines[I915_EXEC_RING_MASK + 1], nengine, n, s;
	unsigned long limit;
	int all_fences;
	IGT_CORK_HANDLE(c);

	limit = -1;
	if (!gem_uses_full_ppgtt(fd))
		limit = ring_size / 3;

	nengine = 0;
	for_each_ctx_engine(fd, ctx, e)
		engines[nengine++] = e->flags;
	igt_require(nengine);

	gem_quiescent_gpu(fd);

	memset(obj, 0, sizeof(obj));
	obj[1].handle = gem_create(fd, sz);
	gem_write(fd, obj[1].handle, sz - sizeof(bbe), &bbe, sizeof(bbe));

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(&obj[1]);
	execbuf.buffer_count = 1;
	execbuf.flags = I915_EXEC_FENCE_OUT;
	execbuf.rsvd1 = ctx->id;

	gem_execbuf_wr(fd, &execbuf);
	all_fences = execbuf.rsvd2 >> 32;

	execbuf.buffers_ptr = to_user_pointer(obj);
	execbuf.buffer_count = 2;

	obj[0].handle = igt_cork_plug(&c, fd);

	igt_until_timeout(5) {
		const intel_ctx_t *tmp_ctx = intel_ctx_create(fd, &ctx->cfg);
		execbuf.rsvd1 = tmp_ctx->id;

		for (n = 0; n < nengine; n++) {
			struct sync_merge_data merge;

			execbuf.flags = engines[n] | I915_EXEC_FENCE_OUT;
			if (__gem_execbuf_wr(fd, &execbuf))
				continue;

			memset(&merge, 0, sizeof(merge));
			merge.fd2 = execbuf.rsvd2 >> 32;
			strcpy(merge.name, "igt");

			do_ioctl(all_fences, SYNC_IOC_MERGE, &merge);

			close(all_fences);
			close(merge.fd2);

			all_fences = merge.fence;
		}

		intel_ctx_destroy(fd, tmp_ctx);
		if (!--limit)
			break;
	}
	igt_cork_unplug(&c);

	igt_info("History depth = %d\n", sync_fence_count(all_fences));

	if (flags & EXPIRED)
		gem_sync(fd, obj[1].handle);

	execbuf.buffers_ptr = to_user_pointer(&obj[1]);
	execbuf.buffer_count = 1;
	execbuf.rsvd2 = all_fences;
	execbuf.rsvd1 = ctx->id;

	for (s = 0; s < ring_size; s++) {
		for (n = 0; n < nengine; n++) {
			execbuf.flags = engines[n] | I915_EXEC_FENCE_IN;
			if (__gem_execbuf_wr(fd, &execbuf))
				continue;
		}
	}

	close(all_fences);

	gem_sync(fd, obj[1].handle);
	gem_close(fd, obj[1].handle);
	gem_close(fd, obj[0].handle);
}

static bool has_submit_fence(int fd)
{
	struct drm_i915_getparam gp;
	int value = 0;

	memset(&gp, 0, sizeof(gp));
	gp.param = I915_PARAM_HAS_EXEC_SUBMIT_FENCE;
	gp.value = &value;

	ioctl(fd, DRM_IOCTL_I915_GETPARAM, &gp, sizeof(gp));
	errno = 0;

	return value;
}

static bool has_syncobj(int fd)
{
	struct drm_get_cap cap = { .capability = DRM_CAP_SYNCOBJ };
	ioctl(fd, DRM_IOCTL_GET_CAP, &cap);
	return cap.value;
}

static bool exec_has_fence_array(int fd)
{
	struct drm_i915_getparam gp;
	int value = 0;

	memset(&gp, 0, sizeof(gp));
	gp.param = I915_PARAM_HAS_EXEC_FENCE_ARRAY;
	gp.value = &value;

	ioctl(fd, DRM_IOCTL_I915_GETPARAM, &gp, sizeof(gp));
	errno = 0;

	return value;
}

static void test_invalid_fence_array(int fd)
{
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 obj;
	struct drm_i915_gem_exec_fence fence;
	void *ptr;

	/* create an otherwise valid execbuf */
	memset(&obj, 0, sizeof(obj));
	obj.handle = gem_create(fd, 4096);
	gem_write(fd, obj.handle, 0, &bbe, sizeof(bbe));
	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(&obj);
	execbuf.buffer_count = 1;
	gem_execbuf(fd, &execbuf);

	execbuf.flags |= I915_EXEC_FENCE_ARRAY;
	gem_execbuf(fd, &execbuf);

	/* Now add a few invalid fence-array pointers */
	if (sizeof(execbuf.num_cliprects) == sizeof(size_t)) {
		execbuf.num_cliprects = -1;
		igt_assert_eq(__gem_execbuf(fd, &execbuf), -EINVAL);
	}

	execbuf.num_cliprects = 1;
	execbuf.cliprects_ptr = -1;
	igt_assert_eq(__gem_execbuf(fd, &execbuf), -EFAULT);

	memset(&fence, 0, sizeof(fence));
	execbuf.cliprects_ptr = to_user_pointer(&fence);
	igt_assert_eq(__gem_execbuf(fd, &execbuf), -ENOENT);

	ptr = mmap(NULL, 4096, PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
	igt_assert(ptr != MAP_FAILED);
	execbuf.cliprects_ptr = to_user_pointer(ptr);
	igt_assert_eq(__gem_execbuf(fd, &execbuf), -ENOENT);

	do_or_die(mprotect(ptr, 4096, PROT_READ));
	igt_assert_eq(__gem_execbuf(fd, &execbuf), -ENOENT);

	do_or_die(mprotect(ptr, 4096, PROT_NONE));
	igt_assert_eq(__gem_execbuf(fd, &execbuf), -EFAULT);

	munmap(ptr, 4096);
}

static int __syncobj_to_sync_file(int fd, uint32_t handle)
{
	struct drm_syncobj_handle arg = {
		.handle = handle,
		.flags = DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE,
	};

	return __syncobj_handle_to_fd(fd, &arg);
}

static int syncobj_export(int fd, uint32_t handle)
{
	return syncobj_handle_to_fd(fd, handle, 0);
}

static uint32_t syncobj_import(int fd, int syncobj)
{
	return syncobj_fd_to_handle(fd, syncobj, 0);
}

static bool syncobj_busy(int fd, uint32_t handle)
{
	bool result;
	int sf;

	sf = syncobj_handle_to_fd(fd, handle,
				  DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE);
	result = poll(&(struct pollfd){sf, POLLIN}, 1, 0) == 0;
	close(sf);

	return result;
}

static void test_syncobj_unused_fence(int fd)
{
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	struct drm_i915_gem_exec_object2 obj;
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_fence fence = {
		.handle = syncobj_create(fd, 0),
	};
	uint64_t ahnd = get_reloc_ahnd(fd, 0);
	igt_spin_t *spin = igt_spin_new(fd, .ahnd = ahnd);

	/* sanity check our syncobj_to_sync_file interface */
	igt_assert_eq(__syncobj_to_sync_file(fd, 0), -ENOENT);

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(&obj);
	execbuf.buffer_count = 1;
	execbuf.flags = I915_EXEC_FENCE_ARRAY;
	execbuf.cliprects_ptr = to_user_pointer(&fence);
	execbuf.num_cliprects = 1;

	memset(&obj, 0, sizeof(obj));
	obj.handle = gem_create(fd, 4096);
	gem_write(fd, obj.handle, 0, &bbe, sizeof(bbe));

	gem_execbuf(fd, &execbuf);

	/* no flags, the fence isn't created */
	igt_assert_eq(__syncobj_to_sync_file(fd, fence.handle), -EINVAL);
	igt_assert(gem_bo_busy(fd, obj.handle));

	gem_close(fd, obj.handle);
	syncobj_destroy(fd, fence.handle);

	igt_spin_free(fd, spin);
	put_ahnd(ahnd);
}

static void test_syncobj_invalid_wait(int fd)
{
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	struct drm_i915_gem_exec_object2 obj;
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_fence fence = {
		.handle = syncobj_create(fd, 0),
	};

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(&obj);
	execbuf.buffer_count = 1;
	execbuf.flags = I915_EXEC_FENCE_ARRAY;
	execbuf.cliprects_ptr = to_user_pointer(&fence);
	execbuf.num_cliprects = 1;

	memset(&obj, 0, sizeof(obj));
	obj.handle = gem_create(fd, 4096);
	gem_write(fd, obj.handle, 0, &bbe, sizeof(bbe));

	/* waiting before the fence is set is invalid */
	fence.flags = I915_EXEC_FENCE_WAIT;
	igt_assert_eq(__gem_execbuf(fd, &execbuf), -EINVAL);

	gem_close(fd, obj.handle);
	syncobj_destroy(fd, fence.handle);
}

static void test_syncobj_invalid_flags(int fd)
{
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	struct drm_i915_gem_exec_object2 obj;
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_fence fence = {
		.handle = syncobj_create(fd, 0),
	};

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(&obj);
	execbuf.buffer_count = 1;
	execbuf.flags = I915_EXEC_FENCE_ARRAY;
	execbuf.cliprects_ptr = to_user_pointer(&fence);
	execbuf.num_cliprects = 1;

	memset(&obj, 0, sizeof(obj));
	obj.handle = gem_create(fd, 4096);
	gem_write(fd, obj.handle, 0, &bbe, sizeof(bbe));

	/* set all flags to hit an invalid one */
	fence.flags = ~0;
	igt_assert_eq(__gem_execbuf(fd, &execbuf), -EINVAL);

	gem_close(fd, obj.handle);
	syncobj_destroy(fd, fence.handle);
}

static void test_syncobj_signal(int fd)
{
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	struct drm_i915_gem_exec_object2 obj;
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_fence fence = {
		.handle = syncobj_create(fd, 0),
	};
	uint64_t ahnd = get_reloc_ahnd(fd, 0);
	igt_spin_t *spin = igt_spin_new(fd, .ahnd = ahnd);

	/* Check that the syncobj is signaled only when our request/fence is */

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(&obj);
	execbuf.buffer_count = 1;
	execbuf.flags = I915_EXEC_FENCE_ARRAY;
	execbuf.cliprects_ptr = to_user_pointer(&fence);
	execbuf.num_cliprects = 1;

	memset(&obj, 0, sizeof(obj));
	obj.handle = gem_create(fd, 4096);
	gem_write(fd, obj.handle, 0, &bbe, sizeof(bbe));

	fence.flags = I915_EXEC_FENCE_SIGNAL;
	gem_execbuf(fd, &execbuf);

	igt_assert(gem_bo_busy(fd, obj.handle));
	igt_assert(syncobj_busy(fd, fence.handle));

	igt_spin_free(fd, spin);

	gem_sync(fd, obj.handle);
	igt_assert(!gem_bo_busy(fd, obj.handle));
	igt_assert(!syncobj_busy(fd, fence.handle));

	gem_close(fd, obj.handle);
	syncobj_destroy(fd, fence.handle);
	put_ahnd(ahnd);
}

static void test_syncobj_wait(int fd, const intel_ctx_t *ctx)
{
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	struct drm_i915_gem_exec_object2 obj;
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_fence fence = {
		.handle = syncobj_create(fd, 0),
	};
	const struct intel_execution_engine2 *e;
	unsigned handle[I915_EXEC_RING_MASK + 1];
	igt_spin_t *spin;
	int n;
	uint64_t ahnd = get_reloc_ahnd(fd, 0);

	/* Check that we can use the syncobj to asynchronous wait prior to
	 * execution.
	 */

	gem_quiescent_gpu(fd);

	spin = igt_spin_new(fd, .ahnd = ahnd);

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(&obj);
	execbuf.buffer_count = 1;

	memset(&obj, 0, sizeof(obj));
	obj.handle = gem_create(fd, 4096);
	gem_write(fd, obj.handle, 0, &bbe, sizeof(bbe));

	/* Queue a signaler from the blocked engine */
	execbuf.flags = I915_EXEC_FENCE_ARRAY;
	execbuf.cliprects_ptr = to_user_pointer(&fence);
	execbuf.num_cliprects = 1;
	fence.flags = I915_EXEC_FENCE_SIGNAL;
	gem_execbuf(fd, &execbuf);
	igt_assert(gem_bo_busy(fd, spin->handle));

	gem_close(fd, obj.handle);
	obj.handle = gem_create(fd, 4096);
	gem_write(fd, obj.handle, 0, &bbe, sizeof(bbe));

	n = 0;
	for_each_ctx_engine(fd, ctx, e) {
		obj.handle = gem_create(fd, 4096);
		gem_write(fd, obj.handle, 0, &bbe, sizeof(bbe));

		/* Now wait upon the blocked engine */
		execbuf.flags = I915_EXEC_FENCE_ARRAY | e->flags;
		execbuf.rsvd1 = ctx->id;
		execbuf.cliprects_ptr = to_user_pointer(&fence);
		execbuf.num_cliprects = 1;
		fence.flags = I915_EXEC_FENCE_WAIT;
		gem_execbuf(fd, &execbuf);

		igt_assert(gem_bo_busy(fd, obj.handle));
		handle[n++] = obj.handle;
	}
	igt_assert(gem_bo_busy(fd, spin->handle));
	syncobj_destroy(fd, fence.handle);

	for (int i = 0; i < n; i++)
		igt_assert(gem_bo_busy(fd, handle[i]));

	igt_spin_free(fd, spin);

	for (int i = 0; i < n; i++) {
		gem_sync(fd, handle[i]);
		gem_close(fd, handle[i]);
	}

	put_ahnd(ahnd);
}

static void test_syncobj_export(int fd)
{
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	struct drm_i915_gem_exec_object2 obj;
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_fence fence = {
		.handle = syncobj_create(fd, 0),
	};
	int export[2];
	igt_spin_t *spin;
	uint64_t ahnd = get_reloc_ahnd(fd, 0);

	spin = igt_spin_new(fd, .ahnd = ahnd);

	/* Check that if we export the syncobj prior to use it picks up
	 * the later fence. This allows a syncobj to establish a channel
	 * between clients that may be updated to a later fence by either
	 * end.
	 */
	for (int n = 0; n < ARRAY_SIZE(export); n++)
		export[n] = syncobj_export(fd, fence.handle);

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(&obj);
	execbuf.buffer_count = 1;
	execbuf.flags = I915_EXEC_FENCE_ARRAY;
	execbuf.cliprects_ptr = to_user_pointer(&fence);
	execbuf.num_cliprects = 1;

	memset(&obj, 0, sizeof(obj));
	obj.handle = gem_create(fd, 4096);
	gem_write(fd, obj.handle, 0, &bbe, sizeof(bbe));

	fence.flags = I915_EXEC_FENCE_SIGNAL;
	gem_execbuf(fd, &execbuf);

	igt_assert(syncobj_busy(fd, fence.handle));
	igt_assert(gem_bo_busy(fd, obj.handle));

	for (int n = 0; n < ARRAY_SIZE(export); n++) {
		uint32_t import = syncobj_import(fd, export[n]);
		igt_assert(syncobj_busy(fd, import));
		syncobj_destroy(fd, import);
	}

	igt_spin_free(fd, spin);

	gem_sync(fd, obj.handle);
	igt_assert(!gem_bo_busy(fd, obj.handle));
	igt_assert(!syncobj_busy(fd, fence.handle));

	gem_close(fd, obj.handle);
	syncobj_destroy(fd, fence.handle);

	for (int n = 0; n < ARRAY_SIZE(export); n++) {
		uint32_t import = syncobj_import(fd, export[n]);
		igt_assert(!syncobj_busy(fd, import));
		syncobj_destroy(fd, import);
		close(export[n]);
	}

	put_ahnd(ahnd);
}

static void test_syncobj_repeat(int fd)
{
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	const unsigned nfences = 4096;
	struct drm_i915_gem_exec_object2 obj;
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_fence *fence;
	int export;
	igt_spin_t *spin;
	uint64_t ahnd = get_reloc_ahnd(fd, 0);

	spin = igt_spin_new(fd, .ahnd = ahnd);

	/* Check that we can wait on the same fence multiple times */
	fence = calloc(nfences, sizeof(*fence));
	fence->handle = syncobj_create(fd, 0);
	export = syncobj_export(fd, fence->handle);
	for (int i = 1; i < nfences; i++)
		fence[i].handle = syncobj_import(fd, export);
	close(export);

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(&obj);
	execbuf.buffer_count = 1;
	execbuf.flags = I915_EXEC_FENCE_ARRAY;
	execbuf.cliprects_ptr = to_user_pointer(fence);
	execbuf.num_cliprects = nfences;

	memset(&obj, 0, sizeof(obj));
	obj.handle = gem_create(fd, 4096);
	gem_write(fd, obj.handle, 0, &bbe, sizeof(bbe));

	for (int i = 0; i < nfences; i++)
		fence[i].flags = I915_EXEC_FENCE_SIGNAL;

	gem_execbuf(fd, &execbuf);

	for (int i = 0; i < nfences; i++) {
		igt_assert(syncobj_busy(fd, fence[i].handle));
		fence[i].flags |= I915_EXEC_FENCE_WAIT;
	}
	igt_assert(gem_bo_busy(fd, obj.handle));

	gem_execbuf(fd, &execbuf);

	for (int i = 0; i < nfences; i++)
		igt_assert(syncobj_busy(fd, fence[i].handle));
	igt_assert(gem_bo_busy(fd, obj.handle));

	igt_spin_free(fd, spin);

	gem_sync(fd, obj.handle);
	gem_close(fd, obj.handle);

	for (int i = 0; i < nfences; i++) {
		igt_assert(!syncobj_busy(fd, fence[i].handle));
		syncobj_destroy(fd, fence[i].handle);
	}
	free(fence);

	put_ahnd(ahnd);
}

static void test_syncobj_import(int fd)
{
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	struct drm_i915_gem_exec_object2 obj;
	struct drm_i915_gem_execbuffer2 execbuf;
	uint64_t ahnd = get_reloc_ahnd(fd, 0);
	igt_spin_t *spin = igt_spin_new(fd, .ahnd = ahnd);
	uint32_t sync = syncobj_create(fd, 0);
	int fence;

	/* Check that we can create a syncobj from an explicit fence (which
	 * uses sync_file) and that it acts just like a regular fence.
	 */

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(&obj);
	execbuf.buffer_count = 1;
	execbuf.flags = I915_EXEC_FENCE_OUT;
	execbuf.rsvd2 = -1;

	memset(&obj, 0, sizeof(obj));
	obj.handle = gem_create(fd, 4096);
	gem_write(fd, obj.handle, 0, &bbe, sizeof(bbe));

	gem_execbuf_wr(fd, &execbuf);

	fence = execbuf.rsvd2 >> 32;
	igt_assert(fence_busy(fence));
	syncobj_import_sync_file(fd, sync, fence);
	close(fence);

	igt_assert(gem_bo_busy(fd, obj.handle));
	igt_assert(syncobj_busy(fd, sync));

	igt_spin_free(fd, spin);

	gem_sync(fd, obj.handle);
	igt_assert(!gem_bo_busy(fd, obj.handle));
	igt_assert(!syncobj_busy(fd, sync));

	gem_close(fd, obj.handle);
	syncobj_destroy(fd, sync);
	put_ahnd(ahnd);
}

static void test_syncobj_channel(int fd)
{
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	struct drm_i915_gem_exec_object2 obj;
	struct drm_i915_gem_execbuffer2 execbuf;
	unsigned int *control;
	int syncobj[3];

	/* Create a pair of channels (like a pipe) between two clients
	 * and try to create races on the syncobj.
	 */

	control = mmap(NULL, 4096, PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
	igt_assert(control != MAP_FAILED);

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(&obj);
	execbuf.buffer_count = 1;
	execbuf.flags = I915_EXEC_FENCE_OUT;
	execbuf.rsvd2 = -1;

	memset(&obj, 0, sizeof(obj));
	obj.handle = gem_create(fd, 4096);
	gem_write(fd, obj.handle, 0, &bbe, sizeof(bbe));

	for (int i = 0; i < ARRAY_SIZE(syncobj); i++) {
		struct drm_i915_gem_exec_fence fence;

		execbuf.flags = I915_EXEC_FENCE_ARRAY;
		execbuf.cliprects_ptr = to_user_pointer(&fence);
		execbuf.num_cliprects = 1;

		/* Create a primed fence */
		fence.handle = syncobj_create(fd, 0);
		fence.flags = I915_EXEC_FENCE_SIGNAL;

		gem_execbuf(fd, &execbuf);

		syncobj[i] = fence.handle;
	}

	/* Two processes in ping-pong unison (pipe), one out of sync */
	igt_fork(child, 1) {
		struct drm_i915_gem_exec_fence fence[3];
		unsigned long count;

		execbuf.flags = I915_EXEC_FENCE_ARRAY;
		execbuf.cliprects_ptr = to_user_pointer(fence);
		execbuf.num_cliprects = 3;

		fence[0].handle = syncobj[0];
		fence[0].flags = I915_EXEC_FENCE_SIGNAL;

		fence[1].handle = syncobj[1];
		fence[1].flags = I915_EXEC_FENCE_WAIT;

		fence[2].handle = syncobj[2];
		fence[2].flags = I915_EXEC_FENCE_WAIT;

		count = 0;
		while (!*(volatile unsigned *)control) {
			gem_execbuf(fd, &execbuf);
			count++;
		}

		control[1] = count;
	}
	igt_fork(child, 1) {
		struct drm_i915_gem_exec_fence fence[3];
		unsigned long count;

		execbuf.flags = I915_EXEC_FENCE_ARRAY;
		execbuf.cliprects_ptr = to_user_pointer(fence);
		execbuf.num_cliprects = 3;

		fence[0].handle = syncobj[0];
		fence[0].flags = I915_EXEC_FENCE_WAIT;

		fence[1].handle = syncobj[1];
		fence[1].flags = I915_EXEC_FENCE_SIGNAL;

		fence[2].handle = syncobj[2];
		fence[2].flags = I915_EXEC_FENCE_WAIT;

		count = 0;
		while (!*(volatile unsigned *)control) {
			gem_execbuf(fd, &execbuf);
			count++;
		}
		control[2] = count;
	}
	igt_fork(child, 1) {
		struct drm_i915_gem_exec_fence fence;
		unsigned long count;

		execbuf.flags = I915_EXEC_FENCE_ARRAY;
		execbuf.cliprects_ptr = to_user_pointer(&fence);
		execbuf.num_cliprects = 1;

		fence.handle = syncobj[2];
		fence.flags = I915_EXEC_FENCE_SIGNAL;

		count = 0;
		while (!*(volatile unsigned *)control) {
			gem_execbuf(fd, &execbuf);
			count++;
		}
		control[3] = count;
	}

	sleep(1);
	*control = 1;
	igt_waitchildren();

	igt_info("Pipe=[%u, %u], gooseberry=%u\n",
		 control[1], control[2], control[3]);
	munmap(control, 4096);

	gem_sync(fd, obj.handle);
	gem_close(fd, obj.handle);

	for (int i = 0; i < ARRAY_SIZE(syncobj); i++)
		syncobj_destroy(fd, syncobj[i]);
}

static bool has_syncobj_timeline(int fd)
{
	struct drm_get_cap cap = { .capability = DRM_CAP_SYNCOBJ_TIMELINE };
	ioctl(fd, DRM_IOCTL_GET_CAP, &cap);
	return cap.value;
}

static bool exec_has_timeline_fences(int fd)
{
	struct drm_i915_getparam gp;
	int value = 0;

	memset(&gp, 0, sizeof(gp));
	gp.param = I915_PARAM_HAS_EXEC_TIMELINE_FENCES;
	gp.value = &value;

	ioctl(fd, DRM_IOCTL_I915_GETPARAM, &gp, sizeof(gp));
	errno = 0;

	return value;
}

static const char *test_invalid_timeline_fence_array_desc =
	"Verifies invalid execbuf parameters in"
	" drm_i915_gem_execbuffer_ext_timeline_fences are rejected";
static void test_invalid_timeline_fence_array(int fd)
{
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 obj;
	struct drm_i915_gem_execbuffer_ext_timeline_fences timeline_fences;
	struct drm_i915_gem_exec_fence fence;
	uint64_t value;
	void *ptr;

	fd = gem_reopen_driver(fd);

	/* create an otherwise valid execbuf */
	memset(&obj, 0, sizeof(obj));
	obj.handle = gem_create(fd, 4096);
	gem_write(fd, obj.handle, 0, &bbe, sizeof(bbe));
	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(&obj);
	execbuf.buffer_count = 1;
	gem_execbuf(fd, &execbuf);

	/* Invalid num_cliprects value */
	execbuf.cliprects_ptr = to_user_pointer(&timeline_fences);
	execbuf.num_cliprects = 1;
	execbuf.flags = I915_EXEC_USE_EXTENSIONS;
	igt_assert_eq(__gem_execbuf(fd, &execbuf), -EINVAL);

	fence.handle = syncobj_create(fd, 0);
	fence.flags = I915_EXEC_FENCE_SIGNAL;
	value = 1;

	memset(&timeline_fences, 0, sizeof(timeline_fences));
	timeline_fences.base.name = DRM_I915_GEM_EXECBUFFER_EXT_TIMELINE_FENCES;
	timeline_fences.fence_count = 1;
	timeline_fences.handles_ptr = to_user_pointer(&fence);
	timeline_fences.values_ptr = to_user_pointer(&value);

	/* Invalid fence array & i915 ext */
	execbuf.cliprects_ptr = to_user_pointer(&timeline_fences);
	execbuf.num_cliprects = 0;
	execbuf.flags = I915_EXEC_FENCE_ARRAY | I915_EXEC_USE_EXTENSIONS;
	igt_assert_eq(__gem_execbuf(fd, &execbuf), -EINVAL);

	execbuf.flags = I915_EXEC_USE_EXTENSIONS;

	/* Invalid handles_ptr */
	value = 1;
	memset(&timeline_fences, 0, sizeof(timeline_fences));
	timeline_fences.base.name = DRM_I915_GEM_EXECBUFFER_EXT_TIMELINE_FENCES;
	timeline_fences.fence_count = 1;
	timeline_fences.handles_ptr = -1;
	timeline_fences.values_ptr = to_user_pointer(&value);
	igt_assert_eq(__gem_execbuf(fd, &execbuf), -EFAULT);

	/* Invalid values_ptr */
	value = 1;
	memset(&timeline_fences, 0, sizeof(timeline_fences));
	timeline_fences.base.name = DRM_I915_GEM_EXECBUFFER_EXT_TIMELINE_FENCES;
	timeline_fences.fence_count = 1;
	timeline_fences.handles_ptr = to_user_pointer(&fence);
	timeline_fences.values_ptr = -1;
	igt_assert_eq(__gem_execbuf(fd, &execbuf), -EFAULT);

	/* Invalid syncobj handle */
	memset(&fence, 0, sizeof(fence));
	fence.handle = 0;
	fence.flags = I915_EXEC_FENCE_WAIT;
	value = 1;
	memset(&timeline_fences, 0, sizeof(timeline_fences));
	timeline_fences.base.name = DRM_I915_GEM_EXECBUFFER_EXT_TIMELINE_FENCES;
	timeline_fences.fence_count = 1;
	timeline_fences.handles_ptr = to_user_pointer(&fence);
	timeline_fences.values_ptr = to_user_pointer(&value);
	igt_assert_eq(__gem_execbuf(fd, &execbuf), -ENOENT);

	/* Invalid syncobj timeline point */
	memset(&fence, 0, sizeof(fence));
	fence.handle = syncobj_create(fd, 0);
	fence.flags = I915_EXEC_FENCE_WAIT;
	value = 1;
	memset(&timeline_fences, 0, sizeof(timeline_fences));
	timeline_fences.base.name = DRM_I915_GEM_EXECBUFFER_EXT_TIMELINE_FENCES;
	timeline_fences.fence_count = 1;
	timeline_fences.handles_ptr = to_user_pointer(&fence);
	timeline_fences.values_ptr = to_user_pointer(&value);
	igt_assert_eq(__gem_execbuf(fd, &execbuf), -EINVAL);
	syncobj_destroy(fd, fence.handle);

	/* Invalid handles_ptr */
	ptr = mmap(NULL, 4096, PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
	igt_assert(ptr != MAP_FAILED);
	memset(&timeline_fences, 0, sizeof(timeline_fences));
	timeline_fences.base.name = DRM_I915_GEM_EXECBUFFER_EXT_TIMELINE_FENCES;
	timeline_fences.fence_count = 1;
	timeline_fences.handles_ptr = to_user_pointer(ptr);
	timeline_fences.values_ptr = to_user_pointer(&value);
	igt_assert_eq(__gem_execbuf(fd, &execbuf), -ENOENT);

	do_or_die(mprotect(ptr, 4096, PROT_READ));
	igt_assert_eq(__gem_execbuf(fd, &execbuf), -ENOENT);

	do_or_die(mprotect(ptr, 4096, PROT_NONE));
	igt_assert_eq(__gem_execbuf(fd, &execbuf), -EFAULT);

	munmap(ptr, 4096);

	/* Invalid values_ptr */
	ptr = mmap(NULL, 4096, PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
	igt_assert(ptr != MAP_FAILED);
	memset(&timeline_fences, 0, sizeof(timeline_fences));
	timeline_fences.base.name = DRM_I915_GEM_EXECBUFFER_EXT_TIMELINE_FENCES;
	timeline_fences.fence_count = 1;
	timeline_fences.handles_ptr = to_user_pointer(&fence);
	timeline_fences.values_ptr = to_user_pointer(ptr);
	igt_assert_eq(__gem_execbuf(fd, &execbuf), -ENOENT);

	do_or_die(mprotect(ptr, 4096, PROT_READ));
	igt_assert_eq(__gem_execbuf(fd, &execbuf), -ENOENT);

	do_or_die(mprotect(ptr, 4096, PROT_NONE));
	igt_assert_eq(__gem_execbuf(fd, &execbuf), -EFAULT);

	munmap(ptr, 4096);
	close(fd);
}

static const char *test_syncobj_timeline_unused_fence_desc =
	"Verifies that a timeline syncobj passed into"
	" drm_i915_gem_execbuffer_ext_timeline_fences but with no signal/wait"
	" flag is left untouched";
static void test_syncobj_timeline_unused_fence(int fd)
{
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	struct drm_i915_gem_exec_object2 obj;
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_execbuffer_ext_timeline_fences timeline_fences;
	struct drm_i915_gem_exec_fence fence = {
		.handle = syncobj_create(fd, 0),
	};
	uint64_t value = 1, ahnd = get_reloc_ahnd(fd, 0);
	igt_spin_t *spin = igt_spin_new(fd, .ahnd = ahnd);

	/* sanity check our syncobj_to_sync_file interface */
	igt_assert_eq(__syncobj_to_sync_file(fd, 0), -ENOENT);

	memset(&timeline_fences, 0, sizeof(timeline_fences));
	timeline_fences.base.name = DRM_I915_GEM_EXECBUFFER_EXT_TIMELINE_FENCES;
	timeline_fences.fence_count = 1;
	timeline_fences.handles_ptr = to_user_pointer(&fence);
	timeline_fences.values_ptr = to_user_pointer(&value);

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(&obj);
	execbuf.buffer_count = 1;
	execbuf.flags = I915_EXEC_USE_EXTENSIONS;
	execbuf.cliprects_ptr = to_user_pointer(&timeline_fences);
	execbuf.num_cliprects = 0;

	memset(&obj, 0, sizeof(obj));
	obj.handle = gem_create(fd, 4096);
	gem_write(fd, obj.handle, 0, &bbe, sizeof(bbe));

	gem_execbuf(fd, &execbuf);

	/* no flags, the fence isn't created */
	igt_assert_eq(__syncobj_to_sync_file(fd, fence.handle), -EINVAL);
	igt_assert(gem_bo_busy(fd, obj.handle));

	gem_close(fd, obj.handle);
	syncobj_destroy(fd, fence.handle);

	igt_spin_free(fd, spin);
	put_ahnd(ahnd);
}

static const char *test_syncobj_timeline_invalid_wait_desc =
	"Verifies that submitting an execbuf with a wait on a timeline syncobj"
	" point that does not exists is rejected";
static void test_syncobj_timeline_invalid_wait(int fd)
{
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	struct drm_i915_gem_exec_object2 obj;
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_execbuffer_ext_timeline_fences timeline_fences;
	struct drm_i915_gem_exec_fence fence = {
		.handle = syncobj_create(fd, 0),
	};
	uint64_t value = 1;

	memset(&timeline_fences, 0, sizeof(timeline_fences));
	timeline_fences.base.name = DRM_I915_GEM_EXECBUFFER_EXT_TIMELINE_FENCES;
	timeline_fences.fence_count = 1;
	timeline_fences.handles_ptr = to_user_pointer(&fence);
	timeline_fences.values_ptr = to_user_pointer(&value);

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(&obj);
	execbuf.buffer_count = 1;
	execbuf.flags = I915_EXEC_USE_EXTENSIONS;
	execbuf.cliprects_ptr = to_user_pointer(&timeline_fences);
	execbuf.num_cliprects = 0;

	memset(&obj, 0, sizeof(obj));
	obj.handle = gem_create(fd, 4096);
	gem_write(fd, obj.handle, 0, &bbe, sizeof(bbe));

	/* waiting before the fence point 1 is set is invalid */
	fence.flags = I915_EXEC_FENCE_WAIT;
	igt_assert_eq(__gem_execbuf(fd, &execbuf), -EINVAL);

	/* Now set point 1. */
	fence.flags = I915_EXEC_FENCE_SIGNAL;
	gem_execbuf(fd, &execbuf);

	/* waiting before the fence point 2 is set is invalid */
	value = 2;
	fence.flags = I915_EXEC_FENCE_WAIT;
	igt_assert_eq(__gem_execbuf(fd, &execbuf), -EINVAL);

	gem_close(fd, obj.handle);
	syncobj_destroy(fd, fence.handle);
}

static const char *test_syncobj_timeline_invalid_flags_desc =
	"Verifies that invalid fence flags in"
	" drm_i915_gem_execbuffer_ext_timeline_fences are rejected";
static void test_syncobj_timeline_invalid_flags(int fd)
{
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	struct drm_i915_gem_exec_object2 obj;
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_execbuffer_ext_timeline_fences timeline_fences;
	struct drm_i915_gem_exec_fence fence = {
		.handle = syncobj_create(fd, 0),
	};
	uint64_t value = 1;

	memset(&timeline_fences, 0, sizeof(timeline_fences));
	timeline_fences.base.name = DRM_I915_GEM_EXECBUFFER_EXT_TIMELINE_FENCES;
	timeline_fences.fence_count = 1;
	timeline_fences.handles_ptr = to_user_pointer(&fence);
	timeline_fences.values_ptr = to_user_pointer(&value);

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(&obj);
	execbuf.buffer_count = 1;
	execbuf.flags = I915_EXEC_USE_EXTENSIONS;
	execbuf.cliprects_ptr = to_user_pointer(&timeline_fences);
	execbuf.num_cliprects = 0;

	memset(&obj, 0, sizeof(obj));
	obj.handle = gem_create(fd, 4096);
	gem_write(fd, obj.handle, 0, &bbe, sizeof(bbe));

	/* set all flags to hit an invalid one */
	fence.flags = ~0;
	igt_assert_eq(__gem_execbuf(fd, &execbuf), -EINVAL);

	gem_close(fd, obj.handle);
	syncobj_destroy(fd, fence.handle);
}

static uint64_t
gettime_ns(void)
{
	struct timespec current;
	clock_gettime(CLOCK_MONOTONIC, &current);
	return (uint64_t)current.tv_sec * NSEC_PER_SEC + current.tv_nsec;
}

static const char *test_syncobj_timeline_signal_desc =
	"Verifies proper signaling of a timeline syncobj through execbuf";
static void test_syncobj_timeline_signal(int fd)
{
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	struct drm_i915_gem_exec_object2 obj;
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_execbuffer_ext_timeline_fences timeline_fences;
	struct drm_i915_gem_exec_fence fence = {
		.handle = syncobj_create(fd, 0),
	};
	uint64_t value = 42, query_value, ahnd = get_reloc_ahnd(fd, 0);
	igt_spin_t *spin;

	/* Check that the syncobj is signaled only when our request/fence is */

	memset(&timeline_fences, 0, sizeof(timeline_fences));
	timeline_fences.base.name = DRM_I915_GEM_EXECBUFFER_EXT_TIMELINE_FENCES;
	timeline_fences.fence_count = 1;
	timeline_fences.handles_ptr = to_user_pointer(&fence);
	timeline_fences.values_ptr = to_user_pointer(&value);

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(&obj);
	execbuf.buffer_count = 1;
	execbuf.flags = I915_EXEC_USE_EXTENSIONS;
	execbuf.cliprects_ptr = to_user_pointer(&timeline_fences);
	execbuf.num_cliprects = 0;

	memset(&obj, 0, sizeof(obj));
	obj.handle = gem_create(fd, 4096);
	gem_write(fd, obj.handle, 0, &bbe, sizeof(bbe));

	fence.flags = I915_EXEC_FENCE_SIGNAL;

	/* Check syncobj after waiting on the buffer handle. */
	spin = igt_spin_new(fd, .ahnd = ahnd);
	gem_execbuf(fd, &execbuf);

	igt_assert(gem_bo_busy(fd, obj.handle));
	igt_assert(syncobj_busy(fd, fence.handle));
	igt_assert(syncobj_timeline_wait(fd, &fence.handle, &value, 1, 0,
					 DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE, NULL));
	igt_assert_eq(syncobj_timeline_wait_err(fd, &fence.handle, &value,
						1, 0, 0), -ETIME);

	igt_spin_free(fd, spin);

	gem_sync(fd, obj.handle);
	igt_assert(!syncobj_busy(fd, fence.handle));
	igt_assert(!gem_bo_busy(fd, obj.handle));

	syncobj_timeline_query(fd, &fence.handle, &query_value, 1);
	igt_assert_eq(query_value, value);

	spin = igt_spin_new(fd, .ahnd = ahnd);

	/*
	 * Wait on the syncobj and verify the state of the buffer
	 * handle.
	 */
	value = 84;
	gem_execbuf(fd, &execbuf);

	igt_assert(gem_bo_busy(fd, obj.handle));
	igt_assert(gem_bo_busy(fd, obj.handle));
	igt_assert(syncobj_busy(fd, fence.handle));
	igt_assert(syncobj_timeline_wait(fd, &fence.handle, &value, 1, 0,
					 DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE, NULL));
	igt_assert_eq(syncobj_timeline_wait_err(fd, &fence.handle, &value,
						1, 0, 0), -ETIME);

	igt_spin_free(fd, spin);

	igt_assert(syncobj_timeline_wait(fd, &fence.handle, &value, 1,
					 gettime_ns() + NSEC_PER_SEC,
					 DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT, NULL));

	igt_assert(!gem_bo_busy(fd, obj.handle));
	igt_assert(!syncobj_busy(fd, fence.handle));

	syncobj_timeline_query(fd, &fence.handle, &query_value, 1);
	igt_assert_eq(query_value, value);

	gem_close(fd, obj.handle);
	syncobj_destroy(fd, fence.handle);
	put_ahnd(ahnd);
}

static const char *test_syncobj_timeline_wait_desc =
	"Verifies that waiting on a timeline syncobj point between engines"
	" works";
static void test_syncobj_timeline_wait(int fd, const intel_ctx_t *ctx)
{
	const uint32_t bbe[2] = {
		MI_BATCH_BUFFER_END,
		MI_NOOP,
	};
	struct drm_i915_gem_exec_object2 obj;
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(&obj),
		.buffer_count = 1,
	};
	struct drm_i915_gem_execbuffer_ext_timeline_fences timeline_fences;
	struct drm_i915_gem_exec_fence fence = {
		.handle = syncobj_create(fd, 0),
	};
	unsigned handle[I915_EXEC_RING_MASK + 1];
	const struct intel_execution_engine2 *e;
	uint64_t value = 1, ahnd = get_reloc_ahnd(fd, ctx->id);
	igt_spin_t *spin;
	int n;

	/* Check that we can use the syncobj to asynchronous wait prior to
	 * execution.
	 */

	gem_quiescent_gpu(fd);

	spin = igt_spin_new(fd, .ahnd = ahnd, .ctx = ctx, .engine = ALL_ENGINES);

	memset(&timeline_fences, 0, sizeof(timeline_fences));
	timeline_fences.base.name = DRM_I915_GEM_EXECBUFFER_EXT_TIMELINE_FENCES;
	timeline_fences.fence_count = 1;
	timeline_fences.handles_ptr = to_user_pointer(&fence);
	timeline_fences.values_ptr = to_user_pointer(&value);

	memset(&obj, 0, sizeof(obj));
	obj.handle = gem_create(fd, 4096);
	gem_write(fd, obj.handle, 0, bbe, sizeof(bbe));

	/* Queue a signaler from the blocked engine */
	execbuf.flags = I915_EXEC_USE_EXTENSIONS;
	execbuf.cliprects_ptr = to_user_pointer(&timeline_fences);
	execbuf.num_cliprects = 0;
	fence.flags = I915_EXEC_FENCE_SIGNAL;
	gem_execbuf(fd, &execbuf);
	igt_assert(gem_bo_busy(fd, spin->handle));
	gem_close(fd, obj.handle);

	n = 0;
	for_each_ctx_engine(fd, ctx, e) {
		obj.handle = gem_create(fd, 4096);
		gem_write(fd, obj.handle, 0, bbe, sizeof(bbe));

		/* Now wait upon the blocked engine */
		execbuf.flags = I915_EXEC_USE_EXTENSIONS | e->flags;
		execbuf.rsvd1 = ctx->id,
		execbuf.cliprects_ptr = to_user_pointer(&timeline_fences);
		execbuf.num_cliprects = 0;
		fence.flags = I915_EXEC_FENCE_WAIT;
		gem_execbuf(fd, &execbuf);

		igt_assert(gem_bo_busy(fd, obj.handle));
		handle[n++] = obj.handle;
	}
	igt_assert(gem_bo_busy(fd, spin->handle));
	syncobj_destroy(fd, fence.handle);

	for (int i = 0; i < n; i++)
		igt_assert(gem_bo_busy(fd, handle[i]));

	igt_spin_free(fd, spin);

	for (int i = 0; i < n; i++) {
		gem_sync(fd, handle[i]);
		gem_close(fd, handle[i]);
	}
	put_ahnd(ahnd);
}

static const char *test_syncobj_timeline_export_desc =
	"Verify exporting of timeline syncobj signaled by i915";
static void test_syncobj_timeline_export(int fd)
{
	const uint32_t bbe[2] = {
		MI_BATCH_BUFFER_END,
		MI_NOOP,
	};
	struct drm_i915_gem_exec_object2 obj;
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_execbuffer_ext_timeline_fences timeline_fences;
	struct drm_i915_gem_exec_fence fence = {
		.handle = syncobj_create(fd, 0),
	};
	uint64_t value = 1, ahnd = get_reloc_ahnd(fd, 0);
	int export[2];
	igt_spin_t *spin = igt_spin_new(fd, .ahnd = ahnd);

	/* Check that if we export the syncobj prior to use it picks up
	 * the later fence. This allows a syncobj to establish a channel
	 * between clients that may be updated to a later fence by either
	 * end.
	 */
	for (int n = 0; n < ARRAY_SIZE(export); n++)
		export[n] = syncobj_export(fd, fence.handle);

	memset(&timeline_fences, 0, sizeof(timeline_fences));
	timeline_fences.base.name = DRM_I915_GEM_EXECBUFFER_EXT_TIMELINE_FENCES;
	timeline_fences.fence_count = 1;
	timeline_fences.handles_ptr = to_user_pointer(&fence);
	timeline_fences.values_ptr = to_user_pointer(&value);

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(&obj);
	execbuf.buffer_count = 1;
	execbuf.flags = I915_EXEC_USE_EXTENSIONS;
	execbuf.cliprects_ptr = to_user_pointer(&timeline_fences);
	execbuf.num_cliprects = 0;

	memset(&obj, 0, sizeof(obj));
	obj.handle = gem_create(fd, 4096);
	gem_write(fd, obj.handle, 0, bbe, sizeof(bbe));

	fence.flags = I915_EXEC_FENCE_SIGNAL;
	gem_execbuf(fd, &execbuf);

	igt_assert(syncobj_busy(fd, fence.handle));
	igt_assert(gem_bo_busy(fd, obj.handle));

	for (int n = 0; n < ARRAY_SIZE(export); n++) {
		uint32_t import = syncobj_import(fd, export[n]);
		igt_assert(syncobj_busy(fd, import));
		syncobj_destroy(fd, import);
	}

	igt_spin_free(fd, spin);

	gem_sync(fd, obj.handle);
	igt_assert(!gem_bo_busy(fd, obj.handle));
	igt_assert(!syncobj_busy(fd, fence.handle));

	gem_close(fd, obj.handle);
	syncobj_destroy(fd, fence.handle);

	for (int n = 0; n < ARRAY_SIZE(export); n++) {
		uint32_t import = syncobj_import(fd, export[n]);
		igt_assert(!syncobj_busy(fd, import));
		syncobj_destroy(fd, import);
		close(export[n]);
	}
	put_ahnd(ahnd);
}

static const char *test_syncobj_timeline_repeat_desc =
	"Verifies that waiting & signaling a same timeline syncobj point within"
	" the same execbuf fworks";
static void test_syncobj_timeline_repeat(int fd)
{
	const uint32_t bbe[2] = {
		MI_BATCH_BUFFER_END,
		MI_NOOP,
	};
	const unsigned nfences = 4096;
	struct drm_i915_gem_exec_object2 obj;
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_execbuffer_ext_timeline_fences timeline_fences;
	struct drm_i915_gem_exec_fence *fence;
	uint64_t *values, ahnd = get_reloc_ahnd(fd, 0);
	int export;
	igt_spin_t *spin = igt_spin_new(fd, .ahnd = ahnd);

	/* Check that we can wait on the same fence multiple times */
	fence = calloc(nfences, sizeof(*fence));
	values = calloc(nfences, sizeof(*values));
	fence->handle = syncobj_create(fd, 0);
	values[0] = 1;
	export = syncobj_export(fd, fence->handle);
	for (int i = 1; i < nfences; i++) {
		fence[i].handle = syncobj_import(fd, export);
		values[i] = i + 1;
	}
	close(export);

	memset(&timeline_fences, 0, sizeof(timeline_fences));
	timeline_fences.base.name = DRM_I915_GEM_EXECBUFFER_EXT_TIMELINE_FENCES;
	timeline_fences.fence_count = 1;
	timeline_fences.handles_ptr = to_user_pointer(fence);
	timeline_fences.values_ptr = to_user_pointer(values);

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(&obj);
	execbuf.buffer_count = 1;
	execbuf.flags = I915_EXEC_USE_EXTENSIONS;
	execbuf.cliprects_ptr = to_user_pointer(&timeline_fences);
	execbuf.num_cliprects = 0;

	memset(&obj, 0, sizeof(obj));
	obj.handle = gem_create(fd, 4096);
	gem_write(fd, obj.handle, 0, bbe, sizeof(bbe));

	for (int i = 0; i < nfences; i++)
		fence[i].flags = I915_EXEC_FENCE_SIGNAL;

	gem_execbuf(fd, &execbuf);

	for (int i = 0; i < nfences; i++) {
		igt_assert(syncobj_busy(fd, fence[i].handle));
		/*
		 * Timeline syncobj cannot resignal the same point
		 * again.
		 */
		fence[i].flags |= I915_EXEC_FENCE_WAIT;
	}
	igt_assert(gem_bo_busy(fd, obj.handle));

	igt_assert_eq(__gem_execbuf(fd, &execbuf), -EINVAL);

	for (int i = 0; i < nfences; i++) {
		igt_assert(syncobj_busy(fd, fence[i].handle));
		fence[i].flags = I915_EXEC_FENCE_WAIT;
	}
	igt_assert(gem_bo_busy(fd, obj.handle));

	gem_execbuf(fd, &execbuf);

	for (int i = 0; i < nfences; i++)
		igt_assert(syncobj_busy(fd, fence[i].handle));
	igt_assert(gem_bo_busy(fd, obj.handle));

	igt_spin_free(fd, spin);

	gem_sync(fd, obj.handle);
	gem_close(fd, obj.handle);

	for (int i = 0; i < nfences; i++) {
		igt_assert(!syncobj_busy(fd, fence[i].handle));
		syncobj_destroy(fd, fence[i].handle);
	}
	free(fence);
	free(values);
	put_ahnd(ahnd);
}

static const char *test_syncobj_timeline_multiple_ext_nodes_desc =
	"Verify that passing multiple execbuffer_ext nodes works";
static void test_syncobj_timeline_multiple_ext_nodes(int fd)
{
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	struct drm_i915_gem_exec_object2 obj;
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_execbuffer_ext_timeline_fences timeline_fences[8];
	uint32_t syncobjs[4];
	struct drm_i915_gem_exec_fence fences[8];
	uint64_t values[8];

	igt_assert(ARRAY_SIZE(syncobjs) < ARRAY_SIZE(values));

	for (uint32_t i = 0; i < ARRAY_SIZE(syncobjs); i++)
		syncobjs[i] = syncobj_create(fd, 0);

	/* Build a chain of
	 * drm_i915_gem_execbuffer_ext_timeline_fences, each signaling
	 * a syncobj at a particular point.
	 */
	for (uint32_t i = 0; i < ARRAY_SIZE(timeline_fences); i++) {
		uint32_t idx = ARRAY_SIZE(timeline_fences) - 1 - i;
		struct drm_i915_gem_execbuffer_ext_timeline_fences *iter =
			&timeline_fences[idx];
		struct drm_i915_gem_execbuffer_ext_timeline_fences *next =
			i == 0 ? NULL : &timeline_fences[ARRAY_SIZE(timeline_fences) - i];
		uint64_t *value = &values[idx];
		struct drm_i915_gem_exec_fence *fence = &fences[idx];

		fence->flags = I915_EXEC_FENCE_SIGNAL;
		fence->handle = syncobjs[idx % ARRAY_SIZE(syncobjs)];
		*value = 3 * i + 1;

		memset(iter, 0, sizeof(*iter));
		iter->base.name = DRM_I915_GEM_EXECBUFFER_EXT_TIMELINE_FENCES;
		iter->base.next_extension = to_user_pointer(next);
		iter->fence_count = 1;
		iter->handles_ptr = to_user_pointer(fence);
		iter->values_ptr = to_user_pointer(value);
	}

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(&obj);
	execbuf.buffer_count = 1;
	execbuf.flags = I915_EXEC_USE_EXTENSIONS;
	execbuf.cliprects_ptr = to_user_pointer(&timeline_fences[0]);
	execbuf.num_cliprects = 0;

	memset(&obj, 0, sizeof(obj));
	obj.handle = gem_create(fd, 4096);
	gem_write(fd, obj.handle, 0, &bbe, sizeof(bbe));

	gem_execbuf(fd, &execbuf);

	/* Wait on the last set of point signaled on each syncobj. */
	igt_assert(syncobj_timeline_wait(fd, syncobjs,
					 &values[ARRAY_SIZE(values) - ARRAY_SIZE(syncobjs)],
					 ARRAY_SIZE(syncobjs),
					 gettime_ns() + NSEC_PER_SEC,
					 0, NULL));

	igt_assert(!gem_bo_busy(fd, obj.handle));

	gem_close(fd, obj.handle);
	for (uint32_t i = 0; i < ARRAY_SIZE(syncobjs); i++)
		syncobj_destroy(fd, syncobjs[i]);
}

#define HSW_CS_GPR(n)                   (0x600 + 8*(n))
#define RING_TIMESTAMP                  (0x358)
#define MI_PREDICATE_RESULT_1           (0x41c)
#define MI_SET_PREDICATE_RESULT         (0x3b8)

#define WAIT_BB_OFFSET			(64 << 20)
#define COUNTER_OFFSET			(65 << 20)

struct inter_engine_context {
	int fd;
	const intel_ctx_cfg_t *cfg;
	bool use_relocs;

	struct {
		const intel_ctx_t *ctx;
	} iterations[9];

	struct intel_engine_data engines;

	struct inter_engine_batches {
		void *increment_bb;
		uint32_t increment_bb_len;
		uint32_t increment_bb_handle;

		uint32_t timeline;

		void *read0_ptrs[2];
		void *read1_ptrs[2];
		void *write_ptrs[2];
	} *batches;

	const intel_ctx_t *wait_ctx;
	uint32_t wait_timeline;

	struct drm_i915_gem_exec_object2 engine_counter_object;
};

static void submit_timeline_execbuf(struct inter_engine_context *context,
				    struct drm_i915_gem_execbuffer2 *execbuf,
				    uint32_t run_engine_idx,
				    uint32_t wait_syncobj,
				    uint64_t wait_value,
				    uint32_t signal_syncobj,
				    uint64_t signal_value)
{
	uint64_t values[2] = { };
	struct drm_i915_gem_exec_fence fences[2] = { };
	struct drm_i915_gem_execbuffer_ext_timeline_fences fence_list = {
		.base = {
			.name = DRM_I915_GEM_EXECBUFFER_EXT_TIMELINE_FENCES,
		},
		.handles_ptr = to_user_pointer(fences),
		.values_ptr = to_user_pointer(values),
	};

	if (wait_syncobj) {
		fences[fence_list.fence_count] = (struct drm_i915_gem_exec_fence) {
			.handle = wait_syncobj,
			.flags = I915_EXEC_FENCE_WAIT,
		};
		values[fence_list.fence_count] = wait_value;
		fence_list.fence_count++;
	}

	if (signal_syncobj) {
		fences[fence_list.fence_count] = (struct drm_i915_gem_exec_fence) {
			.handle = signal_syncobj,
			.flags = I915_EXEC_FENCE_SIGNAL,
		};
		values[fence_list.fence_count] = signal_value;
		fence_list.fence_count++;
	}

	if (wait_syncobj || signal_syncobj) {
		execbuf->flags |= I915_EXEC_USE_EXTENSIONS;
		execbuf->cliprects_ptr = to_user_pointer(&fence_list);
	}

	execbuf->flags |= context->engines.engines[run_engine_idx].flags;

	gem_execbuf(context->fd, execbuf);
}

static unsigned int offset_in_page(void *addr)
{
	return (uintptr_t)addr & 4095;
}

static uint64_t
get_cs_timestamp_frequency(int fd)
{
	int cs_ts_freq = 0;
	drm_i915_getparam_t gp;

	gp.param = I915_PARAM_CS_TIMESTAMP_FREQUENCY;
	gp.value = &cs_ts_freq;
	if (igt_ioctl(fd, DRM_IOCTL_I915_GETPARAM, &gp) == 0)
		return cs_ts_freq;

	igt_skip("Kernel with PARAM_CS_TIMESTAMP_FREQUENCY support required\n");
}

static bool use_set_predicate_result(int i915)
{
	uint16_t devid = intel_get_drm_devid(i915);

	return intel_graphics_ver(devid) >= IP_VER(12, 50);
}

static struct drm_i915_gem_exec_object2
build_wait_bb(int i915,
	      const struct intel_execution_engine2 *engine,
	      uint64_t delay,
	      bool use_relocs,
	      struct drm_i915_gem_relocation_entry *relocs)
{
	const uint64_t timestamp_frequency = get_cs_timestamp_frequency(i915);
	uint64_t wait_value =
		0xffffffffffffffff - (delay * timestamp_frequency) / NSEC_PER_SEC;
	struct drm_i915_gem_exec_object2 obj = {};
	uint32_t mmio_base = gem_engine_mmio_base(i915, engine->name);
	uint32_t *map, *bb;

	igt_debug("%s wait_value=0x%"PRIx64", %x\n", engine->name, wait_value, mmio_base);
	igt_assert(mmio_base);

	obj.handle = gem_create(i915, 4096);
	obj.relocs_ptr = to_user_pointer(memset(relocs, 0, sizeof(*relocs)));
	obj.relocation_count = use_relocs ? 1 : 0;
	obj.offset = WAIT_BB_OFFSET;

	relocs->target_handle = obj.handle;
	relocs->presumed_offset = obj.offset;

	map = gem_mmap__device_coherent(i915, obj.handle, 0, 4096, PROT_WRITE);
	bb = map;

	*bb++ = MI_LOAD_REGISTER_IMM;
	*bb++ = mmio_base + HSW_CS_GPR(0);
	*bb++ = wait_value & 0xffffffff;
	*bb++ = MI_LOAD_REGISTER_IMM;
	*bb++ = mmio_base + HSW_CS_GPR(0) + 4;
	*bb++ = wait_value >> 32;

	*bb++ = MI_LOAD_REGISTER_REG;
	*bb++ = mmio_base + RING_TIMESTAMP;
	*bb++ = mmio_base + HSW_CS_GPR(1);
	*bb++ = MI_LOAD_REGISTER_IMM;
	*bb++ = mmio_base + HSW_CS_GPR(1) + 4;
	*bb++ = 0;

	*bb++ = MI_LOAD_REGISTER_IMM;
	*bb++ = mmio_base + HSW_CS_GPR(2) + 4;
	*bb++ = 0;
	relocs->delta = offset_in_page(bb);
	*bb++ = MI_LOAD_REGISTER_REG;
	*bb++ = mmio_base + RING_TIMESTAMP;
	*bb++ = mmio_base + HSW_CS_GPR(2);

	*bb++ = MI_MATH(4);
	*bb++ = MI_MATH_LOAD(MI_MATH_REG_SRCA, MI_MATH_REG(2));
	*bb++ = MI_MATH_LOAD(MI_MATH_REG_SRCB, MI_MATH_REG(1));
	*bb++ = MI_MATH_SUB;
	*bb++ = MI_MATH_STORE(MI_MATH_REG(3), MI_MATH_REG_ACCU);

	if (use_set_predicate_result(i915)) {
		*bb++ = MI_MATH(4);
		*bb++ = MI_MATH_LOAD(MI_MATH_REG_SRCA, MI_MATH_REG(0));
		*bb++ = MI_MATH_LOAD(MI_MATH_REG_SRCB, MI_MATH_REG(3));
		*bb++ = MI_MATH_ADD;
		*bb++ = MI_MATH_STORE(MI_MATH_REG(4), MI_MATH_REG_CF);

		*bb++ = MI_LOAD_REGISTER_REG;
		*bb++ = mmio_base + HSW_CS_GPR(4);
		*bb++ = mmio_base + MI_SET_PREDICATE_RESULT;
	} else {
		*bb++ = MI_MATH(4);
		*bb++ = MI_MATH_LOAD(MI_MATH_REG_SRCA, MI_MATH_REG(0));
		*bb++ = MI_MATH_LOAD(MI_MATH_REG_SRCB, MI_MATH_REG(3));
		*bb++ = MI_MATH_ADD;
		*bb++ = MI_MATH_STOREINV(MI_MATH_REG(4), MI_MATH_REG_CF);

		*bb++ = MI_LOAD_REGISTER_REG;
		*bb++ = mmio_base + HSW_CS_GPR(4);
		*bb++ = mmio_base + MI_PREDICATE_RESULT_1;
	}

	*bb++ = MI_BATCH_BUFFER_START | MI_BATCH_PREDICATE | 1;
	relocs->offset = offset_in_page(bb);
	*bb++ = obj.offset + relocs->delta;
	*bb++ = obj.offset >> 32;

	if (use_set_predicate_result(i915))
		*bb++ = 1 << 23; // MI_SET_PREDICATE

	*bb++ = MI_BATCH_BUFFER_END;

	munmap(map, 4096);
	return obj;
}

static void wait_engine(int i915,
			struct inter_engine_context *context,
			uint32_t run_engine_idx,
			uint32_t signal_syncobj,
			uint64_t signal_value)
{
	struct drm_i915_gem_relocation_entry reloc;
	struct drm_i915_gem_exec_object2 objects[2] = {
		context->engine_counter_object,
		build_wait_bb(i915,
			      &context->engines.engines[run_engine_idx],
			      20 * 1000 * 1000ull /* 20ms */,
			      context->use_relocs,
			      &reloc),
	};
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(&objects[0]),
		.buffer_count = 2,
		.rsvd1 = context->wait_ctx->id,
		.flags = I915_EXEC_NO_RELOC,
		.batch_len = 4096,
	};

	submit_timeline_execbuf(context, &execbuf, run_engine_idx,
				0, 0,
				signal_syncobj, signal_value);

	gem_close(i915, objects[1].handle);
}

static void build_increment_engine_bb(struct inter_engine_batches *batch,
				      uint32_t mmio_base)
{
	uint32_t *bb = batch->increment_bb = calloc(1, 4096);

	*bb++ = MI_LOAD_REGISTER_MEM | 2;
	*bb++ = mmio_base + HSW_CS_GPR(0);
	batch->read0_ptrs[0] = bb;
	*bb++ = 0;
	*bb++ = 0;
	*bb++ = MI_LOAD_REGISTER_MEM | 2;
	*bb++ = mmio_base + HSW_CS_GPR(0) + 4;
	batch->read0_ptrs[1] = bb;
	*bb++ = 0;
	*bb++ = 0;

	*bb++ = MI_LOAD_REGISTER_MEM | 2;
	*bb++ = mmio_base + HSW_CS_GPR(1);
	batch->read1_ptrs[0] = bb;
	*bb++ = 0;
	*bb++ = 0;
	*bb++ = MI_LOAD_REGISTER_MEM | 2;
	*bb++ = mmio_base + HSW_CS_GPR(1) + 4;
	batch->read1_ptrs[1] = bb;
	*bb++ = 0;
	*bb++ = 0;

	*bb++ = MI_MATH(4);
	*bb++ = MI_MATH_LOAD(MI_MATH_REG_SRCA, MI_MATH_REG(0));
	*bb++ = MI_MATH_LOAD(MI_MATH_REG_SRCB, MI_MATH_REG(1));
	*bb++ = MI_MATH_ADD;
	*bb++ = MI_MATH_STORE(MI_MATH_REG(0), MI_MATH_REG_ACCU);

	*bb++ = MI_STORE_REGISTER_MEM | 2;
	*bb++ = mmio_base + HSW_CS_GPR(0);
	batch->write_ptrs[0] = bb;
	*bb++ = 0;
	*bb++ = 0;
	*bb++ = MI_STORE_REGISTER_MEM | 2;
	*bb++ = mmio_base + HSW_CS_GPR(0) + 4;
	batch->write_ptrs[1] = bb;
	*bb++ = 0;
	*bb++ = 0;

	*bb++ = MI_BATCH_BUFFER_END;

	batch->increment_bb_len = ALIGN((void *) bb - batch->increment_bb, 8);
}

static void increment_engine(struct inter_engine_context *context,
			     const intel_ctx_t *ctx,
			     int iteration,
			     uint32_t read0_engine_idx,
			     uint32_t read1_engine_idx,
			     uint32_t write_engine_idx,
			     uint32_t wait_syncobj,
			     uint64_t wait_value,
			     uint32_t signal_syncobj,
			     uint64_t signal_value)
{
	struct inter_engine_batches *batch = &context->batches[write_engine_idx];
	struct drm_i915_gem_relocation_entry relocs[3 * 2];
	struct drm_i915_gem_exec_object2 objects[2] = {
		context->engine_counter_object,
		{
			.handle = batch->increment_bb_handle,
			.relocs_ptr = to_user_pointer(relocs),
			.relocation_count = context->use_relocs ?
						ARRAY_SIZE(relocs) : 0,
		},
	};
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(&objects[0]),
		.buffer_count = ARRAY_SIZE(objects),
		.flags = I915_EXEC_HANDLE_LUT,
		.rsvd1 = ctx->id,
		.batch_len = batch->increment_bb_len,
	};

	memset(relocs, 0, sizeof(relocs));

	/* MI_LOAD_REGISTER_MEM */
	relocs[0].target_handle = 0;
	relocs[0].delta = read0_engine_idx * 8;
	relocs[0].offset = batch->read0_ptrs[0] - batch->increment_bb;
	relocs[0].presumed_offset = -1;
	relocs[1].target_handle = 0;
	relocs[1].delta = read0_engine_idx * 8 + 4;
	relocs[1].offset = batch->read0_ptrs[1] - batch->increment_bb;
	relocs[1].presumed_offset = -1;

	/* MI_LOAD_REGISTER_MEM */
	relocs[2].target_handle = 0;
	relocs[2].delta = read1_engine_idx * 8;
	relocs[2].offset = batch->read1_ptrs[0] - batch->increment_bb;
	relocs[2].presumed_offset = -1;
	relocs[3].target_handle = 0;
	relocs[3].delta = read1_engine_idx * 8 + 4;
	relocs[3].offset = batch->read1_ptrs[1] - batch->increment_bb;
	relocs[3].presumed_offset = -1;

	/* MI_STORE_REGISTER_MEM */
	relocs[4].target_handle = 0;
	relocs[4].delta = write_engine_idx * 8;
	relocs[4].offset = batch->write_ptrs[0] - batch->increment_bb;
	relocs[4].presumed_offset = -1;
	relocs[5].target_handle = 0;
	relocs[5].delta = write_engine_idx * 8 + 4;
	relocs[5].offset = batch->write_ptrs[1] - batch->increment_bb;
	relocs[5].presumed_offset = -1;

	/*
	 * For no-relocs prepare batch for dedicated write engine once
	 * as iteration doesn't matter for it. So we got full pipelining
	 * starting from the second iteration. For relocs we keep its previous
	 * behavior where kernel has to change offsets within bb for each round.
	 */
	if (!iteration && !context->use_relocs) {
		uint64_t counter_offset;
		uint32_t *bb;

		counter_offset = context->engine_counter_object.offset;
		bb = (uint32_t *) batch->increment_bb;

		for (int i = 0; i < ARRAY_SIZE(relocs); i++) {
			bb[relocs[i].offset / sizeof(uint32_t)] =
					counter_offset + relocs[i].delta;
			bb[relocs[i].offset / sizeof(uint32_t) + 1] =
					(counter_offset + relocs[i].delta) >> 32;
		}
		gem_write(context->fd, batch->increment_bb_handle, 0,
			  batch->increment_bb, batch->increment_bb_len);
	}

	submit_timeline_execbuf(context, &execbuf, write_engine_idx,
				wait_syncobj, wait_value,
				signal_syncobj, signal_value);

	context->engine_counter_object = objects[0];
}

static uint64_t fib(uint32_t iters)
{
	uint64_t last_value = 0;
	uint64_t value = 1;
	uint32_t i = 0;

	while (i < iters) {
		uint64_t new_value = value + last_value;

		last_value = value;
		value = new_value;
		i++;
	}

	return last_value;
}

static void setup_timeline_chain_engines(struct inter_engine_context *context, int fd,
					 const intel_ctx_cfg_t *cfg)
{
	memset(context, 0, sizeof(*context));

	context->fd = fd;
	context->cfg = cfg;
	context->engines = intel_engine_list_for_ctx_cfg(fd, cfg);
	igt_require(context->engines.nengines > 1);
	context->use_relocs = gem_has_relocations(fd);

	context->wait_ctx = intel_ctx_create(fd, cfg);
	context->wait_timeline = syncobj_create(fd, 0);

	context->engine_counter_object.handle = gem_create(fd, 4096);
	context->engine_counter_object.offset = COUNTER_OFFSET;
	if (context->use_relocs)
		context->engine_counter_object.flags |= EXEC_OBJECT_PINNED;

	for (uint32_t i = 0; i < ARRAY_SIZE(context->iterations); i++) {
		context->iterations[i].ctx = intel_ctx_create(fd, context->cfg);

		/* Give a different priority to all contexts. */
		gem_context_set_priority(fd, context->iterations[i].ctx->id,
					 I915_CONTEXT_MAX_USER_PRIORITY - ARRAY_SIZE(context->iterations) + i);
	}

	context->batches = calloc(context->engines.nengines,
				  sizeof(*context->batches));
	for (uint32_t e = 0; e < context->engines.nengines; e++) {
		struct inter_engine_batches *batches = &context->batches[e];

		batches->timeline = syncobj_create(fd, 0);

		build_increment_engine_bb(
			batches,
			gem_engine_mmio_base(fd, context->engines.engines[e].name));
		batches->increment_bb_handle = gem_create(fd, 4096);
		gem_write(fd, batches->increment_bb_handle, 0,
			  batches->increment_bb, batches->increment_bb_len);
	}

	for (uint32_t i = 0; i < 10; i++)
		igt_debug("%u = %"PRIu64"\n", i, fib(i));

	/* Bootstrap the fibonacci sequence */
	{
		uint64_t dword = 1;
		gem_write(fd, context->engine_counter_object.handle,
			  sizeof(dword) * (context->engines.nengines - 1),
			  &dword, sizeof(dword));
	}
}

static void teardown_timeline_chain_engines(struct inter_engine_context *context)
{
	gem_close(context->fd, context->engine_counter_object.handle);

	for (uint32_t i = 0; i < ARRAY_SIZE(context->iterations); i++) {
		intel_ctx_destroy(context->fd, context->iterations[i].ctx);
	}

	intel_ctx_destroy(context->fd, context->wait_ctx);
	syncobj_destroy(context->fd, context->wait_timeline);

	for (uint32_t e = 0; e < context->engines.nengines; e++) {
		struct inter_engine_batches *batches = &context->batches[e];

		syncobj_destroy(context->fd, batches->timeline);
		gem_close(context->fd, batches->increment_bb_handle);
		free(batches->increment_bb);
	}
	free(context->batches);
}

static const char *test_syncobj_timeline_chain_engines_desc =
	"Engine chaining tests to verify ordering of timeline syncobjs through execbuf.";
static void test_syncobj_timeline_chain_engines(int fd, const intel_ctx_cfg_t *cfg)
{
	struct inter_engine_context ctx;
	uint64_t *counter_output;

	setup_timeline_chain_engines(&ctx, fd, cfg);

	/*
	 * Delay all the other operations by making them depend on an
	 * active wait on the RCS.
	 */
	wait_engine(fd, &ctx, 0, ctx.wait_timeline, 1);

	for (uint32_t iter = 0; iter < ARRAY_SIZE(ctx.iterations); iter++) {
		for (uint32_t engine = 0; engine < ctx.engines.nengines; engine++) {
			uint32_t prev_prev_engine =
				(ctx.engines.nengines + engine - 2) % ctx.engines.nengines;
			uint32_t prev_engine =
				(ctx.engines.nengines + engine - 1) % ctx.engines.nengines;
			/*
			 * Pick up the wait engine semaphore for the
			 * first increment, then pick up the previous
			 * engine's timeline.
			 */
			uint32_t wait_syncobj =
				iter == 0 && engine == 0 ?
				ctx.wait_timeline : ctx.batches[prev_engine].timeline;
			uint32_t wait_value =
				iter == 0 && engine == 0 ?
				1 : (engine == 0 ? iter : (iter + 1));

			increment_engine(&ctx, ctx.iterations[iter].ctx, iter,
					 prev_prev_engine /* read0 engine */,
					 prev_engine /* read1 engine */,
					 engine /* write engine */,
					 wait_syncobj, wait_value,
					 ctx.batches[engine].timeline, iter + 1);
		}
	}

	gem_sync(fd, ctx.engine_counter_object.handle);

	counter_output = gem_mmap__device_coherent(fd, ctx.engine_counter_object.handle, 0, 4096, PROT_READ);

	for (uint32_t i = 0; i < ctx.engines.nengines; i++)
		igt_debug("engine %i (%s)\t= %016"PRIx64"\n", i,
			  ctx.engines.engines[i].name, counter_output[i]);

	/*
	 * Verify that we get the fibonacci number expected (we start
	 * at the sequence on the second number : 1).
	 */
	igt_assert_eq(counter_output[ctx.engines.nengines - 1],
		      fib(ARRAY_SIZE(ctx.iterations) * ctx.engines.nengines + 1));

	munmap(counter_output, 4096);

	teardown_timeline_chain_engines(&ctx);
}

static const char *test_syncobj_stationary_timeline_chain_engines_desc =
	"Engine chaining tests to verify ordering of timeline syncobj with stationary timeline"
	" points.";
static void test_syncobj_stationary_timeline_chain_engines(int fd, const intel_ctx_cfg_t *cfg)
{
	struct inter_engine_context ctx;
	uint64_t *counter_output;

	setup_timeline_chain_engines(&ctx, fd, cfg);

	/*
	 * Delay all the other operations by making them depend on an
	 * active wait on the RCS.
	 */
	wait_engine(fd, &ctx, 0, ctx.wait_timeline, 1);

	for (uint32_t iter = 0; iter < ARRAY_SIZE(ctx.iterations); iter++) {
		for (uint32_t engine = 0; engine < ctx.engines.nengines; engine++) {
			uint32_t prev_prev_engine =
				(ctx.engines.nengines + engine - 2) % ctx.engines.nengines;
			uint32_t prev_engine =
				(ctx.engines.nengines + engine - 1) % ctx.engines.nengines;
			/*
			 * Pick up the wait engine semaphore for the
			 * first increment, then pick up the previous
			 * engine's timeline.
			 */
			uint32_t wait_syncobj =
				iter == 0 && engine == 0 ?
				ctx.wait_timeline : ctx.batches[prev_engine].timeline;
			/*
			 * Always signal the value 10. Because the
			 * signal operations are submitted in order,
			 * we should always pickup the right
			 * dma-fence.
			 */
			uint32_t wait_value =
				iter == 0 && engine == 0 ?
				1 : 10;

			increment_engine(&ctx, ctx.iterations[iter].ctx, iter,
					 prev_prev_engine /* read0 engine */,
					 prev_engine /* read1 engine */,
					 engine /* write engine */,
					 wait_syncobj, wait_value,
					 ctx.batches[engine].timeline, 10);
		}
	}

	gem_sync(fd, ctx.engine_counter_object.handle);

	counter_output = gem_mmap__device_coherent(fd, ctx.engine_counter_object.handle, 0, 4096, PROT_READ);

	for (uint32_t i = 0; i < ctx.engines.nengines; i++)
		igt_debug("engine %i (%s)\t= %016"PRIx64"\n", i,
			  ctx.engines.engines[i].name, counter_output[i]);
	igt_assert_eq(counter_output[ctx.engines.nengines - 1],
		      fib(ARRAY_SIZE(ctx.iterations) * ctx.engines.nengines + 1));

	munmap(counter_output, 4096);

	teardown_timeline_chain_engines(&ctx);
}

static const char *test_syncobj_backward_timeline_chain_engines_desc =
	"Engine chaining tests to verify ordering of timeline syncobjs with backward timeline"
	" points.";
static void test_syncobj_backward_timeline_chain_engines(int fd, const intel_ctx_cfg_t *cfg)
{
	struct inter_engine_context ctx;
	uint64_t *counter_output;

	setup_timeline_chain_engines(&ctx, fd, cfg);

	/*
	 * Delay all the other operations by making them depend on an
	 * active wait on the RCS.
	 */
	wait_engine(fd, &ctx, 0, ctx.wait_timeline, 1);

	for (uint32_t iter = 0; iter < ARRAY_SIZE(ctx.iterations); iter++) {
		for (uint32_t engine = 0; engine < ctx.engines.nengines; engine++) {
			uint32_t prev_prev_engine =
				(ctx.engines.nengines + engine - 2) % ctx.engines.nengines;
			uint32_t prev_engine =
				(ctx.engines.nengines + engine - 1) % ctx.engines.nengines;
			/*
			 * Pick up the wait engine semaphore for the
			 * first increment, then pick up the previous
			 * engine's timeline.
			 */
			uint32_t wait_syncobj =
				iter == 0 && engine == 0 ?
				ctx.wait_timeline : ctx.batches[prev_engine].timeline;
			/*
			 * Always signal the value 10. Because the
			 * signal operations are submitted in order,
			 * we should always pickup the right
			 * dma-fence.
			 */
			uint32_t wait_value =
				iter == 0 && engine == 0 ?
				1 : 1;

			increment_engine(&ctx, ctx.iterations[iter].ctx, iter,
					 prev_prev_engine /* read0 engine */,
					 prev_engine /* read1 engine */,
					 engine /* write engine */,
					 wait_syncobj, wait_value,
					 ctx.batches[engine].timeline, ARRAY_SIZE(ctx.iterations) - iter);
		}
	}

	gem_sync(fd, ctx.engine_counter_object.handle);

	counter_output = gem_mmap__device_coherent(fd, ctx.engine_counter_object.handle, 0, 4096, PROT_READ);

	for (uint32_t i = 0; i < ctx.engines.nengines; i++)
		igt_debug("engine %i (%s)\t= %016"PRIx64"\n", i,
			  ctx.engines.engines[i].name, counter_output[i]);
	igt_assert_eq(counter_output[ctx.engines.nengines - 1],
		      fib(ARRAY_SIZE(ctx.iterations) * ctx.engines.nengines + 1));

	munmap(counter_output, 4096);

	teardown_timeline_chain_engines(&ctx);
}

igt_main
{
	const struct intel_execution_engine2 *e;
	const intel_ctx_t *ctx;
	int i915 = -1;

	igt_fixture {
		i915 = drm_open_driver(DRIVER_INTEL);
		igt_require_gem(i915);
		igt_require(gem_has_exec_fence(i915));
		gem_require_mmap_device_coherent(i915);
		ctx = intel_ctx_create_all_physical(i915);

		gem_submission_print_method(i915);
	}

	igt_subtest_group {
		igt_hang_t hang;

		igt_fixture {
			igt_fork_hang_detector(i915);
		}

		igt_describe("Basic check for composite fence on all busy engines.");
		igt_subtest("basic-busy-all")
			test_fence_busy_all(i915, ctx, 0);

		igt_describe("Basic check for composite fence with additional wait on all busy"
			     " engines.");
		igt_subtest("basic-wait-all")
			test_fence_busy_all(i915, ctx, WAIT);

		igt_fixture {
			igt_stop_hang_detector();
			hang = igt_allow_hang(i915, ctx->id, 0);
		}

		igt_describe("Check for composite fence on all busy engines with a pending gpu"
			     " hang.");
		igt_subtest("busy-hang-all")
			test_fence_busy_all(i915, ctx, HANG);

		igt_describe("Check for composite fence with additional wait on all busy engines"
			     " and with a pending gpu hang.");
		igt_subtest("wait-hang-all")
			test_fence_busy_all(i915, ctx, WAIT | HANG);

		igt_fixture {
			igt_disallow_hang(i915, hang);
		}
	}

	igt_subtest_group {
		for_each_ctx_engine(i915, ctx, e) {
			igt_fixture {
				igt_require(gem_class_can_store_dword(i915, e->class));
			}
		}
		igt_subtest_group {
			igt_fixture {
				igt_fork_hang_detector(i915);
				intel_allocator_multiprocess_start();
			}

			igt_describe("Basic check for explicit fence on each busy engine.");
			igt_subtest_with_dynamic("basic-busy") {
				for_each_ctx_engine(i915, ctx, e) {
					igt_dynamic_f("%s", e->name)
						test_fence_busy(i915, ctx, e, 0);
				}
			}

			igt_describe("Basic check for explicit fence with additinal wait time on"
				     " each busy engine.");
			igt_subtest_with_dynamic("basic-wait") {
				for_each_ctx_engine(i915, ctx, e) {
					igt_dynamic_f("%s", e->name)
						test_fence_busy(i915, ctx, e, WAIT);
				}
			}

			igt_describe("Basic check for explicit fence with async wait on each"
				     " engine.");
			igt_subtest_with_dynamic("basic-await") {
				for_each_ctx_engine(i915, ctx, e) {
					igt_dynamic_f("%s", e->name)
						test_fence_await(i915, ctx, e, 0);
				}
			}

			igt_describe("Check for explicit fence with non-blocking wait on each"
				     " engine.");
			igt_subtest_with_dynamic("nb-await") {
				for_each_ctx_engine(i915, ctx, e) {
					igt_dynamic_f("%s", e->name)
						test_fence_await(i915, ctx, e,
								 NONBLOCK);
				}
			}
			igt_describe("Check in-fence is not overwritten with out-fence on each"
				     " engine.");
			igt_subtest_with_dynamic("keep-in-fence") {
				for_each_ctx_engine(i915, ctx, e) {
					igt_dynamic_f("%s", e->name)
						test_keep_in_fence(i915, ctx, e);
				}
			}
			igt_describe("Check for EXEC_FENCE_SUBMIT in parallel execution scenarios"
				     " on each engine.");
			igt_subtest_with_dynamic("parallel") {
				igt_require(has_submit_fence(i915));
				for_each_ctx_engine(i915, ctx, e) {
					igt_dynamic_f("%s", e->name) {
						igt_until_timeout(2)
							test_parallel(i915, ctx, e);
					}
				}
			}

			igt_describe("Check blocking bonded fences on each engine.");
			igt_subtest_with_dynamic("concurrent") {
				igt_require(has_submit_fence(i915));
				igt_require(gem_scheduler_has_semaphores(i915));
				igt_require(gem_scheduler_has_preemption(i915));
				for_each_ctx_engine(i915, ctx, e) {
					igt_dynamic_f("%s", e->name)
						test_concurrent(i915, ctx, e);
				}
			}

			igt_describe("Check timeslicing on submit-fence.");
			igt_subtest_with_dynamic("submit") {
				igt_require(gem_scheduler_has_semaphores(i915));
				igt_require(gem_scheduler_has_preemption(i915));
				igt_require(intel_gen(intel_get_drm_devid(i915)) >= 8);

				for_each_ctx_engine(i915, ctx, e) {
					igt_dynamic_f("%s", e->name)
						test_submit_fence(i915, ctx, e);
				}
			}

			igt_describe("Check timeslicing on submit-fence by submitting coupled"
				     " batches.");
			igt_subtest_with_dynamic("submit3") {
				igt_require(gem_scheduler_has_semaphores(i915));
				igt_require(gem_scheduler_has_preemption(i915));
				igt_require(intel_gen(intel_get_drm_devid(i915)) >= 8);

				for_each_ctx_engine(i915, ctx, e) {
					igt_dynamic_f("%s", e->name)
						test_submitN(i915, ctx, e, 3);
				}
			}

			igt_describe("Check timeslicing on submit-fence by submitting more"
				     " coupled batches than can possibly fit into the ELSP.");
			igt_subtest_with_dynamic("submit67") {
				igt_require(gem_scheduler_has_semaphores(i915));
				igt_require(gem_scheduler_has_preemption(i915));
				igt_require(intel_gen(intel_get_drm_devid(i915)) >= 8);

				for_each_ctx_engine(i915, ctx, e) {
					igt_dynamic_f("%s", e->name)
						test_submitN(i915, ctx, e, 67);
				}
			}

			igt_describe("Check launching a chain of spinners across all engines using"
				     " submit-fence.");
			igt_subtest("submit-chain") {
				igt_require(has_submit_fence(i915));
				test_submit_chain(i915, ctx);
			}

			igt_fixture {
				intel_allocator_multiprocess_stop();
				igt_stop_hang_detector();
			}
		}

		igt_subtest_group {
			igt_hang_t hang;

			igt_fixture {
				hang = igt_allow_hang(i915, ctx->id, 0);
				intel_allocator_multiprocess_start();
			}

			igt_describe("Check for explicit fence on each busy engine with a pending"
				     " gpu hang.");
			igt_subtest_with_dynamic("busy-hang") {
				for_each_ctx_engine(i915, ctx, e) {
					igt_dynamic_f("%s", e->name)
						test_fence_busy(i915, ctx, e, HANG);
				}
			}
			igt_describe("Check for explicit fence with additional wait time on each"
				     " busy engine with a pending gpu hang.");
			igt_subtest_with_dynamic("wait-hang") {
				for_each_ctx_engine(i915, ctx, e) {
					igt_dynamic_f("%s", e->name)
						test_fence_busy(i915, ctx, e, HANG | WAIT);
				}
			}
			igt_describe("Check for explicit fence with async wait on each engine with"
				     " a pending gpu hang.");
			igt_subtest_with_dynamic("await-hang") {
				for_each_ctx_engine(i915, ctx, e) {
					igt_dynamic_f("%s", e->name)
						test_fence_await(i915, ctx, e, HANG);
				}
			}
			igt_describe("Check for explicit fence with non-blocking async wait on each"
				     " engine with a pending gpu hang.");
			igt_subtest_with_dynamic("nb-await-hang") {
				for_each_ctx_engine(i915, ctx, e) {
					igt_dynamic_f("%s", e->name)
						test_fence_await(i915, ctx, e, NONBLOCK | HANG);
				}
			}
			igt_fixture {
				intel_allocator_multiprocess_stop();
				igt_disallow_hang(i915, hang);
			}
		}
	}

	igt_subtest_group {
		long ring_size = 0;

		igt_fixture {
			ring_size = gem_submission_measure(i915, &ctx->cfg,
							   ALL_ENGINES);
			igt_info("Ring size: %ld batches\n", ring_size);
			igt_require(ring_size);

			gem_require_contexts(i915);
		}

		igt_describe("Verifies accumulation of long history of fences.");
		igt_subtest("long-history")
			test_long_history(i915, ctx, ring_size, 0);

		igt_describe("Verifies long history of fences are expired.");
		igt_subtest("expired-history")
			test_long_history(i915, ctx, ring_size, EXPIRED);
	}

	igt_subtest_group { /* syncobj */
		igt_fixture {
			igt_require(exec_has_fence_array(i915));
			igt_assert(has_syncobj(i915));
			igt_fork_hang_detector(i915);
			intel_allocator_multiprocess_start();
		}

		igt_describe("Verifies invalid fence-array pointers are rejected.");
		igt_subtest("invalid-fence-array")
			test_invalid_fence_array(i915);

		igt_describe("Verifies that a syncobj passed into execbuf but with no signal/wait"
			     " flag is left untouched.");
		igt_subtest("syncobj-unused-fence")
			test_syncobj_unused_fence(i915);

		igt_describe("Verifies that submitting an execbuf with a wait on a syncobj that"
			     " doesn't exists is rejected.");
		igt_subtest("syncobj-invalid-wait")
			test_syncobj_invalid_wait(i915);

		igt_describe("Verifies that invalid fence flags in fence-array are rejected.");
		igt_subtest("syncobj-invalid-flags")
			test_syncobj_invalid_flags(i915);

		igt_describe("Verifies proper signaling of a fence-array syncobj through execbuf.");
		igt_subtest("syncobj-signal")
			test_syncobj_signal(i915);

		igt_describe("Verifies that waiting on a timeline syncobj point between engines"
			     " works.");
		igt_subtest("syncobj-wait")
			test_syncobj_wait(i915, ctx);

		igt_describe("Verify exporting of fence-array syncobj signaled by i915.");
		igt_subtest("syncobj-export")
			test_syncobj_export(i915);

		igt_describe("Verifies that waiting & signaling a same fence-array syncobj within"
			     " the same execbuf works.");
		igt_subtest("syncobj-repeat")
			test_syncobj_repeat(i915);

		igt_describe("Verifies creating of a syncobj from explicit fence.");
		igt_subtest("syncobj-import")
			test_syncobj_import(i915);

		igt_describe("Verifies two clients racing for syncobj using channel.");
		igt_subtest("syncobj-channel")
			test_syncobj_channel(i915);

		igt_fixture {
			intel_allocator_multiprocess_stop();
			igt_stop_hang_detector();
		}
	}

	igt_subtest_group { /* syncobj timeline */
		igt_fixture {
			igt_require(exec_has_timeline_fences(i915));
			igt_require(has_syncobj_timeline(i915));
			igt_fork_hang_detector(i915);
		}

		igt_describe(test_invalid_timeline_fence_array_desc);
		igt_subtest("invalid-timeline-fence-array")
			test_invalid_timeline_fence_array(i915);

		igt_describe(test_syncobj_timeline_unused_fence_desc);
		igt_subtest("syncobj-timeline-unused-fence")
			test_syncobj_timeline_unused_fence(i915);

		igt_describe(test_syncobj_timeline_invalid_wait_desc);
		igt_subtest("syncobj-timeline-invalid-wait")
			test_syncobj_timeline_invalid_wait(i915);

		igt_describe(test_syncobj_timeline_invalid_flags_desc);
		igt_subtest("syncobj-timeline-invalid-flags")
			test_syncobj_timeline_invalid_flags(i915);

		igt_describe(test_syncobj_timeline_signal_desc);
		igt_subtest("syncobj-timeline-signal")
			test_syncobj_timeline_signal(i915);

		igt_describe(test_syncobj_timeline_wait_desc);
		igt_subtest("syncobj-timeline-wait")
			test_syncobj_timeline_wait(i915, ctx);

		igt_describe(test_syncobj_timeline_export_desc);
		igt_subtest("syncobj-timeline-export")
			test_syncobj_timeline_export(i915);

		igt_describe(test_syncobj_timeline_repeat_desc);
		igt_subtest("syncobj-timeline-repeat")
			test_syncobj_timeline_repeat(i915);

		igt_describe(test_syncobj_timeline_multiple_ext_nodes_desc);
		igt_subtest("syncobj-timeline-multiple-ext-nodes")
			test_syncobj_timeline_multiple_ext_nodes(i915);

		igt_subtest_group { /* syncobj timeline engine chaining */
			igt_fixture {
				/*
				 * We need support for MI_ALU on all
				 * engines which seems to be there
				 * only on Gen8+
				 */
				igt_require(intel_gen(intel_get_drm_devid(i915)) >= 8);
			}

			igt_describe(test_syncobj_timeline_chain_engines_desc);
			igt_subtest("syncobj-timeline-chain-engines")
				test_syncobj_timeline_chain_engines(i915, &ctx->cfg);

			igt_describe(test_syncobj_stationary_timeline_chain_engines_desc);
			igt_subtest("syncobj-stationary-timeline-chain-engines")
				test_syncobj_stationary_timeline_chain_engines(i915, &ctx->cfg);

			igt_describe(test_syncobj_backward_timeline_chain_engines_desc);
			igt_subtest("syncobj-backward-timeline-chain-engines")
				test_syncobj_backward_timeline_chain_engines(i915, &ctx->cfg);
		}

		igt_fixture {
			igt_stop_hang_detector();
		}
	}

	igt_fixture {
		close(i915);
	}
}
