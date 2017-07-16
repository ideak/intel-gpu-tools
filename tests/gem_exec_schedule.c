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

#include <sys/poll.h>
#include <sys/ioctl.h>

#include "igt.h"
#include "igt_vgem.h"
#include "igt_rand.h"

#define LOCAL_PARAM_HAS_SCHEDULER 41
#define LOCAL_CONTEXT_PARAM_PRIORITY 6

#define LO 0
#define HI 1
#define NOISE 2

#define MAX_PRIO 1023

#define BUSY_QLEN 8

IGT_TEST_DESCRIPTION("Check that we can control the order of execution");

static int __ctx_set_priority(int fd, uint32_t ctx, int prio)
{
	struct local_i915_gem_context_param param;

	memset(&param, 0, sizeof(param));
	param.context = ctx;
	param.size = 0;
	param.param = LOCAL_CONTEXT_PARAM_PRIORITY;
	param.value = prio;

	return __gem_context_set_param(fd, &param);
}

static void ctx_set_priority(int fd, uint32_t ctx, int prio)
{
	igt_assert_eq(__ctx_set_priority(fd, ctx, prio), 0);
}

static void ctx_has_priority(int fd)
{
	igt_require(__ctx_set_priority(fd, 0, MAX_PRIO) == 0);
}

static void store_dword(int fd, uint32_t ctx, unsigned ring,
			uint32_t target, uint32_t offset, uint32_t value,
			uint32_t cork, unsigned write_domain)
{
	const int gen = intel_gen(intel_get_drm_devid(fd));
	struct drm_i915_gem_exec_object2 obj[3];
	struct drm_i915_gem_relocation_entry reloc;
	struct drm_i915_gem_execbuffer2 execbuf;
	uint32_t batch[16];
	int i;

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(obj + !cork);
	execbuf.buffer_count = 2 + !!cork;
	execbuf.flags = ring;
	if (gen < 6)
		execbuf.flags |= I915_EXEC_SECURE;
	execbuf.rsvd1 = ctx;

	memset(obj, 0, sizeof(obj));
	obj[0].handle = cork;
	obj[1].handle = target;
	obj[2].handle = gem_create(fd, 4096);

	memset(&reloc, 0, sizeof(reloc));
	reloc.target_handle = obj[1].handle;
	reloc.presumed_offset = 0;
	reloc.offset = sizeof(uint32_t);
	reloc.delta = offset;
	reloc.read_domains = I915_GEM_DOMAIN_INSTRUCTION;
	reloc.write_domain = write_domain;
	obj[2].relocs_ptr = to_user_pointer(&reloc);
	obj[2].relocation_count = 1;

	i = 0;
	batch[i] = MI_STORE_DWORD_IMM | (gen < 6 ? 1 << 22 : 0);
	if (gen >= 8) {
		batch[++i] = offset;
		batch[++i] = 0;
	} else if (gen >= 4) {
		batch[++i] = 0;
		batch[++i] = offset;
		reloc.offset += sizeof(uint32_t);
	} else {
		batch[i]--;
		batch[++i] = offset;
	}
	batch[++i] = value;
	batch[++i] = MI_BATCH_BUFFER_END;
	gem_write(fd, obj[2].handle, 0, batch, sizeof(batch));
	gem_execbuf(fd, &execbuf);
	gem_close(fd, obj[2].handle);
}

