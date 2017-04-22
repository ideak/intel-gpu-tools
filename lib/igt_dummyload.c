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
 *
 */

#include "igt.h"
#include "igt_dummyload.h"
#include <time.h>
#include <signal.h>
#include <sys/syscall.h>

/**
 * SECTION:igt_dummyload
 * @short_description: Library for submitting GPU workloads
 * @title: Dummyload
 * @include: igt.h
 *
 * A lot of igt testcases need some GPU workload to make sure a race window is
 * big enough. Unfortunately having a fixed amount of workload leads to
 * spurious test failures or overly long runtimes on some fast/slow platforms.
 * This library contains functionality to submit GPU workloads that should
 * consume exactly a specific amount of time.
 */

#define LOCAL_I915_EXEC_BSD_SHIFT      (13)
#define LOCAL_I915_EXEC_BSD_MASK       (3 << LOCAL_I915_EXEC_BSD_SHIFT)

#define ENGINE_MASK  (I915_EXEC_RING_MASK | LOCAL_I915_EXEC_BSD_MASK)

static const int BATCH_SIZE = 4096;
static IGT_LIST(spin_list);

static void
fill_reloc(struct drm_i915_gem_relocation_entry *reloc,
	   uint32_t gem_handle, uint32_t offset,
	   uint32_t read_domains, uint32_t write_domains)
{
	reloc->target_handle = gem_handle;
	reloc->offset = offset * sizeof(uint32_t);
	reloc->read_domains = read_domains;
	reloc->write_domain = write_domains;
}

static void emit_recursive_batch(igt_spin_t *spin,
				 int fd, int engine, unsigned int dep_handle)
{
#define SCRATCH 0
#define BATCH 1
	const int gen = intel_gen(intel_get_drm_devid(fd));
	struct drm_i915_gem_exec_object2 obj[2];
	struct drm_i915_gem_relocation_entry relocs[2];
	struct drm_i915_gem_execbuffer2 execbuf;
	unsigned int engines[16];
	unsigned int nengine;
	uint32_t *batch;
	int i;

	nengine = 0;
	if (engine < 0) {
		for_each_engine(fd, engine)
			if (engine)
				engines[nengine++] = engine;
	} else {
		gem_require_ring(fd, engine);
		engines[nengine++] = engine;
	}
	igt_require(nengine);

	memset(&execbuf, 0, sizeof(execbuf));
	memset(obj, 0, sizeof(obj));
	memset(relocs, 0, sizeof(relocs));

	obj[BATCH].handle = gem_create(fd, BATCH_SIZE);
	batch = __gem_mmap__wc(fd, obj[BATCH].handle,
			       0, BATCH_SIZE, PROT_WRITE);
	if (!batch)
		batch = __gem_mmap__gtt(fd, obj[BATCH].handle,
				       	BATCH_SIZE, PROT_WRITE);
	gem_set_domain(fd, obj[BATCH].handle,
			I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);
	execbuf.buffer_count++;

	if (dep_handle > 0) {
		/* dummy write to dependency */
		obj[SCRATCH].handle = dep_handle;
		fill_reloc(&relocs[obj[BATCH].relocation_count++],
			   dep_handle, 256,
			   I915_GEM_DOMAIN_RENDER,
			   I915_GEM_DOMAIN_RENDER);
		execbuf.buffer_count++;
	}

	spin->batch = batch;
	spin->handle = obj[BATCH].handle;

	/* recurse */
	fill_reloc(&relocs[obj[BATCH].relocation_count],
		   obj[BATCH].handle, 1, I915_GEM_DOMAIN_COMMAND, 0);
	if (gen >= 8) {
		*batch++ = MI_BATCH_BUFFER_START | 1 << 8 | 1;
		*batch++ = 0;
		*batch++ = 0;
	} else if (gen >= 6) {
		*batch++ = MI_BATCH_BUFFER_START | 1 << 8;
		*batch++ = 0;
	} else {
		*batch++ = MI_BATCH_BUFFER_START | 2 << 6;
		*batch = 0;
		if (gen < 4) {
			*batch |= 1;
			relocs[obj[BATCH].relocation_count].delta = 1;
		}
		batch++;
	}
	obj[BATCH].relocation_count++;
	obj[BATCH].relocs_ptr = to_user_pointer(relocs);

	execbuf.buffers_ptr = to_user_pointer(obj + (2 - execbuf.buffer_count));

	for (i = 0; i < nengine; i++) {
		execbuf.flags &= ~ENGINE_MASK;
		execbuf.flags = engines[i];
		gem_execbuf(fd, &execbuf);
	}
}

