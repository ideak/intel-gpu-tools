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

#include <sched.h>
#include <signal.h>
#include <sys/ioctl.h>

#include "i915/gem.h"
#include "igt.h"
#include "igt_rand.h"
#include "igt_vgem.h"

#define PAGE_ALIGN(x) ALIGN(x, 4096)

/* Exercise the busy-ioctl, ensuring the ABI is never broken */
IGT_TEST_DESCRIPTION("Basic check of busy-ioctl ABI.");

enum { TEST = 0, BUSY, BATCH };

static bool gem_busy(int fd, uint32_t handle)
{
	struct drm_i915_gem_busy busy;

	memset(&busy, 0, sizeof(busy));
	busy.handle = handle;

	do_ioctl(fd, DRM_IOCTL_I915_GEM_BUSY, &busy);

	return busy.busy != 0;
}

static void __gem_busy(int fd,
		       uint32_t handle,
		       uint32_t *read,
		       uint32_t *write)
{
	struct drm_i915_gem_busy busy;

	memset(&busy, 0, sizeof(busy));
	busy.handle = handle;

	do_ioctl(fd, DRM_IOCTL_I915_GEM_BUSY, &busy);

	*write = busy.busy & 0xffff;
	*read = busy.busy >> 16;
}

static bool exec_noop(int fd,
		      uint32_t *handles,
		      unsigned flags,
		      bool write)
{
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 exec[3];

	memset(exec, 0, sizeof(exec));
	exec[0].handle = handles[BUSY];
	exec[1].handle = handles[TEST];
	if (write)
		exec[1].flags |= EXEC_OBJECT_WRITE;
	exec[2].handle = handles[BATCH];

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(exec);
	execbuf.buffer_count = 3;
	execbuf.flags = flags;
	igt_debug("Queuing handle for %s on engine %d\n",
		  write ? "writing" : "reading", flags);
	return __gem_execbuf(fd, &execbuf) == 0;
}

static bool still_busy(int fd, uint32_t handle)
{
	uint32_t read, write;
	__gem_busy(fd, handle, &read, &write);
	return write;
}

static void semaphore(int fd, const struct intel_execution_engine2 *e)
{
	struct intel_execution_engine2 *__e;
	uint32_t bbe = MI_BATCH_BUFFER_END;
	igt_spin_t *spin;
	uint32_t handle[3];
	uint32_t read, write;
	uint32_t active;
	unsigned i;

	handle[TEST] = gem_create(fd, 4096);
	handle[BATCH] = gem_create(fd, 4096);
	gem_write(fd, handle[BATCH], 0, &bbe, sizeof(bbe));

	/* Create a long running batch which we can use to hog the GPU */
	handle[BUSY] = gem_create(fd, 4096);
	spin = igt_spin_new(fd,
			    .engine = e->flags,
			    .dependency = handle[BUSY]);

	/* Queue a batch after the busy, it should block and remain "busy" */
	igt_assert(exec_noop(fd, handle, e->flags, false));
	igt_assert(still_busy(fd, handle[BUSY]));
	__gem_busy(fd, handle[TEST], &read, &write);
	igt_assert_eq(read, 1 << e->class);
	igt_assert_eq(write, 0);

	/* Requeue with a write */
	igt_assert(exec_noop(fd, handle, e->flags, true));
	igt_assert(still_busy(fd, handle[BUSY]));
	__gem_busy(fd, handle[TEST], &read, &write);
	igt_assert_eq(read, 1 << e->class);
	igt_assert_eq(write, 1 + e->class);

	/* Now queue it for a read across all available rings */
	active = 0;
	__for_each_physical_engine(fd, __e) {
		if (exec_noop(fd, handle, __e->flags, false))
			active |= 1 << __e->class;
	}
	igt_assert(still_busy(fd, handle[BUSY]));
	__gem_busy(fd, handle[TEST], &read, &write);
	igt_assert_eq(read, active);
	igt_assert_eq(write, 1 + e->class); /* from the earlier write */

	/* Check that our long batch was long enough */
	igt_assert(still_busy(fd, handle[BUSY]));
	igt_spin_free(fd, spin);

	/* And make sure it becomes idle again */
	gem_sync(fd, handle[TEST]);
	__gem_busy(fd, handle[TEST], &read, &write);
	igt_assert_eq(read, 0);
	igt_assert_eq(write, 0);

	for (i = TEST; i <= BATCH; i++)
		gem_close(fd, handle[i]);
}