static uint32_t *make_busy(int fd, uint32_t target, unsigned ring)
{
	const int gen = intel_gen(intel_get_drm_devid(fd));
	struct drm_i915_gem_exec_object2 obj[2];
	struct drm_i915_gem_relocation_entry reloc[2];
	struct drm_i915_gem_execbuffer2 execbuf;
	uint32_t *batch;
	int i;

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(obj + !target);
	execbuf.buffer_count = 1 + !!target;

	memset(obj, 0, sizeof(obj));
	obj[0].handle = target;
	obj[1].handle = gem_create(fd, 4096);
	batch = gem_mmap__wc(fd, obj[1].handle, 0, 4096, PROT_WRITE);
	gem_set_domain(fd, obj[1].handle,
			I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);

	obj[1].relocs_ptr = to_user_pointer(reloc);
	obj[1].relocation_count = 1 + !!target;
	memset(reloc, 0, sizeof(reloc));

	reloc[0].target_handle = obj[1].handle; /* recurse */
	reloc[0].presumed_offset = 0;
	reloc[0].offset = sizeof(uint32_t);
	reloc[0].delta = 0;
	reloc[0].read_domains = I915_GEM_DOMAIN_COMMAND;
	reloc[0].write_domain = 0;

	reloc[1].target_handle = target;
	reloc[1].presumed_offset = 0;
	reloc[1].offset = 1024;
	reloc[1].delta = 0;
	reloc[1].read_domains = I915_GEM_DOMAIN_COMMAND;
	reloc[1].write_domain = 0;

	i = 0;
	batch[i] = MI_BATCH_BUFFER_START;
	if (gen >= 8) {
		batch[i] |= 1 << 8 | 1;
		batch[++i] = 0;
		batch[++i] = 0;
	} else if (gen >= 6) {
		batch[i] |= 1 << 8;
		batch[++i] = 0;
	} else {
		batch[i] |= 2 << 6;
		batch[++i] = 0;
		if (gen < 4) {
			batch[i] |= 1;
			reloc[0].delta = 1;
		}
	}
	i++;

	if (ring != -1) {
		execbuf.flags = ring;
		for (int n = 0; n < BUSY_QLEN; n++)
			gem_execbuf(fd, &execbuf);
	} else {
		for_each_engine(fd, ring) {
			if (ring == 0)
				continue;

			execbuf.flags = ring;
			for (int n = 0; n < BUSY_QLEN; n++)
				gem_execbuf(fd, &execbuf);
			igt_assert(execbuf.flags == ring);
		}
	}

	if (target) {
		execbuf.flags = 0;
		reloc[1].write_domain = I915_GEM_DOMAIN_COMMAND;
		gem_execbuf(fd, &execbuf);
	}

	gem_close(fd, obj[1].handle);

	return batch;
}

static void finish_busy(uint32_t *busy)
{
	*busy = MI_BATCH_BUFFER_END;
	munmap(busy, 4096);
}

struct cork {
	int device;
	uint32_t handle;
	uint32_t fence;
};

static void plug(int fd, struct cork *c)
{
	struct vgem_bo bo;
	int dmabuf;

	c->device = drm_open_driver(DRIVER_VGEM);

	bo.width = bo.height = 1;
	bo.bpp = 4;
	vgem_create(c->device, &bo);
	c->fence = vgem_fence_attach(c->device, &bo, VGEM_FENCE_WRITE);

	dmabuf = prime_handle_to_fd(c->device, bo.handle);
	c->handle = prime_fd_to_handle(fd, dmabuf);
	close(dmabuf);
}

static void unplug(struct cork *c)
{
	vgem_fence_signal(c->device, c->fence);
	close(c->device);
}

static void fifo(int fd, unsigned ring)
{
	struct cork cork;
	uint32_t *busy;
	uint32_t scratch;
	uint32_t *ptr;

	scratch = gem_create(fd, 4096);

	busy = make_busy(fd, scratch, ring);
	plug(fd, &cork);

	/* Same priority, same timeline, final result will be the second eb */
	store_dword(fd, 0, ring, scratch, 0, 1, cork.handle, 0);
	store_dword(fd, 0, ring, scratch, 0, 2, cork.handle, 0);

	unplug(&cork); /* only now submit our batches */
	igt_debugfs_dump(fd, "i915_engine_info");
	finish_busy(busy);

	ptr = gem_mmap__gtt(fd, scratch, 4096, PROT_READ);
	gem_set_domain(fd, scratch, /* no write hazard lies! */
			I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);
	gem_close(fd, scratch);

	igt_assert_eq_u32(ptr[0], 2);
	munmap(ptr, 4096);
}