/**
 * igt_spin_batch_new:
 * @fd: open i915 drm file descriptor
 * @engine: Ring to execute batch OR'd with execbuf flags. If value is less
 *          than 0, execute on all available rings.
 * @dep_handle: handle to a buffer object dependency. If greater than 0, add a
 *              relocation entry to this buffer within the batch.
 *
 * Start a recursive batch on a ring. Immediately returns a #igt_spin_t that
 * contains the batch's handle that can be waited upon. The returned structure
 * must be passed to igt_spin_batch_free() for post-processing.
 *
 * Returns:
 * Structure with helper internal state for igt_spin_batch_free().
 */
igt_spin_t *
igt_spin_batch_new(int fd, int engine, unsigned int dep_handle)
{
	igt_spin_t *spin;

	igt_require_gem(fd);

	spin = calloc(1, sizeof(struct igt_spin));
	igt_assert(spin);

	emit_recursive_batch(spin, fd, engine, dep_handle);
	igt_assert(gem_bo_busy(fd, spin->handle));

	igt_list_add(&spin->link, &spin_list);

	return spin;
}

static void notify(union sigval arg)
{
	igt_spin_t *spin = arg.sival_ptr;

	igt_spin_batch_end(spin);
}

/**
 * igt_spin_batch_set_timeout:
 * @spin: spin batch state from igt_spin_batch_new()
 * @ns: amount of time in nanoseconds the batch continues to execute
 *      before finishing.
 *
 * Specify a timeout. This ends the recursive batch associated with @spin after
 * the timeout has elapsed.
 */
void igt_spin_batch_set_timeout(igt_spin_t *spin, int64_t ns)
{
	timer_t timer;
	struct sigevent sev;
	struct itimerspec its;

	igt_assert(ns > 0);
	if (!spin)
		return;

	igt_assert(!spin->timer);

	memset(&sev, 0, sizeof(sev));
	sev.sigev_notify = SIGEV_THREAD;
	sev.sigev_value.sival_ptr = spin;
	sev.sigev_notify_function = notify;
	igt_assert(timer_create(CLOCK_MONOTONIC, &sev, &timer) == 0);
	igt_assert(timer);

	memset(&its, 0, sizeof(its));
	its.it_value.tv_sec = ns / NSEC_PER_SEC;
	its.it_value.tv_nsec = ns % NSEC_PER_SEC;
	igt_assert(timer_settime(timer, 0, &its, NULL) == 0);

	spin->timer = timer;
}

/**
 * igt_spin_batch_end:
 * @spin: spin batch state from igt_spin_batch_new()
 *
 * End the recursive batch associated with @spin manually.
 */
void igt_spin_batch_end(igt_spin_t *spin)
{
	if (!spin)
		return;

	*spin->batch = MI_BATCH_BUFFER_END;
	__sync_synchronize();
}

/**
 * igt_spin_batch_free:
 * @fd: open i915 drm file descriptor
 * @spin: spin batch state from igt_spin_batch_new()
 *
 * This function does the necessary post-processing after starting a recursive
 * batch with igt_spin_batch_new().
 */
void igt_spin_batch_free(int fd, igt_spin_t *spin)
{
	if (!spin)
		return;

	igt_list_del(&spin->link);

	if (spin->timer)
		timer_delete(spin->timer);

	igt_spin_batch_end(spin);
	gem_munmap(spin->batch, BATCH_SIZE);

	gem_close(fd, spin->handle);
	free(spin);
}

void igt_terminate_spin_batches(void)
{
	struct igt_spin *iter;

	igt_list_for_each(iter, &spin_list, link)
		igt_spin_batch_end(iter);
}
