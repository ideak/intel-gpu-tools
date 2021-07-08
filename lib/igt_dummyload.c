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

#include <time.h>
#include <signal.h>
#include <pthread.h>
#include <sys/poll.h>
#include <sys/timerfd.h>

#include <i915_drm.h>

#include "drmtest.h"
#include "i915/gem_create.h"
#include "i915/gem_engine_topology.h"
#include "i915/gem_mman.h"
#include "i915/gem_submission.h"
#include "igt_core.h"
#include "igt_device.h"
#include "igt_dummyload.h"
#include "igt_gt.h"
#include "igt_vgem.h"
#include "intel_allocator.h"
#include "intel_chipset.h"
#include "intel_reg.h"
#include "ioctl_wrappers.h"
#include "sw_sync.h"

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

#define ENGINE_MASK  (I915_EXEC_RING_MASK | I915_EXEC_BSD_MASK)

#define MI_ARB_CHK (0x5 << 23)

static const int BATCH_SIZE = 4096;
static const int LOOP_START_OFFSET = 64;

static IGT_LIST_HEAD(spin_list);
static pthread_mutex_t list_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t
handle_create(int fd, size_t sz, unsigned long flags, uint32_t **mem)
{
	*mem = NULL;

	if (flags & IGT_SPIN_USERPTR) {
		uint32_t handle;

		*mem = mmap(NULL, sz, PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
		igt_assert(*mem != (uint32_t *)-1);
		gem_userptr(fd, *mem, sz, 0, 0, &handle);

		return handle;
	}

	return gem_create(fd, sz);
}

static int
emit_recursive_batch(igt_spin_t *spin,
		     int fd, const struct igt_spin_factory *opts)
{
#define SCRATCH 0
#define BATCH IGT_SPIN_BATCH
	const unsigned int devid = intel_get_drm_devid(fd);
	const unsigned int gen = intel_gen(devid);
	struct drm_i915_gem_relocation_entry relocs[3], *r;
	struct drm_i915_gem_execbuffer2 *execbuf;
	struct drm_i915_gem_exec_object2 *obj;
	unsigned int flags[GEM_MAX_ENGINES];
	unsigned int nengine;
	int fence_fd = -1;
	uint64_t addr;
	uint32_t *cs;
	int i;

	/*
	 * Pick a random location for our spinner et al.
	 *
	 * If available, the kernel will place our objects in our hinted
	 * locations and we will avoid having to perform any relocations.
	 *
	 * It must be a valid location (or else the kernel will be forced
	 * to select one for us) and so must be within the GTT and suitably
	 * aligned. For simplicity, stick to the low 32bit addresses.
	 *
	 * One odd restriction to remember is that batches with relocations
	 * are not allowed in the first 256KiB, for fear of negative relocations
	 * that wrap.
	 */
	addr = gem_aperture_size(fd) / 2;
	if (addr >> 31)
		addr = 1u << 31;
	addr += random() % addr / 2;
	addr &= -4096;

	igt_assert(!(opts->ctx && opts->ctx_id));

	nengine = 0;
	if (opts->engine == ALL_ENGINES) {
		struct intel_execution_engine2 *engine;

		igt_assert(opts->ctx);
		for_each_ctx_engine(fd, opts->ctx, engine) {
			if (opts->flags & IGT_SPIN_POLL_RUN &&
			    !gem_class_can_store_dword(fd, engine->class))
				continue;

			flags[nengine++] = engine->flags;
		}
	} else {
		flags[nengine++] = opts->engine;
	}
	igt_require(nengine);

	memset(relocs, 0, sizeof(relocs));
	execbuf = memset(&spin->execbuf, 0, sizeof(spin->execbuf));
	execbuf->flags = I915_EXEC_NO_RELOC;
	obj = memset(spin->obj, 0, sizeof(spin->obj));

	obj[BATCH].handle =
		handle_create(fd, BATCH_SIZE, opts->flags, &spin->batch);
	if (!spin->batch) {
		spin->batch = gem_mmap__device_coherent(fd, obj[BATCH].handle,
						  0, BATCH_SIZE, PROT_WRITE);
		gem_set_domain(fd, obj[BATCH].handle,
			       I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);
	} else if (gen < 6) {
		gem_set_caching(fd, obj[BATCH].handle, I915_CACHING_NONE);
		igt_require(igt_setup_clflush());
		spin->flags |= SPIN_CLFLUSH;
	}
	execbuf->buffer_count++;
	cs = spin->batch;

	obj[BATCH].offset = addr;
	addr += BATCH_SIZE;

	if (opts->dependency) {
		igt_assert(!(opts->flags & IGT_SPIN_POLL_RUN));

		obj[SCRATCH].handle = opts->dependency;
		obj[SCRATCH].offset = addr;
		if (!(opts->flags & IGT_SPIN_SOFTDEP)) {
			obj[SCRATCH].flags = EXEC_OBJECT_WRITE;

			/* dummy write to dependency */
			r = &relocs[obj[BATCH].relocation_count++];
			r->presumed_offset = obj[SCRATCH].offset;
			r->target_handle = obj[SCRATCH].handle;
			r->offset = sizeof(uint32_t) * 1020;
			r->delta = 0;
			r->read_domains = I915_GEM_DOMAIN_RENDER;
			r->write_domain = I915_GEM_DOMAIN_RENDER;
		}

		execbuf->buffer_count++;
	} else if (opts->flags & IGT_SPIN_POLL_RUN) {
		r = &relocs[obj[BATCH].relocation_count++];

		igt_assert(!opts->dependency);

		if (gen == 4 || gen == 5) {
			execbuf->flags |= I915_EXEC_SECURE;
			igt_require(__igt_device_set_master(fd) == 0);
		}

		spin->poll_handle =
			handle_create(fd, 4096, opts->flags, &spin->poll);
		obj[SCRATCH].handle = spin->poll_handle;

		if (!spin->poll) {
			if (__gem_set_caching(fd, spin->poll_handle,
					      I915_CACHING_CACHED) == 0)
				spin->poll = gem_mmap__cpu(fd, spin->poll_handle,
							   0, 4096,
							   PROT_READ | PROT_WRITE);
			else
				spin->poll = gem_mmap__device_coherent(fd,
								       spin->poll_handle,
								       0, 4096,
								       PROT_READ | PROT_WRITE);
		}
		addr += 4096; /* guard page */
		obj[SCRATCH].offset = addr;
		addr += 4096;

		igt_assert_eq(spin->poll[SPIN_POLL_START_IDX], 0);

		r->presumed_offset = obj[SCRATCH].offset;
		r->target_handle = obj[SCRATCH].handle;
		r->offset = sizeof(uint32_t) * 1;
		r->delta = sizeof(uint32_t) * SPIN_POLL_START_IDX;

		*cs++ = MI_STORE_DWORD_IMM | (gen < 6 ? 1 << 22 : 0);

		if (gen >= 8) {
			*cs++ = r->presumed_offset + r->delta;
			*cs++ = 0;
		} else if (gen >= 4) {
			*cs++ = 0;
			*cs++ = r->presumed_offset + r->delta;
			r->offset += sizeof(uint32_t);
		} else {
			cs[-1]--;
			*cs++ = r->presumed_offset + r->delta;
		}

		*cs++ = 1;

		execbuf->buffer_count++;
	}

	spin->handle = obj[BATCH].handle;

	igt_assert_lt(cs - spin->batch, LOOP_START_OFFSET / sizeof(*cs));
	spin->condition = spin->batch + LOOP_START_OFFSET / sizeof(*cs);
	cs = spin->condition;

	/* Allow ourselves to be preempted */
	if (!(opts->flags & IGT_SPIN_NO_PREEMPTION))
		*cs++ = MI_ARB_CHK;
	if (opts->flags & IGT_SPIN_INVALID_CS) {
		igt_assert(opts->ctx);
		if (!gem_engine_has_cmdparser(fd, &opts->ctx->cfg, opts->engine))
			*cs++ = 0xdeadbeef;
	}

	/* Pad with a few nops so that we do not completely hog the system.
	 *
	 * Part of the attraction of using a recursive batch is that it is
	 * hard on the system (executing the "function" call is apparently
	 * quite expensive). However, the GPU may hog the entire system for
	 * a few minutes, preventing even NMI. Quite why this is so is unclear,
	 * but presumably it relates to the PM_INTRMSK workaround on gen6/gen7.
	 * If we give the system a break by having the GPU execute a few nops
	 * between function calls, that appears enough to keep SNB out of
	 * trouble. See https://bugs.freedesktop.org/show_bug.cgi?id=102262
	 */
	if (!(opts->flags & IGT_SPIN_FAST))
		cs += 960;

	/*
	 * When using a cmdparser, the batch is copied into a read only location
	 * and validated. We are then unable to alter the executing batch,
	 * breaking the older *spin->condition = MI_BB_END termination.
	 * Instead we can use a conditional MI_BB_END here that looks at
	 * the user's copy of the batch and terminates when they modified it,
	 * no matter how they modify it (from either the GPU or CPU).
	 */
	if (gen >= 8) { /* arbitrary cutoff between ring/execlists submission */
		r = &relocs[obj[BATCH].relocation_count++];

		/*
		 * On Sandybridge+ the comparison is a strict greater-than:
		 * if the value at spin->condition is greater than BB_END,
		 * we loop back to the beginning.
		 * Beginning with Kabylake, we can select the comparison mode
		 * and loop back to the beginning if spin->condition != BB_END
		 * (using 5 << 12).
		 * For simplicity, we try to stick to a one-size fits all.
		 */
		spin->condition = spin->batch + BATCH_SIZE / sizeof(*spin->batch) - 2;
		spin->condition[0] = 0xffffffff;
		spin->condition[1] = 0xffffffff;

		r->presumed_offset = obj[BATCH].offset;
		r->target_handle = obj[BATCH].handle;
		r->offset = (cs + 2 - spin->batch) * sizeof(*cs);
		r->read_domains = I915_GEM_DOMAIN_COMMAND;
		r->delta = (spin->condition - spin->batch) * sizeof(*cs);

		*cs++ = MI_COND_BATCH_BUFFER_END | MI_DO_COMPARE | 2;
		*cs++ = MI_BATCH_BUFFER_END;
		*cs++ = r->presumed_offset + r->delta;
		*cs++ = 0;
	}

	/* recurse */
	r = &relocs[obj[BATCH].relocation_count++];
	r->target_handle = obj[BATCH].handle;
	r->presumed_offset = obj[BATCH].offset;
	r->offset = (cs + 1 - spin->batch) * sizeof(*cs);
	r->read_domains = I915_GEM_DOMAIN_COMMAND;
	r->delta = LOOP_START_OFFSET;
	if (gen >= 8) {
		*cs++ = MI_BATCH_BUFFER_START | 1 << 8 | 1;
		*cs++ = r->presumed_offset + r->delta;
		*cs++ = 0;
	} else if (gen >= 6) {
		*cs++ = MI_BATCH_BUFFER_START | 1 << 8;
		*cs++ = r->presumed_offset + r->delta;
	} else {
		*cs++ = MI_BATCH_BUFFER_START | 2 << 6;
		if (gen < 4)
			r->delta |= 1;
		*cs = r->presumed_offset + r->delta;
		cs++;
	}
	obj[BATCH].relocs_ptr = to_user_pointer(relocs);

	execbuf->buffers_ptr =
	       	to_user_pointer(obj + (2 - execbuf->buffer_count));
	execbuf->rsvd1 = opts->ctx ? opts->ctx->id : opts->ctx_id;

	if (opts->flags & IGT_SPIN_FENCE_OUT)
		execbuf->flags |= I915_EXEC_FENCE_OUT;

	if (opts->flags & IGT_SPIN_FENCE_IN && opts->fence != -1) {
		execbuf->flags |= I915_EXEC_FENCE_IN;
		execbuf->rsvd2 = opts->fence;
	}

	if (opts->flags & IGT_SPIN_FENCE_SUBMIT && opts->fence != -1) {
		execbuf->flags |= I915_EXEC_FENCE_SUBMIT;
		execbuf->rsvd2 = opts->fence;
	}

	for (i = 0; i < nengine; i++) {
		execbuf->flags &= ~ENGINE_MASK;
		execbuf->flags |= flags[i];

		gem_execbuf_wr(fd, execbuf);

		if (opts->flags & IGT_SPIN_FENCE_OUT) {
			int _fd = execbuf->rsvd2 >> 32;

			igt_assert(_fd >= 0);
			if (fence_fd == -1) {
				fence_fd = _fd;
			} else {
				int old_fd = fence_fd;

				fence_fd = sync_fence_merge(old_fd, _fd);
				close(old_fd);
				close(_fd);
			}
			igt_assert(fence_fd >= 0);
		}
	}

	igt_assert_lt(cs - spin->batch, BATCH_SIZE / sizeof(*cs));

	/* Make it easier for callers to resubmit. */
	for (i = 0; i < ARRAY_SIZE(spin->obj); i++) {
		spin->obj[i].relocation_count = 0;
		spin->obj[i].relocs_ptr = 0;
		spin->obj[i].offset = CANONICAL(spin->obj[i].offset);
		spin->obj[i].flags |= EXEC_OBJECT_PINNED;
	}

	spin->cmd_precondition = *spin->condition;

	return fence_fd;
}

static igt_spin_t *
spin_create(int fd, const struct igt_spin_factory *opts)
{
	igt_spin_t *spin;

	spin = calloc(1, sizeof(struct igt_spin));
	igt_assert(spin);

	spin->timerfd = -1;
	spin->out_fence = emit_recursive_batch(spin, fd, opts);

	pthread_mutex_lock(&list_lock);
	igt_list_add(&spin->link, &spin_list);
	pthread_mutex_unlock(&list_lock);

	return spin;
}

igt_spin_t *
__igt_spin_factory(int fd, const struct igt_spin_factory *opts)
{
	return spin_create(fd, opts);
}

/**
 * igt_spin_factory:
 * @fd: open i915 drm file descriptor
 * @opts: controlling options such as context, engine, dependencies etc
 *
 * Start a recursive batch on a ring. Immediately returns a #igt_spin_t that
 * contains the batch's handle that can be waited upon. The returned structure
 * must be passed to igt_spin_free() for post-processing.
 *
 * Returns:
 * Structure with helper internal state for igt_spin_free().
 */
igt_spin_t *
igt_spin_factory(int fd, const struct igt_spin_factory *opts)
{
	igt_spin_t *spin;

	if ((opts->flags & IGT_SPIN_POLL_RUN) && opts->engine != ALL_ENGINES) {
		unsigned int class;

		igt_assert(opts->ctx);
		class = intel_ctx_engine_class(opts->ctx, opts->engine);
		igt_require(gem_class_can_store_dword(fd, class));
	}

	if (opts->flags & IGT_SPIN_INVALID_CS) {
		igt_assert(opts->ctx);
		igt_require(!gem_engine_has_cmdparser(fd, &opts->ctx->cfg,
						      opts->engine));
	}

	spin = spin_create(fd, opts);

	if (!(opts->flags & IGT_SPIN_INVALID_CS)) {
		/*
		 * When injecting invalid CS into the batch, the spinner may
		 * be killed immediately -- i.e. may already be completed!
		 */
		igt_assert(gem_bo_busy(fd, spin->handle));
		if (opts->flags & IGT_SPIN_FENCE_OUT) {
			struct pollfd pfd = { spin->out_fence, POLLIN };

			igt_assert(poll(&pfd, 1, 0) == 0);
		}
	}

	return spin;
}

static void *timer_thread(void *data)
{
	igt_spin_t *spin = data;
	uint64_t overruns = 0;

	/* Wait until we see the timer fire, or we get cancelled */
	do {
		read(spin->timerfd, &overruns, sizeof(overruns));
	} while (!overruns);

	igt_spin_end(spin);
	return NULL;
}

/**
 * igt_spin_set_timeout:
 * @spin: spin state from igt_spin_new()
 * @ns: amount of time in nanoseconds the batch continues to execute
 *      before finishing.
 *
 * Specify a timeout. This ends the recursive batch associated with @spin after
 * the timeout has elapsed.
 */
void igt_spin_set_timeout(igt_spin_t *spin, int64_t ns)
{
	struct sched_param param = { .sched_priority = 99 };
	struct itimerspec its;
	pthread_attr_t attr;
	int timerfd;

	if (!spin)
		return;

	if (ns <= 0) {
		igt_spin_end(spin);
		return;
	}

	igt_assert(spin->timerfd == -1);
	timerfd = timerfd_create(CLOCK_MONOTONIC, 0);
	igt_assert(timerfd >= 0);
	spin->timerfd = timerfd;

	pthread_attr_init(&attr);
	pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
	pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
	pthread_attr_setschedparam(&attr, &param);

	igt_assert(pthread_create(&spin->timer_thread, &attr,
				  timer_thread, spin) == 0);
	pthread_attr_destroy(&attr);

	memset(&its, 0, sizeof(its));
	its.it_value.tv_sec = ns / NSEC_PER_SEC;
	its.it_value.tv_nsec = ns % NSEC_PER_SEC;
	igt_assert(timerfd_settime(timerfd, 0, &its, NULL) == 0);
}

static void sync_write(igt_spin_t *spin, uint32_t value)
{
	*spin->condition = value;
	if (spin->flags & SPIN_CLFLUSH)
		igt_clflush_range(spin->condition, sizeof(*spin->condition));
	__sync_synchronize();
}

/**
 * igt_spin_reset:
 * @spin: spin state from igt_spin_new()
 *
 * Reset the state of spin, allowing its reuse.
 */
void igt_spin_reset(igt_spin_t *spin)
{
	if (igt_spin_has_poll(spin))
		spin->poll[SPIN_POLL_START_IDX] = 0;

	sync_write(spin, spin->cmd_precondition);
	memset(&spin->last_signal, 0, sizeof(spin->last_signal));
}

/**
 * igt_spin_end:
 * @spin: spin state from igt_spin_new()
 *
 * End the spinner associated with @spin manually.
 */
void igt_spin_end(igt_spin_t *spin)
{
	if (!spin)
		return;

	igt_gettime(&spin->last_signal);
	sync_write(spin, MI_BATCH_BUFFER_END);
}

static void __igt_spin_free(int fd, igt_spin_t *spin)
{
	if (spin->timerfd >= 0) {
		pthread_cancel(spin->timer_thread);
		igt_assert(pthread_join(spin->timer_thread, NULL) == 0);
		close(spin->timerfd);
	}

	igt_spin_end(spin);

	if (spin->poll)
		gem_munmap(spin->poll, 4096);
	if (spin->batch)
		gem_munmap(spin->batch, BATCH_SIZE);

	if (spin->poll_handle)
		gem_close(fd, spin->poll_handle);

	if (spin->handle)
		gem_close(fd, spin->handle);

	if (spin->out_fence >= 0)
		close(spin->out_fence);

	free(spin);
}

/**
 * igt_spin_free:
 * @fd: open i915 drm file descriptor
 * @spin: spin state from igt_spin_new()
 *
 * This function does the necessary post-processing after starting a
 * spin with igt_spin_new() and then frees it.
 */
void igt_spin_free(int fd, igt_spin_t *spin)
{
	if (!spin)
		return;

	pthread_mutex_lock(&list_lock);
	igt_list_del(&spin->link);
	pthread_mutex_unlock(&list_lock);

	__igt_spin_free(fd, spin);
}

void igt_terminate_spins(void)
{
	struct igt_spin *iter;

	pthread_mutex_lock(&list_lock);
	igt_list_for_each_entry(iter, &spin_list, link)
		igt_spin_end(iter);
	pthread_mutex_unlock(&list_lock);
}

void igt_free_spins(int i915)
{
	struct igt_spin *iter, *next;

	pthread_mutex_lock(&list_lock);
	igt_list_for_each_entry_safe(iter, next, &spin_list, link)
		__igt_spin_free(i915, iter);
	IGT_INIT_LIST_HEAD(&spin_list);
	pthread_mutex_unlock(&list_lock);
}

void igt_unshare_spins(void)
{
	struct igt_spin *it, *n;

	/* Disable the automatic termination on inherited spinners */
	igt_list_for_each_entry_safe(it, n, &spin_list, link)
		IGT_INIT_LIST_HEAD(&it->link);
	IGT_INIT_LIST_HEAD(&spin_list);
}

static uint32_t plug_vgem_handle(struct igt_cork *cork, int fd)
{
	struct vgem_bo bo;
	int dmabuf;
	uint32_t handle;

	cork->vgem.device = drm_open_driver(DRIVER_VGEM);
	igt_require(vgem_has_fences(cork->vgem.device));

	bo.width = bo.height = 1;
	bo.bpp = 4;
	vgem_create(cork->vgem.device, &bo);
	cork->vgem.fence = vgem_fence_attach(cork->vgem.device, &bo, VGEM_FENCE_WRITE);

	dmabuf = prime_handle_to_fd(cork->vgem.device, bo.handle);
	handle = prime_fd_to_handle(fd, dmabuf);
	close(dmabuf);

	return handle;
}

static void unplug_vgem_handle(struct igt_cork *cork)
{
	vgem_fence_signal(cork->vgem.device, cork->vgem.fence);
	close(cork->vgem.device);
}

static uint32_t plug_sync_fd(struct igt_cork *cork)
{
	int fence;

	igt_require_sw_sync();

	cork->sw_sync.timeline = sw_sync_timeline_create();
	fence = sw_sync_timeline_create_fence(cork->sw_sync.timeline, 1);

	return fence;
}

static void unplug_sync_fd(struct igt_cork *cork)
{
	sw_sync_timeline_inc(cork->sw_sync.timeline, 1);
	close(cork->sw_sync.timeline);
}

/**
 * igt_cork_plug:
 * @fd: open drm file descriptor
 * @method: method to utilize for corking.
 * @cork: structure that will be filled with the state of the cork bo.
 * Note: this has to match the corking method.
 *
 * This function provides a mechanism to stall submission. It provides two
 * blocking methods:
 *
 * VGEM_BO.
 * Imports a vgem bo with a fence attached to it. This bo can be used as a
 * dependency during submission to stall execution until the fence is signaled.
 *
 * SW_SYNC:
 * Creates a timeline and then a fence on that timeline. The fence can be used
 * as an input fence to a request, the request will be stalled until the fence
 * is signaled.
 *
 * The parameters required to unblock the execution and to cleanup are stored in
 * the provided cork structure.
 *
 * Returns:
 * Handle of the imported BO / Sw sync fence FD.
 */
uint32_t igt_cork_plug(struct igt_cork *cork, int fd)
{
	igt_assert(cork->fd == -1);

	switch (cork->type) {
	case CORK_SYNC_FD:
		return plug_sync_fd(cork);

	case CORK_VGEM_HANDLE:
		return plug_vgem_handle(cork, fd);

	default:
		igt_assert_f(0, "Invalid cork type!\n");
		return 0;
	}
}

/**
 * igt_cork_unplug:
 * @method: method to utilize for corking.
 * @cork: cork state from igt_cork_plug()
 *
 * This function unblocks the execution by signaling the fence attached to the
 * imported bo and does the necessary post-processing.
 *
 * NOTE: the handle returned by igt_cork_plug is not closed during this phase.
 */
void igt_cork_unplug(struct igt_cork *cork)
{
	igt_assert(cork->fd != -1);

	switch (cork->type) {
	case CORK_SYNC_FD:
		unplug_sync_fd(cork);
		break;

	case CORK_VGEM_HANDLE:
		unplug_vgem_handle(cork);
		break;

	default:
		igt_assert_f(0, "Invalid cork type!\n");
	}

	cork->fd = -1; /* Reset cork */
}