static void reorder(int fd, unsigned ring, unsigned flags)
#define EQUAL 1
{
	struct cork cork;
	uint32_t scratch;
	uint32_t *busy;
	uint32_t *ptr;
	uint32_t ctx[2];

	ctx[LO] = gem_context_create(fd);
	ctx_set_priority(fd, ctx[LO], -MAX_PRIO);

	ctx[HI] = gem_context_create(fd);
	ctx_set_priority(fd, ctx[HI], flags & EQUAL ? -MAX_PRIO : 0);

	scratch = gem_create(fd, 4096);

	busy = make_busy(fd, scratch, ring);
	plug(fd, &cork);

	/* We expect the high priority context to be executed first, and
	 * so the final result will be value from the low priority context.
	 */
	store_dword(fd, ctx[LO], ring, scratch, 0, ctx[LO], cork.handle, 0);
	store_dword(fd, ctx[HI], ring, scratch, 0, ctx[HI], cork.handle, 0);

	unplug(&cork); /* only now submit our batches */
	igt_debugfs_dump(fd, "i915_engine_info");
	finish_busy(busy);

	gem_context_destroy(fd, ctx[LO]);
	gem_context_destroy(fd, ctx[HI]);

	ptr = gem_mmap__gtt(fd, scratch, 4096, PROT_READ);
	gem_set_domain(fd, scratch, /* no write hazard lies! */
			I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);
	gem_close(fd, scratch);

	if (flags & EQUAL) /* equal priority, result will be fifo */
		igt_assert_eq_u32(ptr[0], ctx[HI]);
	else
		igt_assert_eq_u32(ptr[0], ctx[LO]);
	munmap(ptr, 4096);
}

static void promotion(int fd, unsigned ring)
{
	struct cork cork;
	uint32_t result, dep;
	uint32_t *busy;
	uint32_t *ptr;
	uint32_t ctx[3];

	ctx[LO] = gem_context_create(fd);
	ctx_set_priority(fd, ctx[LO], -MAX_PRIO);

	ctx[HI] = gem_context_create(fd);
	ctx_set_priority(fd, ctx[HI], 0);

	ctx[NOISE] = gem_context_create(fd);
	ctx_set_priority(fd, ctx[NOISE], -MAX_PRIO/2);

	result = gem_create(fd, 4096);
	dep = gem_create(fd, 4096);

	busy = make_busy(fd, result, ring);
	plug(fd, &cork);

	/* Expect that HI promotes LO, so the order will be LO, HI, NOISE.
	 *
	 * fifo would be NOISE, LO, HI.
	 * strict priority would be  HI, NOISE, LO
	 */
	store_dword(fd, ctx[NOISE], ring, result, 0, ctx[NOISE], cork.handle, 0);
	store_dword(fd, ctx[LO], ring, result, 0, ctx[LO], cork.handle, 0);

	/* link LO <-> HI via a dependency on another buffer */
	store_dword(fd, ctx[LO], ring, dep, 0, ctx[LO], 0, I915_GEM_DOMAIN_INSTRUCTION);
	store_dword(fd, ctx[HI], ring, dep, 0, ctx[HI], 0, 0);

	store_dword(fd, ctx[HI], ring, result, 0, ctx[HI], 0, 0);

	unplug(&cork); /* only now submit our batches */
	igt_debugfs_dump(fd, "i915_engine_info");
	finish_busy(busy);

	gem_context_destroy(fd, ctx[NOISE]);
	gem_context_destroy(fd, ctx[LO]);
	gem_context_destroy(fd, ctx[HI]);

	ptr = gem_mmap__gtt(fd, dep, 4096, PROT_READ);
	gem_set_domain(fd, dep, /* no write hazard lies! */
			I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);
	gem_close(fd, dep);

	igt_assert_eq_u32(ptr[0], ctx[HI]);
	munmap(ptr, 4096);

	ptr = gem_mmap__gtt(fd, result, 4096, PROT_READ);
	gem_set_domain(fd, result, /* no write hazard lies! */
			I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);
	gem_close(fd, result);

	igt_assert_eq_u32(ptr[0], ctx[NOISE]);
	munmap(ptr, 4096);
}

#define NEW_CTX 0x1
static void preempt(int fd, unsigned ring, unsigned flags)
{
	uint32_t result = gem_create(fd, 4096);
	uint32_t *ptr = gem_mmap__gtt(fd, result, 4096, PROT_READ);
	igt_spin_t *spin[16];
	uint32_t ctx[2];

	ctx[LO] = gem_context_create(fd);
	ctx_set_priority(fd, ctx[LO], -MAX_PRIO);

	ctx[HI] = gem_context_create(fd);
	ctx_set_priority(fd, ctx[HI], MAX_PRIO);

	for (int n = 0; n < 16; n++) {
		if (flags & NEW_CTX) {
			gem_context_destroy(fd, ctx[LO]);
			ctx[LO] = gem_context_create(fd);
			ctx_set_priority(fd, ctx[LO], -MAX_PRIO);
		}
		spin[n] = __igt_spin_batch_new(fd, ctx[LO], ring, 0);
		igt_debug("spin[%d].handle=%d\n", n, spin[n]->handle);

		store_dword(fd, ctx[HI], ring, result, 0, n + 1, 0, I915_GEM_DOMAIN_RENDER);

		gem_set_domain(fd, result, I915_GEM_DOMAIN_GTT, 0);
		igt_assert_eq_u32(ptr[0], n + 1);
		igt_assert(gem_bo_busy(fd, spin[0]->handle));
	}

	for (int n = 0; n < 16; n++)
		igt_spin_batch_free(fd, spin[n]);

	gem_context_destroy(fd, ctx[LO]);
	gem_context_destroy(fd, ctx[HI]);

	munmap(ptr, 4096);
	gem_close(fd, result);
}

