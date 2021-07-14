/*
 * Copyright © 2009 Intel Corporation
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
 *    Eric Anholt <eric@anholt.net>
 *
 */

/** @file gem_ringfill.c
 *
 * This is a test of doing many tiny batchbuffer operations, in the hope of
 * catching failure to manage the ring properly near full.
 */

#include "i915/gem.h"
#include "i915/gem_create.h"
#include "i915/gem_ring.h"
#include "igt.h"
#include "igt_device.h"
#include "igt_gt.h"
#include "igt_vgem.h"

#include <signal.h>
#include <sys/ioctl.h>

#define INTERRUPTIBLE 0x1
#define HANG 0x2
#define CHILD 0x8
#define FORKED 0x8
#define BOMB 0x10
#define SUSPEND 0x20
#define HIBERNATE 0x40
#define NEWFD 0x80

static unsigned int ring_size;

static void check_bo(int fd, uint32_t handle)
{
	uint32_t *map;
	int i;

	igt_debug("Verifying result\n");
	map = gem_mmap__cpu(fd, handle, 0, 4096, PROT_READ);
	gem_set_domain(fd, handle, I915_GEM_DOMAIN_CPU, 0);
	for (i = 0; i < 1024; i++)
		igt_assert_eq(map[i], i);
	munmap(map, 4096);
}

static void fill_ring(int fd,
		      struct drm_i915_gem_execbuffer2 *execbuf,
		      unsigned flags, unsigned timeout)
{
	/* The ring we've been using is 128k, and each rendering op
	 * will use at least 8 dwords:
	 *
	 * BATCH_START
	 * BATCH_START offset
	 * MI_FLUSH
	 * STORE_DATA_INDEX
	 * STORE_DATA_INDEX offset
	 * STORE_DATA_INDEX value
	 * MI_USER_INTERRUPT
	 * (padding)
	 *
	 * So iterate just a little more than that -- if we don't fill the ring
	 * doing this, we aren't likely to with this test.
	 */
	igt_debug("Executing execbuf %d times\n", 128*1024/(8*4));
	igt_until_timeout(timeout) {
		igt_while_interruptible(flags & INTERRUPTIBLE) {
			for (typeof(ring_size) i = 0; i < ring_size; i++)
				gem_execbuf(fd, execbuf);
		}
	}
}

static void setup_execbuf(int fd, const intel_ctx_t *ctx,
			  struct drm_i915_gem_execbuffer2 *execbuf,
			  struct drm_i915_gem_exec_object2 *obj,
			  struct drm_i915_gem_relocation_entry *reloc,
			  unsigned int ring)
{
	const unsigned int gen = intel_gen(intel_get_drm_devid(fd));
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	uint32_t *batch, *b;
	int i;

	memset(execbuf, 0, sizeof(*execbuf));
	memset(obj, 0, 2*sizeof(*obj));
	memset(reloc, 0, 1024*sizeof(*reloc));

	execbuf->buffers_ptr = to_user_pointer(obj);
	execbuf->flags = ring | (1 << 11) | (1 << 12);

	if (gen > 3 && gen < 6)
		execbuf->flags |= I915_EXEC_SECURE;

	execbuf->rsvd1 = ctx->id;

	obj[0].handle = gem_create(fd, 4096);
	gem_write(fd, obj[0].handle, 0, &bbe, sizeof(bbe));
	execbuf->buffer_count = 1;
	gem_execbuf(fd, execbuf);

	obj[0].flags |= EXEC_OBJECT_WRITE;
	obj[1].handle = gem_create(fd, 1024*16 + 4096);

	obj[1].relocs_ptr = to_user_pointer(reloc);
	obj[1].relocation_count = 1024;

	batch = gem_mmap__cpu(fd, obj[1].handle, 0, 16*1024 + 4096,
			      PROT_WRITE | PROT_READ);
	gem_set_domain(fd, obj[1].handle,
		       I915_GEM_DOMAIN_CPU, I915_GEM_DOMAIN_CPU);

	b = batch;
	for (i = 0; i < 1024; i++) {
		uint64_t offset;

		reloc[i].presumed_offset = obj[0].offset;
		reloc[i].offset = (b - batch + 1) * sizeof(*batch);
		reloc[i].delta = i * sizeof(uint32_t);
		reloc[i].read_domains = I915_GEM_DOMAIN_INSTRUCTION;
		reloc[i].write_domain = I915_GEM_DOMAIN_INSTRUCTION;

		offset = obj[0].offset + reloc[i].delta;
		*b++ = MI_STORE_DWORD_IMM;
		if (gen >= 8) {
			*b++ = offset;
			*b++ = offset >> 32;
		} else if (gen >= 4) {
			if (gen < 6)
				b[-1] |= 1 << 22;
			*b++ = 0;
			*b++ = offset;
			reloc[i].offset += sizeof(*batch);
		} else {
			b[-1] |= 1 << 22;
			b[-1] -= 1;
			*b++ = offset;
		}
		*b++ = i;
	}
	*b++ = MI_BATCH_BUFFER_END;
	munmap(batch, 16*1024+4096);

	execbuf->buffer_count = 2;
	gem_execbuf(fd, execbuf);

	check_bo(fd, obj[0].handle);
}