#define PARALLEL 1
#define HANG 2
static void one(int fd, const struct intel_execution_engine2 *e, unsigned test_flags)
{
	uint32_t scratch = gem_create(fd, 4096);
	uint32_t read[2], write[2];
	enum { READ, WRITE };
	struct timespec tv;
	igt_spin_t *spin;
	int timeout;

	spin = igt_spin_new(fd,
			    .engine = e->flags,
			    .dependency = scratch,
			    .flags = (test_flags & HANG) ? IGT_SPIN_NO_PREEMPTION : 0);

	__gem_busy(fd, scratch, &read[WRITE], &write[WRITE]);
	__gem_busy(fd, spin->handle, &read[READ], &write[READ]);

	if (test_flags & PARALLEL) {
		struct intel_execution_engine2 *e2;

		__for_each_physical_engine(fd, e2) {
			if (e2->class == e->class &&
			    e2->instance == e->instance)
				continue;

			igt_debug("Testing %s in parallel\n", e2->name);
			one(fd, e2, 0);
		}
	}

	timeout = 120;
	if ((test_flags & HANG) == 0)
		igt_spin_end(spin);

	igt_assert_eq(write[WRITE], 1 + e->class);
	igt_assert_eq_u32(read[WRITE], 1 << e->class);

	/*
	 * We do not expect the batch to be in a modified state, but
	 * if we are using GPU relocations then it will indeed be marked
	 * as written to by the GPU. We may use any engine to update the
	 * relocations.
	 *
	 * If we just used CPU relocations, we could
	 *   igt_assert_eq(write[READ], 0);
	 * since we do not, we have to massage the busyness to remove the trace
	 * of GPU relocation.
	 */
	if (write[READ] && write[READ] != 1 + e->class)
		/* Inter-engine GPU relocation! */
		read[READ] &= ~(1 << (write[READ] - 1));
	igt_assert_eq_u32(read[READ], 1 << e->class);

	/* Calling busy in a loop should be enough to flush the rendering */
	memset(&tv, 0, sizeof(tv));
	while (gem_busy(fd, spin->handle))
		igt_assert(igt_seconds_elapsed(&tv) < timeout);
	igt_assert(!gem_busy(fd, scratch));

	igt_spin_free(fd, spin);
	gem_close(fd, scratch);
}

static void xchg_u32(void *array, unsigned i, unsigned j)
{
	uint32_t *u32 = array;
	uint32_t tmp = u32[i];
	u32[i] = u32[j];
	u32[j] = tmp;
}