static void preempt_other(int fd, unsigned ring)
{
	uint32_t result = gem_create(fd, 4096);
	uint32_t *ptr = gem_mmap__gtt(fd, result, 4096, PROT_READ);
	igt_spin_t *spin[16];
	unsigned int other;
	unsigned int n, i;
	uint32_t ctx[3];

	/* On each engine, insert
	 * [NOISE] spinner,
	 * [LOW] write
	 *
	 * Then on our target engine do a [HIGH] write which should then
	 * prompt its dependent LOW writes in front of the spinner on
	 * each engine. The purpose of this test is to check that preemption
	 * can cross engines.
	 */

	ctx[LO] = gem_context_create(fd);
	ctx_set_priority(fd, ctx[LO], -MAX_PRIO);

	ctx[NOISE] = gem_context_create(fd);

	ctx[HI] = gem_context_create(fd);
	ctx_set_priority(fd, ctx[HI], MAX_PRIO);

	n = 0;
	for_each_engine(fd, other) {
		spin[n] = __igt_spin_batch_new(fd, ctx[NOISE], other, 0);
		store_dword(fd, ctx[LO], other,
			    result, (n + 1)*sizeof(uint32_t), n + 1,
			    0, I915_GEM_DOMAIN_RENDER);
		n++;
	}
	store_dword(fd, ctx[HI], ring,
		    result, (n + 1)*sizeof(uint32_t), n + 1,
		    0, I915_GEM_DOMAIN_RENDER);

	gem_set_domain(fd, result, I915_GEM_DOMAIN_GTT, 0);

	for (i = 0; i < n; i++) {
		igt_assert(gem_bo_busy(fd, spin[i]->handle));
		igt_spin_batch_free(fd, spin[i]);
	}

	n++;
	for (i = 0; i <= n; i++)
		igt_assert_eq_u32(ptr[i], i);

	gem_context_destroy(fd, ctx[LO]);
	gem_context_destroy(fd, ctx[NOISE]);
	gem_context_destroy(fd, ctx[HI]);

	munmap(ptr, 4096);
	gem_close(fd, result);
}

static void preempt_self(int fd, unsigned ring)
{
	uint32_t result = gem_create(fd, 4096);
	uint32_t *ptr = gem_mmap__gtt(fd, result, 4096, PROT_READ);
	igt_spin_t *spin[16];
	unsigned int other;
	unsigned int n, i;
	uint32_t ctx[3];

	/* On each engine, insert
	 * [NOISE] spinner,
	 * [self/LOW] write
	 *
	 * Then on our target engine do a [self/HIGH] write which should then
	 * preempt its own lower priority task on any engine.
	 */

	ctx[NOISE] = gem_context_create(fd);

	ctx[HI] = gem_context_create(fd);

	n = 0;
	ctx_set_priority(fd, ctx[HI], -MAX_PRIO);
	for_each_engine(fd, other) {
		spin[n] = __igt_spin_batch_new(fd, ctx[NOISE], other, 0);
		store_dword(fd, ctx[HI], other,
			    result, (n + 1)*sizeof(uint32_t), n + 1,
			    0, I915_GEM_DOMAIN_RENDER);
		n++;
	}
	ctx_set_priority(fd, ctx[HI], MAX_PRIO);
	store_dword(fd, ctx[HI], ring,
		    result, (n + 1)*sizeof(uint32_t), n + 1,
		    0, I915_GEM_DOMAIN_RENDER);

	gem_set_domain(fd, result, I915_GEM_DOMAIN_GTT, 0);

	for (i = 0; i < n; i++) {
		igt_assert(gem_bo_busy(fd, spin[i]->handle));
		igt_spin_batch_free(fd, spin[i]);
	}

	n++;
	for (i = 0; i <= n; i++)
		igt_assert_eq_u32(ptr[i], i);

	gem_context_destroy(fd, ctx[NOISE]);
	gem_context_destroy(fd, ctx[HI]);

	munmap(ptr, 4096);
	gem_close(fd, result);
}