static void run_test(int fd, const intel_ctx_t *ctx, unsigned ring,
		     unsigned flags, unsigned timeout)
{
	struct drm_i915_gem_exec_object2 obj[2];
	struct drm_i915_gem_relocation_entry reloc[1024];
	struct drm_i915_gem_execbuffer2 execbuf;
	igt_hang_t hang;

	if (flags & (SUSPEND | HIBERNATE)) {
		run_test(fd, ctx, ring, 0, 0);
		gem_quiescent_gpu(fd);
	}

	setup_execbuf(fd, ctx, &execbuf, obj, reloc, ring);

	memset(&hang, 0, sizeof(hang));
	if (flags & HANG)
		hang = igt_hang_ctx(fd, ctx->id, ring & ~(3<<13), 0);

	if (flags & (CHILD | FORKED | BOMB)) {
		int nchild;

		if (flags & FORKED)
			nchild = sysconf(_SC_NPROCESSORS_ONLN);
		else if (flags & BOMB)
			nchild = 8*sysconf(_SC_NPROCESSORS_ONLN);
		else
			nchild = 1;

		igt_debug("Forking %d children\n", nchild);
		igt_fork(child, nchild) {
			const intel_ctx_t *child_ctx = NULL;
			if (flags & NEWFD) {
				fd = gem_reopen_driver(fd);
				child_ctx = intel_ctx_create(fd, &ctx->cfg);

				setup_execbuf(fd, child_ctx, &execbuf, obj, reloc, ring);
			}
			fill_ring(fd, &execbuf, flags, timeout);
			if (child_ctx)
				intel_ctx_destroy(fd, child_ctx);
		}

		if (flags & SUSPEND)
			igt_system_suspend_autoresume(SUSPEND_STATE_MEM,
						      SUSPEND_TEST_NONE);

		if (flags & HIBERNATE)
			igt_system_suspend_autoresume(SUSPEND_STATE_DISK,
						      SUSPEND_TEST_NONE);

		if (flags & NEWFD)
			fill_ring(fd, &execbuf, flags, timeout);

		igt_waitchildren();
	} else
		fill_ring(fd, &execbuf, flags, timeout);

	if (flags & HANG)
		igt_post_hang_ring(fd, hang);
	else
		check_bo(fd, obj[0].handle);

	gem_close(fd, obj[1].handle);
	gem_close(fd, obj[0].handle);

	if (flags & (SUSPEND | HIBERNATE)) {
		gem_quiescent_gpu(fd);
		run_test(fd, ctx, ring, 0, 0);
	}
}

static uint32_t batch_create(int i915)
{
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	uint32_t handle = gem_create(i915, 4096);

	gem_write(i915, handle, 0, &bbe, sizeof(bbe));
	return handle;
}

static bool has_lut_handle(int i915)
{
	struct drm_i915_gem_exec_object2 obj = {
		.handle = batch_create(i915),
	};
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffer_count = 1,
		.buffers_ptr = to_user_pointer(&obj),
		.flags = (1 << 11) | (1 << 12),
	};
	bool result;

	/* Check for v3.10 with LUT_HANDLE AND NORELOC */
	result = __gem_execbuf(i915, &execbuf) == 0;
	gem_close(i915, obj.handle);

	return result;
}

igt_main
{
	const struct {
		const char *suffix;
		unsigned flags;
		unsigned timeout;
	} modes[] = {
		{ "basic", 0, 0 },
		{ "interruptible", INTERRUPTIBLE, 1 },
		{ "hang", HANG, 10 },
		{ "child", CHILD, 0 },
		{ "forked", FORKED, 0 },
		{ "fd", FORKED | NEWFD, 0 },
		{ "bomb", BOMB | NEWFD | INTERRUPTIBLE, 150 },
		{ "S3", BOMB | SUSPEND, 30 },
		{ "S4", BOMB | HIBERNATE, 30 },
		{ NULL }
	}, *m;
	bool master = false;
	const intel_ctx_t *ctx;
	int fd = -1;

	igt_fixture {
		int gen;

		fd = drm_open_driver(DRIVER_INTEL);

		igt_require_gem(fd);
		igt_require(has_lut_handle(fd));

		gen = intel_gen(intel_get_drm_devid(fd));
		if (gen > 3 && gen < 6) { /* ctg and ilk need secure batches */
			igt_device_set_master(fd);
			master = true;
		}

		ring_size = gem_measure_ring_inflight(fd, ALL_ENGINES, 0);
		igt_info("Ring size: %d batches\n", ring_size);
		igt_require(ring_size);

		ctx = intel_ctx_create_all_physical(fd);
	}

	/* Legacy path for selecting "rings". */
	for (m = modes; m->suffix; m++) {
		igt_subtest_with_dynamic_f("legacy-%s", m->suffix) {
			igt_skip_on(m->flags & NEWFD && master);

			for_each_ring(e, fd) {
				igt_dynamic_f("%s", e->name) {
					igt_require(gem_can_store_dword(fd, eb_ring(e)));
					run_test(fd, intel_ctx_0(fd),
						 eb_ring(e),
						 m->flags,
						 m->timeout);
					gem_quiescent_gpu(fd);
				}
			}
		}
	}

	/* New interface for selecting "engines". */
	for (m = modes; m->suffix; m++) {
		igt_subtest_with_dynamic_f("engines-%s", m->suffix) {
			const struct intel_execution_engine2 *e;

			igt_skip_on(m->flags & NEWFD && master);
			for_each_ctx_engine(fd, ctx, e) {
				if (!gem_class_can_store_dword(fd, e->class))
					continue;

				igt_dynamic_f("%s", e->name) {
					run_test(fd, ctx,
						 e->flags,
						 m->flags,
						 m->timeout);
					gem_quiescent_gpu(fd);
				}
			}
		}
	}

	igt_subtest("basic-all") {
		const struct intel_execution_engine2 *e;

		for_each_ctx_engine(fd, ctx, e) {
			if (!gem_class_can_store_dword(fd, e->class))
				continue;

			igt_fork(child, 1)
				run_test(fd, ctx, e->flags, 0, 1);
		}

		igt_waitchildren();
	}

	igt_fixture {
		intel_ctx_destroy(fd, ctx);
		close(fd);
	}
}