static void close_race(int fd)
{
	const unsigned int ncpus = sysconf(_SC_NPROCESSORS_ONLN);
	const unsigned int nhandles = gem_submission_measure(fd, ALL_ENGINES);
	unsigned int engines[I915_EXEC_RING_MASK + 1], nengine;
	const struct intel_execution_engine2 *e;
	unsigned long *control;
	uint32_t *handles;
	int i;

	igt_require(ncpus > 1);
	intel_require_memory(nhandles, 4096, CHECK_RAM);

	/*
	 * One thread spawning work and randomly closing handles.
	 * One background thread per cpu checking busyness.
	 */

	nengine = 0;
	__for_each_physical_engine(fd, e)
		engines[nengine++] = e->flags;
	igt_require(nengine);

	control = mmap(NULL, 4096, PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
	igt_assert(control != MAP_FAILED);

	handles = mmap(NULL, PAGE_ALIGN(nhandles*sizeof(*handles)),
		   PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
	igt_assert(handles != MAP_FAILED);

	igt_fork(child, ncpus - 1) {
		struct drm_i915_gem_busy busy;
		uint32_t indirection[nhandles];
		unsigned long count = 0;

		for (i = 0; i < nhandles; i++)
			indirection[i] = i;

		hars_petruska_f54_1_random_perturb(child);

		memset(&busy, 0, sizeof(busy));
		do {
			igt_permute_array(indirection, nhandles, xchg_u32);
			__sync_synchronize();
			for (i = 0; i < nhandles; i++) {
				busy.handle = handles[indirection[i]];
				/* Check that the busy computation doesn't
				 * explode in the face of random gem_close().
				 */
				drmIoctl(fd, DRM_IOCTL_I915_GEM_BUSY, &busy);
			}
			count++;
		} while(*(volatile long *)control == 0);

		igt_debug("child[%d]: count = %lu\n", child, count);
		control[child + 1] = count;
	}

	igt_fork(child, 1) {
		struct sched_param rt = {.sched_priority = 99 };
		igt_spin_t *spin[nhandles];
		unsigned long count = 0;

		igt_assert(sched_setscheduler(getpid(), SCHED_RR, &rt) == 0);

		for (i = 0; i < nhandles; i++) {
			spin[i] = __igt_spin_new(fd,
						 .engine = engines[rand() % nengine]);
			handles[i] = spin[i]->handle;
		}

		igt_until_timeout(20) {
			for (i = 0; i < nhandles; i++) {
				igt_spin_free(fd, spin[i]);
				spin[i] = __igt_spin_new(fd,
							 .engine = engines[rand() % nengine]);
				handles[i] = spin[i]->handle;
				__sync_synchronize();
			}
			count += nhandles;
		}
		control[0] = count;
		__sync_synchronize();

		for (i = 0; i < nhandles; i++)
			igt_spin_free(fd, spin[i]);
	}
	igt_waitchildren();

	for (i = 0; i < ncpus - 1; i++)
		control[ncpus] += control[i + 1];
	igt_info("Total execs %lu, busy-ioctls %lu\n",
		 control[0], control[ncpus] * nhandles);

	munmap(handles, PAGE_ALIGN(nhandles * sizeof(*handles)));
	munmap(control, 4096);

	gem_quiescent_gpu(fd);
}

static bool has_semaphores(int fd)
{
	struct drm_i915_getparam gp;
	int val = -1;

	memset(&gp, 0, sizeof(gp));
	gp.param = I915_PARAM_HAS_SEMAPHORES;
	gp.value = &val;

	drmIoctl(fd, DRM_IOCTL_I915_GETPARAM, &gp);
	errno = 0;

	return val > 0;
}

static bool has_extended_busy_ioctl(int fd)
{
	igt_spin_t *spin = igt_spin_new(fd, .engine = I915_EXEC_DEFAULT);
	uint32_t read, write;

	__gem_busy(fd, spin->handle, &read, &write);
	igt_spin_free(fd, spin);

	return read != 0;
}

static void basic(int fd, const struct intel_execution_engine2 *e, unsigned flags)
{
	igt_spin_t *spin =
		igt_spin_new(fd,
			     .engine = e->flags,
			     .flags = flags & HANG ?
			     IGT_SPIN_NO_PREEMPTION | IGT_SPIN_INVALID_CS : 0);
	struct timespec tv;
	int timeout;

	timeout = 120;
	if ((flags & HANG) == 0) {
		igt_spin_end(spin);
		timeout = 1;
	}

	memset(&tv, 0, sizeof(tv));
	while (gem_bo_busy(fd, spin->handle)) {
		if (igt_seconds_elapsed(&tv) > timeout) {
			igt_debugfs_dump(fd, "i915_engine_info");
			igt_assert_f(igt_seconds_elapsed(&tv) < timeout,
				     "%s batch did not complete within %ds\n",
				     flags & HANG ? "Hanging" : "Normal",
				     timeout);
		}
	}

	igt_spin_free(fd, spin);
}

static void all(int i915)
{
	const struct intel_execution_engine2 *e;

	__for_each_physical_engine(i915, e)
		igt_fork(child, 1) basic(i915, e, 0);
	igt_waitchildren();
}

#define test_each_engine(T, i915, e) \
	igt_subtest_with_dynamic(T) __for_each_physical_engine(i915, e) \
		igt_dynamic_f("%s", (e)->name)

#define test_each_engine_store(T, i915, e) \
	igt_subtest_with_dynamic(T) __for_each_physical_engine(i915, e) \
		for_each_if (gem_class_can_store_dword(i915, (e)->class)) \
			igt_dynamic_f("%s", (e)->name)

igt_main
{
	const struct intel_execution_engine2 *e;
	int fd = -1;

	igt_fixture {
		fd = drm_open_driver_master(DRIVER_INTEL);
		igt_require_gem(fd);
	}

	igt_subtest_group {
		igt_fixture {
			igt_fork_hang_detector(fd);
		}

		igt_subtest_with_dynamic("busy") {
			igt_dynamic("all") {
				gem_quiescent_gpu(fd);
				all(fd);
			}

			__for_each_physical_engine(fd, e) {
				igt_dynamic_f("%s", e->name) {
					gem_quiescent_gpu(fd);
					basic(fd, e, 0);
				}
			}
		}

		igt_subtest_group {
			igt_fixture {
				igt_require(has_extended_busy_ioctl(fd));
				gem_require_mmap_wc(fd);
			}

			test_each_engine_store("extended", fd, e) {
				gem_quiescent_gpu(fd);
				one(fd, e, 0);
				gem_quiescent_gpu(fd);
			}

			test_each_engine_store("parallel", fd, e) {
				gem_quiescent_gpu(fd);
				one(fd, e, PARALLEL);
				gem_quiescent_gpu(fd);
			}
		}

		igt_subtest_group {
			igt_fixture {
				igt_require(has_extended_busy_ioctl(fd));
				igt_require(has_semaphores(fd));
			}

			test_each_engine("semaphore", fd, e) {
				gem_quiescent_gpu(fd);
				semaphore(fd, e);
				gem_quiescent_gpu(fd);
			}
		}

		igt_subtest("close-race")
			close_race(fd);

		igt_fixture {
			igt_stop_hang_detector();
		}
	}

	igt_subtest_group {
		igt_hang_t hang;

		igt_fixture {
			hang = igt_allow_hang(fd, 0, 0);
		}

		test_each_engine("hang", fd, e) {
			gem_quiescent_gpu(fd);
			basic(fd, e, HANG);
			gem_quiescent_gpu(fd);
		}

		igt_subtest_group {
			igt_fixture {
				igt_require(has_extended_busy_ioctl(fd));
				gem_require_mmap_wc(fd);
			}

			test_each_engine_store("hang-extended", fd, e) {
				gem_quiescent_gpu(fd);
				one(fd, e, HANG);
				gem_quiescent_gpu(fd);
			}
		}

		igt_fixture {
			igt_disallow_hang(fd, hang);
		}
	}

	igt_fixture {
		close(fd);
	}
}