static void deep(int fd, unsigned ring)
{
#define XS 8
	struct cork cork;
	uint32_t result, dep[XS];
	uint32_t *busy;
	uint32_t *ptr;
	uint32_t *ctx;

	ctx = malloc(sizeof(*ctx)*(MAX_PRIO + 1));
	for (int n = 0; n <= MAX_PRIO; n++) {
		ctx[n] = gem_context_create(fd);
		ctx_set_priority(fd, ctx[n], n);
	}

	result = gem_create(fd, 4096);
	for (int m = 0; m < XS; m ++)
		dep[m] = gem_create(fd, 4096);

	busy = make_busy(fd, result, ring);
	plug(fd, &cork);

	/* Create a deep dependency chain, with a few branches */
	for (int n = 0; n <= MAX_PRIO; n++)
		for (int m = 0; m < XS; m++)
			store_dword(fd, ctx[n], ring, dep[m], 4*n, ctx[n], cork.handle, I915_GEM_DOMAIN_INSTRUCTION);

	for (int n = 0; n <= MAX_PRIO; n++) {
		for (int m = 0; m < XS; m++) {
			store_dword(fd, ctx[n], ring, result, 4*n, ctx[n], dep[m], 0);
			store_dword(fd, ctx[n], ring, result, 4*m, ctx[n], 0, I915_GEM_DOMAIN_INSTRUCTION);
		}
	}

	igt_assert(gem_bo_busy(fd, result));
	unplug(&cork); /* only now submit our batches */
	igt_debugfs_dump(fd, "i915_engine_info");
	finish_busy(busy);

	for (int n = 0; n <= MAX_PRIO; n++)
		gem_context_destroy(fd, ctx[n]);

	for (int m = 0; m < XS; m++) {
		ptr = gem_mmap__gtt(fd, dep[m], 4096, PROT_READ);
		gem_set_domain(fd, dep[m], /* no write hazard lies! */
				I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);
		gem_close(fd, dep[m]);

		for (int n = 0; n <= MAX_PRIO; n++)
			igt_assert_eq_u32(ptr[n], ctx[n]);
		munmap(ptr, 4096);
	}

	ptr = gem_mmap__gtt(fd, result, 4096, PROT_READ);
	gem_set_domain(fd, result, /* no write hazard lies! */
			I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);
	gem_close(fd, result);

	for (int m = 0; m < XS; m++)
		igt_assert_eq_u32(ptr[m], ctx[MAX_PRIO]);
	munmap(ptr, 4096);

	free(ctx);
#undef XS
}

static void alarm_handler(int sig)
{
}

static int __execbuf(int fd, struct drm_i915_gem_execbuffer2 *execbuf)
{
	return ioctl(fd, DRM_IOCTL_I915_GEM_EXECBUFFER2, execbuf);
}

static unsigned int measure_ring_size(int fd, unsigned int ring)
{
	struct sigaction sa = { .sa_handler = alarm_handler };
	struct drm_i915_gem_exec_object2 obj[2];
	struct drm_i915_gem_execbuffer2 execbuf;
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	unsigned int count, last;
	struct itimerval itv;
	struct cork c;

	memset(obj, 0, sizeof(obj));
	obj[1].handle = gem_create(fd, 4096);
	gem_write(fd, obj[1].handle, 0, &bbe, sizeof(bbe));

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(obj + 1);
	execbuf.buffer_count = 1;
	execbuf.flags = ring;
	gem_execbuf(fd, &execbuf);
	gem_sync(fd, obj[1].handle);

	plug(fd, &c);
	obj[0].handle = c.handle;

	execbuf.buffers_ptr = to_user_pointer(obj);
	execbuf.buffer_count = 2;

	sigaction(SIGALRM, &sa, NULL);
	itv.it_interval.tv_sec = 0;
	itv.it_interval.tv_usec = 100;
	itv.it_value.tv_sec = 0;
	itv.it_value.tv_usec = 1000;
	setitimer(ITIMER_REAL, &itv, NULL);

	last = -1;
	count = 0;
	do {
		if (__execbuf(fd, &execbuf) == 0) {
			count++;
			continue;
		}

		if (last == count)
			break;

		last = count;
	} while (1);

	memset(&itv, 0, sizeof(itv));
	setitimer(ITIMER_REAL, &itv, NULL);

	unplug(&c);
	gem_close(fd, obj[1].handle);

	return count;
}

static void wide(int fd, unsigned ring)
{
#define NCTX 4096
	struct timespec tv = {};
	unsigned int ring_size = measure_ring_size(fd, ring);

	struct cork cork;
	uint32_t result;
	uint32_t *busy;
	uint32_t *ptr;
	uint32_t *ctx;
	unsigned int count;

	ctx = malloc(sizeof(*ctx)*NCTX);
	for (int n = 0; n < NCTX; n++)
		ctx[n] = gem_context_create(fd);

	result = gem_create(fd, 4*NCTX);

	busy = make_busy(fd, result, ring);
	plug(fd, &cork);

	/* Lots of in-order requests, plugged and submitted simultaneously */
	for (count = 0;
	     igt_seconds_elapsed(&tv) < 5 && count < ring_size;
	     count++) {
		for (int n = 0; n < NCTX; n++) {
			store_dword(fd, ctx[n], ring, result, 4*n, ctx[n], cork.handle, I915_GEM_DOMAIN_INSTRUCTION);
		}
	}
	igt_info("Submitted %d requests over %d contexts in %.1fms\n",
		 count, NCTX, igt_nsec_elapsed(&tv) * 1e-6);

	igt_assert(gem_bo_busy(fd, result));
	unplug(&cork); /* only now submit our batches */
	igt_debugfs_dump(fd, "i915_engine_info");
	finish_busy(busy);

	for (int n = 0; n < NCTX; n++)
		gem_context_destroy(fd, ctx[n]);

	ptr = gem_mmap__gtt(fd, result, 4*NCTX, PROT_READ);
	gem_set_domain(fd, result, /* no write hazard lies! */
			I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);
	for (int n = 0; n < NCTX; n++)
		igt_assert_eq_u32(ptr[n], ctx[n]);
	munmap(ptr, 4*NCTX);

	gem_close(fd, result);
	free(ctx);
#undef NCTX
}

static void reorder_wide(int fd, unsigned ring)
{
	const int gen = intel_gen(intel_get_drm_devid(fd));
	struct drm_i915_gem_relocation_entry reloc;
	struct drm_i915_gem_exec_object2 obj[3];
	struct drm_i915_gem_execbuffer2 execbuf;
	struct cork cork;
	uint32_t result, target;
	uint32_t *busy;
	uint32_t *r, *t;

	result = gem_create(fd, 4096);
	target = gem_create(fd, 4096);

	busy = make_busy(fd, result, ring);
	plug(fd, &cork);

	t = gem_mmap__cpu(fd, target, 0, 4096, PROT_WRITE);
	gem_set_domain(fd, target, I915_GEM_DOMAIN_CPU, I915_GEM_DOMAIN_CPU);

	memset(obj, 0, sizeof(obj));
	obj[0].handle = cork.handle;
	obj[1].handle = result;
	obj[2].relocs_ptr = to_user_pointer(&reloc);
	obj[2].relocation_count = 1;

	memset(&reloc, 0, sizeof(reloc));
	reloc.target_handle = result;
	reloc.read_domains = I915_GEM_DOMAIN_INSTRUCTION;
	reloc.write_domain = 0; /* lies */

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(obj);
	execbuf.buffer_count = 3;
	execbuf.flags = ring;
	if (gen < 6)
		execbuf.flags |= I915_EXEC_SECURE;

	for (int n = -MAX_PRIO, x = 1; n <= MAX_PRIO; n++, x++) {
		uint32_t *batch;

		execbuf.rsvd1 = gem_context_create(fd);
		ctx_set_priority(fd, execbuf.rsvd1, n);

		obj[2].handle = gem_create(fd, 128 * 64);
		batch = gem_mmap__gtt(fd, obj[2].handle, 128 * 64, PROT_WRITE);
		gem_set_domain(fd, obj[2].handle, I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);

		for (int m = 0; m < 128; m++) {
			uint64_t addr;
			int idx = hars_petruska_f54_1_random_unsafe_max( 1024);
			int i;

			execbuf.batch_start_offset = m * 64;
			reloc.offset = execbuf.batch_start_offset + sizeof(uint32_t);
			reloc.delta = idx * sizeof(uint32_t);
			addr = reloc.presumed_offset + reloc.delta;

			i = execbuf.batch_start_offset / sizeof(uint32_t);
			batch[i] = MI_STORE_DWORD_IMM | (gen < 6 ? 1 << 22 : 0);
			if (gen >= 8) {
				batch[++i] = addr;
				batch[++i] = addr >> 32;
			} else if (gen >= 4) {
				batch[++i] = 0;
				batch[++i] = addr;
				reloc.offset += sizeof(uint32_t);
			} else {
				batch[i]--;
				batch[++i] = addr;
			}
			batch[++i] = x;
			batch[++i] = MI_BATCH_BUFFER_END;

			if (!t[idx])
				t[idx] =  x;

			gem_execbuf(fd, &execbuf);
		}

		munmap(batch, 128 * 64);
		gem_close(fd, obj[2].handle);
		gem_context_destroy(fd, execbuf.rsvd1);
	}

	igt_assert(gem_bo_busy(fd, result));
	unplug(&cork); /* only now submit our batches */
	igt_debugfs_dump(fd, "i915_engine_info");
	finish_busy(busy);

	r = gem_mmap__gtt(fd, result, 4096, PROT_READ);
	gem_set_domain(fd, result, /* no write hazard lies! */
			I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);
	for (int n = 0; n < 1024; n++)
		igt_assert_eq_u32(r[n], t[n]);
	munmap(r, 4096);
	munmap(t, 4096);

	gem_close(fd, result);
	gem_close(fd, target);
}

static bool has_scheduler(int fd)
{
	drm_i915_getparam_t gp;
	int has = -1;

	gp.param = LOCAL_PARAM_HAS_SCHEDULER;
	gp.value = &has;
	drmIoctl(fd, DRM_IOCTL_I915_GETPARAM, &gp);

	return has > 0;
}

igt_main
{
	const struct intel_execution_engine *e;
	int fd = -1;

	igt_skip_on_simulation();

	igt_fixture {
		fd = drm_open_driver_master(DRIVER_INTEL);
		igt_require_gem(fd);
		gem_require_mmap_wc(fd);
		igt_fork_hang_detector(fd);
	}

	igt_subtest_group {
		for (e = intel_execution_engines; e->name; e++) {
			/* default exec-id is purely symbolic */
			if (e->exec_id == 0)
				continue;

			igt_subtest_f("fifo-%s", e->name) {
				gem_require_ring(fd, e->exec_id | e->flags);
				igt_require(gem_can_store_dword(fd, e->exec_id) | e->flags);
				fifo(fd, e->exec_id | e->flags);
			}
		}
	}

	igt_subtest_group {
		igt_fixture {
			igt_require(has_scheduler(fd));
			ctx_has_priority(fd);
		}

		for (e = intel_execution_engines; e->name; e++) {
			/* default exec-id is purely symbolic */
			if (e->exec_id == 0)
				continue;

			igt_subtest_group {
				igt_fixture {
					gem_require_ring(fd, e->exec_id | e->flags);
					igt_require(gem_can_store_dword(fd, e->exec_id) | e->flags);
				}

				igt_subtest_f("in-order-%s", e->name)
					reorder(fd, e->exec_id | e->flags, EQUAL);

				igt_subtest_f("out-order-%s", e->name)
					reorder(fd, e->exec_id | e->flags, 0);

				igt_subtest_f("promotion-%s", e->name)
					promotion(fd, e->exec_id | e->flags);

				igt_subtest_f("preempt-%s", e->name)
					preempt(fd, e->exec_id | e->flags, 0);

				igt_subtest_f("preempt-contexts-%s", e->name)
					preempt(fd, e->exec_id | e->flags, NEW_CTX);

				igt_subtest_f("preempt-other-%s", e->name)
					preempt_other(fd, e->exec_id | e->flags);

				igt_subtest_f("preempt-self-%s", e->name)
					preempt_self(fd, e->exec_id | e->flags);

				igt_subtest_f("deep-%s", e->name)
					deep(fd, e->exec_id | e->flags);

				igt_subtest_f("wide-%s", e->name)
					wide(fd, e->exec_id | e->flags);

				igt_subtest_f("reorder-wide-%s", e->name)
					reorder_wide(fd, e->exec_id | e->flags);
			}
		}
	}

	igt_fixture {
		igt_stop_hang_detector();
		close(fd);
	}
}
