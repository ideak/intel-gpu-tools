/*
 * Copyright © 2011 Intel Corporation
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
 *    Chris Wilson <chris@chris-wilson.co.uk>
 *
 */

#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <inttypes.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <sys/time.h>
#include <time.h>

#include "drm.h"
#include "i915/gem.h"
#include "i915/gem_create.h"
#include "igt.h"
#include "igt_device.h"
#include "igt_rand.h"
#include "igt_sysfs.h"


#define ENGINE_FLAGS  (I915_EXEC_RING_MASK | I915_EXEC_BSD_MASK)

#define MAX_PRIO I915_CONTEXT_MAX_USER_PRIORITY
#define MIN_PRIO I915_CONTEXT_MIN_USER_PRIORITY
#define MAX_ENGINES (I915_EXEC_RING_MASK + 1)

#define FORKED (1 << 0)
#define CONTEXT (1 << 1)

static double elapsed(const struct timespec *start, const struct timespec *end)
{
	return ((end->tv_sec - start->tv_sec) +
		(end->tv_nsec - start->tv_nsec)*1e-9);
}

static double nop_on_ring(int fd, uint32_t handle,
			  const struct intel_execution_engine2 *e,
			  int timeout_ms,
			  unsigned long *out)
{
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 obj;
	struct timespec start, now;
	unsigned long count;

	memset(&obj, 0, sizeof(obj));
	obj.handle = handle;

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(&obj);
	execbuf.buffer_count = 1;
	execbuf.flags = e->flags;
	execbuf.flags |= I915_EXEC_HANDLE_LUT;
	execbuf.flags |= I915_EXEC_NO_RELOC;
	if (__gem_execbuf(fd, &execbuf)) {
		execbuf.flags = e->flags;
		gem_execbuf(fd, &execbuf);
	}
	intel_detect_and_clear_missed_interrupts(fd);

	count = 0;
	clock_gettime(CLOCK_MONOTONIC, &start);
	do {
		gem_execbuf(fd, &execbuf);
		count++;

		clock_gettime(CLOCK_MONOTONIC, &now);
	} while (elapsed(&start, &now) < timeout_ms * 1e-3);
	igt_assert_eq(intel_detect_and_clear_missed_interrupts(fd), 0);

	*out = count;
	return elapsed(&start, &now);
}

static void poll_ring(int fd, const struct intel_execution_engine2 *e,
		      int timeout)
{
	const unsigned int gen = intel_gen(intel_get_drm_devid(fd));
	const uint32_t MI_ARB_CHK = 0x5 << 23;
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 obj;
	struct drm_i915_gem_relocation_entry reloc[4], *r;
	uint32_t *bbe[2], *state, *batch;
	struct timespec tv = {};
	unsigned long cycles;
	unsigned flags;
	uint64_t elapsed;

	flags = I915_EXEC_NO_RELOC;
	if (gen == 4 || gen == 5)
		flags |= I915_EXEC_SECURE;

	igt_require(gem_class_can_store_dword(fd, e->class));
	igt_require(gem_class_has_mutable_submission(fd, e->class));

	memset(&obj, 0, sizeof(obj));
	obj.handle = gem_create(fd, 4096);
	obj.relocs_ptr = to_user_pointer(reloc);
	obj.relocation_count = ARRAY_SIZE(reloc);

	r = memset(reloc, 0, sizeof(reloc));
	batch = gem_mmap__wc(fd, obj.handle, 0, 4096, PROT_WRITE);

	for (unsigned int start_offset = 0;
	     start_offset <= 128;
	     start_offset += 128) {
		uint32_t *b = batch + start_offset / sizeof(*batch);

		r->target_handle = obj.handle;
		r->offset = (b - batch + 1) * sizeof(uint32_t);
		r->delta = 4092;
		r->read_domains = I915_GEM_DOMAIN_RENDER;

		*b = MI_STORE_DWORD_IMM | (gen < 6 ? 1 << 22 : 0);
		if (gen >= 8) {
			*++b = r->delta;
			*++b = 0;
		} else if (gen >= 4) {
			r->offset += sizeof(uint32_t);
			*++b = 0;
			*++b = r->delta;
		} else {
			*b -= 1;
			*++b = r->delta;
		}
		*++b = start_offset != 0;
		r++;

		b = batch + (start_offset + 64) / sizeof(*batch);
		bbe[start_offset != 0] = b;
		*b++ = MI_ARB_CHK;

		r->target_handle = obj.handle;
		r->offset = (b - batch + 1) * sizeof(uint32_t);
		r->read_domains = I915_GEM_DOMAIN_COMMAND;
		r->delta = start_offset + 64;
		if (gen >= 8) {
			*b++ = MI_BATCH_BUFFER_START | 1 << 8 | 1;
			*b++ = r->delta;
			*b++ = 0;
		} else if (gen >= 6) {
			*b++ = MI_BATCH_BUFFER_START | 1 << 8;
			*b++ = r->delta;
		} else {
			*b++ = MI_BATCH_BUFFER_START | 2 << 6;
			if (gen < 4)
				r->delta |= 1;
			*b++ = r->delta;
		}
		r++;
	}
	igt_assert(r == reloc + ARRAY_SIZE(reloc));
	state = batch + 1023;

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(&obj);
	execbuf.buffer_count = 1;
	execbuf.flags = e->flags | flags;

	cycles = 0;
	do {
		unsigned int idx = ++cycles & 1;

		*bbe[idx] = MI_ARB_CHK;
		execbuf.batch_start_offset =
			(bbe[idx] - batch) * sizeof(*batch) - 64;

		gem_execbuf(fd, &execbuf);

		*bbe[!idx] = MI_BATCH_BUFFER_END;
		__sync_synchronize();

		while (READ_ONCE(*state) != idx)
			;
	} while ((elapsed = igt_nsec_elapsed(&tv)) >> 30 < timeout);
	*bbe[cycles & 1] = MI_BATCH_BUFFER_END;
	gem_sync(fd, obj.handle);

	igt_info("%s completed %ld cycles: %.3f us\n",
		 e->name, cycles, elapsed*1e-3/cycles);

	munmap(batch, 4096);
	gem_close(fd, obj.handle);
}

static void poll_sequential(int fd, const char *name, int timeout)
{
	const unsigned int gen = intel_gen(intel_get_drm_devid(fd));
	const struct intel_execution_engine2 *e;
	const uint32_t MI_ARB_CHK = 0x5 << 23;
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 obj[2];
	struct drm_i915_gem_relocation_entry reloc[4], *r;
	uint32_t *bbe[2], *state, *batch;
	unsigned engines[MAX_ENGINES], nengine, flags;
	struct timespec tv = {};
	unsigned long cycles;
	uint64_t elapsed;
	bool cached;

	flags = I915_EXEC_NO_RELOC;
	if (gen == 4 || gen == 5)
		flags |= I915_EXEC_SECURE;

	nengine = 0;
	__for_each_physical_engine(fd, e) {
		if (!gem_class_can_store_dword(fd, e->class) ||
		    !gem_class_has_mutable_submission(fd, e->class))
			continue;

		engines[nengine++] = e->flags;
	}

	igt_require(nengine);

	memset(obj, 0, sizeof(obj));
	obj[0].handle = gem_create(fd, 4096);
	obj[0].flags = EXEC_OBJECT_WRITE;
	cached = __gem_set_caching(fd, obj[0].handle, 1) == 0;
	obj[1].handle = gem_create(fd, 4096);
	obj[1].relocs_ptr = to_user_pointer(reloc);
	obj[1].relocation_count = ARRAY_SIZE(reloc);

	r = memset(reloc, 0, sizeof(reloc));
	batch = gem_mmap__wc(fd, obj[1].handle, 0, 4096, PROT_WRITE);

	for (unsigned int start_offset = 0;
	     start_offset <= 128;
	     start_offset += 128) {
		uint32_t *b = batch + start_offset / sizeof(*batch);

		r->target_handle = obj[0].handle;
		r->offset = (b - batch + 1) * sizeof(uint32_t);
		r->delta = 0;
		r->read_domains = I915_GEM_DOMAIN_RENDER;
		r->write_domain = I915_GEM_DOMAIN_RENDER;

		*b = MI_STORE_DWORD_IMM | (gen < 6 ? 1 << 22 : 0);
		if (gen >= 8) {
			*++b = r->delta;
			*++b = 0;
		} else if (gen >= 4) {
			r->offset += sizeof(uint32_t);
			*++b = 0;
			*++b = r->delta;
		} else {
			*b -= 1;
			*++b = r->delta;
		}
		*++b = start_offset != 0;
		r++;

		b = batch + (start_offset + 64) / sizeof(*batch);
		bbe[start_offset != 0] = b;
		*b++ = MI_ARB_CHK;

		r->target_handle = obj[1].handle;
		r->offset = (b - batch + 1) * sizeof(uint32_t);
		r->read_domains = I915_GEM_DOMAIN_COMMAND;
		r->delta = start_offset + 64;
		if (gen >= 8) {
			*b++ = MI_BATCH_BUFFER_START | 1 << 8 | 1;
			*b++ = r->delta;
			*b++ = 0;
		} else if (gen >= 6) {
			*b++ = MI_BATCH_BUFFER_START | 1 << 8;
			*b++ = r->delta;
		} else {
			*b++ = MI_BATCH_BUFFER_START | 2 << 6;
			if (gen < 4)
				r->delta |= 1;
			*b++ = r->delta;
		}
		r++;
	}
	igt_assert(r == reloc + ARRAY_SIZE(reloc));

	if (cached)
		state = gem_mmap__cpu(fd, obj[0].handle, 0, 4096, PROT_READ);
	else
		state = gem_mmap__wc(fd, obj[0].handle, 0, 4096, PROT_READ);

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(obj);
	execbuf.buffer_count = ARRAY_SIZE(obj);

	cycles = 0;
	do {
		unsigned int idx = ++cycles & 1;

		*bbe[idx] = MI_ARB_CHK;
		execbuf.batch_start_offset =
			(bbe[idx] - batch) * sizeof(*batch) - 64;

		execbuf.flags = engines[cycles % nengine] | flags;
		gem_execbuf(fd, &execbuf);

		*bbe[!idx] = MI_BATCH_BUFFER_END;
		__sync_synchronize();

		while (READ_ONCE(*state) != idx)
			;
	} while ((elapsed = igt_nsec_elapsed(&tv)) >> 30 < timeout);
	*bbe[cycles & 1] = MI_BATCH_BUFFER_END;
	gem_sync(fd, obj[1].handle);

	igt_info("%s completed %ld cycles: %.3f us\n",
		 name, cycles, elapsed*1e-3/cycles);

	munmap(state, 4096);
	munmap(batch, 4096);
	gem_close(fd, obj[1].handle);
	gem_close(fd, obj[0].handle);
}

static void single(int fd, uint32_t handle,
		   const struct intel_execution_engine2 *e)
{
	double time;
	unsigned long count;

	time = nop_on_ring(fd, handle, e, 20000, &count);
	igt_info("%s: %'lu cycles: %.3fus\n",
		  e->name, count, time*1e6 / count);
}

static double
stable_nop_on_ring(int fd, uint32_t handle,
		   const struct intel_execution_engine2 *e,
		   int timeout_ms,
		   int reps)
{
	igt_stats_t s;
	double n;

	igt_assert(reps >= 5);

	igt_stats_init_with_size(&s, reps);
	s.is_float = true;

	while (reps--) {
		unsigned long count;
		double time;

		time = nop_on_ring(fd, handle, e, timeout_ms, &count);
		igt_stats_push_float(&s, time / count);
	}

	n = igt_stats_get_median(&s);
	igt_stats_fini(&s);

	return n;
}

#define assert_within_epsilon(x, ref, tolerance) \
        igt_assert_f((x) <= (1.0 + tolerance) * ref && \
                     (x) >= (1.0 - tolerance) * ref, \
                     "'%s' != '%s' (%f not within %f%% tolerance of %f)\n",\
                     #x, #ref, x, tolerance * 100.0, ref)

static void headless(int fd, uint32_t handle,
		     const struct intel_execution_engine2 *e)
{
	unsigned int nr_connected = 0;
	double n_display, n_headless;
	drmModeConnector *connector;
	unsigned long count;
	drmModeRes *res;

	res = drmModeGetResources(fd);
	igt_require(res);

	/* require at least one connected connector for the test */
	for (int i = 0; i < res->count_connectors; i++) {
		connector = drmModeGetConnectorCurrent(fd, res->connectors[i]);
		if (connector->connection == DRM_MODE_CONNECTED)
			nr_connected++;
		drmModeFreeConnector(connector);
	}
	igt_require(nr_connected > 0);

	/* set graphics mode to prevent blanking */
	kmstest_set_vt_graphics_mode();

	nop_on_ring(fd, handle, e, 10, &count);
	igt_require_f(count > 100, "submillisecond precision required\n");

	/* benchmark nops */
	n_display = stable_nop_on_ring(fd, handle, e, 500, 5);
	igt_info("With one display connected: %.2fus\n",
		 n_display * 1e6);

	/* force all connectors off */
	kmstest_unset_all_crtcs(fd, res);

	/* benchmark nops again */
	n_headless = stable_nop_on_ring(fd, handle, e, 500, 5);
	igt_info("Without a display connected (headless): %.2fus\n",
		 n_headless * 1e6);

	/* check that the two execution speeds are roughly the same */
	assert_within_epsilon(n_headless, n_display, 0.1f);
}

static void parallel(int fd, uint32_t handle, int timeout)
{
	const struct intel_execution_engine2 *e;
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 obj;
	unsigned engines[MAX_ENGINES];
	char *names[MAX_ENGINES];
	unsigned nengine;
	unsigned long count;
	double time, sum;

	sum = 0;
	nengine = 0;

	__for_each_physical_engine(fd, e) {
		engines[nengine] = e->flags;
		names[nengine++] = strdup(e->name);

		time = nop_on_ring(fd, handle, e, 250, &count) / count;
		sum += time;
		igt_debug("%s: %.3fus\n", e->name, 1e6*time);
	}
	igt_require(nengine);
	igt_info("average (individually): %.3fus\n", sum/nengine*1e6);

	memset(&obj, 0, sizeof(obj));
	obj.handle = handle;

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(&obj);
	execbuf.buffer_count = 1;
	execbuf.flags |= I915_EXEC_HANDLE_LUT;
	execbuf.flags |= I915_EXEC_NO_RELOC;
	if (__gem_execbuf(fd, &execbuf)) {
		execbuf.flags = 0;
		gem_execbuf(fd, &execbuf);
	}
	intel_detect_and_clear_missed_interrupts(fd);

	igt_fork(child, nengine) {
		struct timespec start, now;

		execbuf.flags &= ~ENGINE_FLAGS;
		execbuf.flags |= engines[child];

		count = 0;
		clock_gettime(CLOCK_MONOTONIC, &start);
		do {
			gem_execbuf(fd, &execbuf);
			count++;

			clock_gettime(CLOCK_MONOTONIC, &now);
		} while (elapsed(&start, &now) < timeout);
		time = elapsed(&start, &now) / count;
		igt_info("%s: %ld cycles, %.3fus\n", names[child], count, 1e6*time);
	}
	while (nengine--)
		free(names[nengine]);

	igt_waitchildren();
	igt_assert_eq(intel_detect_and_clear_missed_interrupts(fd), 0);
}

static void independent(int fd, uint32_t handle, int timeout)
{
	const struct intel_execution_engine2 *e;
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 obj;
	unsigned engines[MAX_ENGINES];
	char *names[MAX_ENGINES];
	unsigned nengine;
	unsigned long count;
	double time, sum;

	sum = 0;
	nengine = 0;
	__for_each_physical_engine(fd, e) {
		engines[nengine] = e->flags;
		names[nengine++] = strdup(e->name);

		time = nop_on_ring(fd, handle, e, 250, &count) / count;
		sum += time;
		igt_debug("%s: %.3fus\n", e->name, 1e6*time);
	}
	igt_require(nengine);
	igt_info("average (individually): %.3fus\n", sum/nengine*1e6);

	memset(&obj, 0, sizeof(obj));
	obj.handle = handle;

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(&obj);
	execbuf.buffer_count = 1;
	execbuf.flags |= I915_EXEC_HANDLE_LUT;
	execbuf.flags |= I915_EXEC_NO_RELOC;
	if (__gem_execbuf(fd, &execbuf)) {
		execbuf.flags = 0;
		gem_execbuf(fd, &execbuf);
	}
	intel_detect_and_clear_missed_interrupts(fd);

	igt_fork(child, nengine) {
		const uint32_t bbe = MI_BATCH_BUFFER_END;
		struct timespec start, now;

		obj.handle = gem_create(fd, 4096);
		gem_write(fd, obj.handle, 0, &bbe, sizeof(bbe));

		execbuf.flags &= ~ENGINE_FLAGS;
		execbuf.flags |= engines[child];

		count = 0;
		clock_gettime(CLOCK_MONOTONIC, &start);
		do {
			gem_execbuf(fd, &execbuf);
			count++;

			clock_gettime(CLOCK_MONOTONIC, &now);
		} while (elapsed(&start, &now) < timeout);
		time = elapsed(&start, &now) / count;
		igt_info("%s: %ld cycles, %.3fus\n", names[child], count, 1e6*time);

		gem_close(fd, obj.handle);
	}
	while (nengine--)
		free(names[nengine]);

	igt_waitchildren();
	igt_assert_eq(intel_detect_and_clear_missed_interrupts(fd), 0);
}

static void multiple(int fd,
		     const struct intel_execution_engine2 *e,
		     int timeout)
{
	const int ncpus = sysconf(_SC_NPROCESSORS_ONLN);
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 obj;

	memset(&obj, 0, sizeof(obj));
	obj.handle = gem_create(fd, 4096);
	gem_write(fd, obj.handle, 0, &bbe, sizeof(bbe));

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(&obj);
	execbuf.buffer_count = 1;
	execbuf.flags = e->flags;
	execbuf.flags |= I915_EXEC_HANDLE_LUT;
	execbuf.flags |= I915_EXEC_NO_RELOC;
	if (__gem_execbuf(fd, &execbuf)) {
		execbuf.flags = e->flags;
		gem_execbuf(fd, &execbuf);
	}
	intel_detect_and_clear_missed_interrupts(fd);

	igt_fork(child, ncpus) {
		struct timespec start, now;
		unsigned long count;
		double time;
		int i915;

		i915 = gem_reopen_driver(fd);
		gem_context_copy_engines(fd, 0, i915, 0);

		obj.handle = gem_create(i915, 4096);
		gem_write(i915, obj.handle, 0, &bbe, sizeof(bbe));

		count = 0;
		clock_gettime(CLOCK_MONOTONIC, &start);
		do {
			gem_execbuf(i915, &execbuf);
			count++;

			clock_gettime(CLOCK_MONOTONIC, &now);
		} while (elapsed(&start, &now) < timeout);
		time = elapsed(&start, &now) / count;
		igt_info("%d: %ld cycles, %.3fus\n", child, count, 1e6*time);
	}

	igt_waitchildren();
	igt_assert_eq(intel_detect_and_clear_missed_interrupts(fd), 0);

	gem_close(fd, obj.handle);
}

static void series(int fd, uint32_t handle, int timeout)
{
	const struct intel_execution_engine2 *e;
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 obj;
	struct timespec start, now, sync;
	unsigned engines[MAX_ENGINES];
	unsigned nengine;
	unsigned long count;
	double time, max = 0, min = HUGE_VAL, sum = 0;
	const char *name;

	nengine = 0;
	__for_each_physical_engine(fd, e) {
		time = nop_on_ring(fd, handle, e, 250, &count) / count;
		if (time > max) {
			name = e->name;
			max = time;
		}
		if (time < min)
			min = time;
		sum += time;
		engines[nengine++] = e->flags;
	}
	igt_require(nengine);
	igt_info("Maximum execution latency on %s, %.3fus, min %.3fus, total %.3fus per cycle, average %.3fus\n",
		 name, max*1e6, min*1e6, sum*1e6, sum/nengine*1e6);

	memset(&obj, 0, sizeof(obj));
	obj.handle = handle;

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(&obj);
	execbuf.buffer_count = 1;
	execbuf.flags |= I915_EXEC_HANDLE_LUT;
	execbuf.flags |= I915_EXEC_NO_RELOC;
	if (__gem_execbuf(fd, &execbuf)) {
		execbuf.flags = 0;
		gem_execbuf(fd, &execbuf);
	}
	intel_detect_and_clear_missed_interrupts(fd);

	count = 0;
	clock_gettime(CLOCK_MONOTONIC, &start);
	do {
		for (int n = 0; n < nengine; n++) {
			execbuf.flags &= ~ENGINE_FLAGS;
			execbuf.flags |= engines[n];
			gem_execbuf(fd, &execbuf);
		}
		count += nengine;
		clock_gettime(CLOCK_MONOTONIC, &now);
	} while (elapsed(&start, &now) < timeout); /* Hang detection ~120s */
	gem_sync(fd, handle);
	clock_gettime(CLOCK_MONOTONIC, &sync);
	igt_debug("sync time: %.3fus\n", elapsed(&now, &sync)*1e6);
	igt_assert_eq(intel_detect_and_clear_missed_interrupts(fd), 0);

	time = elapsed(&start, &now) / count;
	igt_info("All (%d engines): %'lu cycles, average %.3fus per cycle [expected %.3fus]\n",
		 nengine, count, 1e6*time, 1e6*((max-min)/nengine+min));
}

static void xchg(void *array, unsigned i, unsigned j)
{
	unsigned *u = array;
	unsigned tmp = u[i];
	u[i] = u[j];
	u[j] = tmp;
}

static void sequential(int fd, uint32_t handle, unsigned flags, int timeout)
{
	const int ncpus = flags & FORKED ? sysconf(_SC_NPROCESSORS_ONLN) : 1;
	const struct intel_execution_engine2 *e;
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 obj[2];
	unsigned engines[MAX_ENGINES];
	unsigned nengine;
	double *results;
	double time, sum;
	unsigned n;

	gem_require_contexts(fd);

	results = mmap(NULL, 4096, PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
	igt_assert(results != MAP_FAILED);

	nengine = 0;
	sum = 0;
	__for_each_physical_engine(fd, e) {
		unsigned long count;

		time = nop_on_ring(fd, handle, e, 250, &count) / count;
		sum += time;
		igt_debug("%s: %.3fus\n", e->name, 1e6*time);

		engines[nengine++] = e->flags;
	}
	igt_require(nengine);
	igt_info("Total (individual) execution latency %.3fus per cycle\n",
		 1e6*sum);

	memset(obj, 0, sizeof(obj));
	obj[0].handle = gem_create(fd, 4096);
	obj[0].flags = EXEC_OBJECT_WRITE;
	obj[1].handle = handle;

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(obj);
	execbuf.buffer_count = 2;
	execbuf.flags |= I915_EXEC_HANDLE_LUT;
	execbuf.flags |= I915_EXEC_NO_RELOC;
	igt_require(__gem_execbuf(fd, &execbuf) == 0);

	if (flags & CONTEXT) {
		gem_require_contexts(fd);
		execbuf.rsvd1 = gem_context_clone_with_engines(fd, 0);
	}

	for (n = 0; n < nengine; n++) {
		execbuf.flags &= ~ENGINE_FLAGS;
		execbuf.flags |= engines[n];
		igt_require(__gem_execbuf(fd, &execbuf) == 0);
	}

	intel_detect_and_clear_missed_interrupts(fd);

	igt_fork(child, ncpus) {
		struct timespec start, now;
		unsigned long count;

		obj[0].handle = gem_create(fd, 4096);
		gem_execbuf(fd, &execbuf);

		if (flags & CONTEXT) {
			gem_require_contexts(fd);
			execbuf.rsvd1 = gem_context_clone_with_engines(fd, 0);
		}

		hars_petruska_f54_1_random_perturb(child);

		count = 0;
		clock_gettime(CLOCK_MONOTONIC, &start);
		do {
			igt_permute_array(engines, nengine, xchg);
			for (n = 0; n < nengine; n++) {
				execbuf.flags &= ~ENGINE_FLAGS;
				execbuf.flags |= engines[n];
				gem_execbuf(fd, &execbuf);
			}
			count++;
			clock_gettime(CLOCK_MONOTONIC, &now);
		} while (elapsed(&start, &now) < timeout);

		gem_sync(fd, obj[0].handle);
		clock_gettime(CLOCK_MONOTONIC, &now);
		results[child] = elapsed(&start, &now) / count;

		if (flags & CONTEXT)
			gem_context_destroy(fd, execbuf.rsvd1);

		gem_close(fd, obj[0].handle);
	}
	igt_waitchildren();
	igt_assert_eq(intel_detect_and_clear_missed_interrupts(fd), 0);

	results[ncpus] = 0;
	for (n = 0; n < ncpus; n++)
		results[ncpus] += results[n];
	results[ncpus] /= ncpus;

	igt_info("Sequential (%d engines, %d processes): average %.3fus per cycle [expected %.3fus]\n",
		 nengine, ncpus, 1e6*results[ncpus], 1e6*sum*ncpus);

	if (flags & CONTEXT)
		gem_context_destroy(fd, execbuf.rsvd1);

	gem_close(fd, obj[0].handle);
	munmap(results, 4096);
}

static bool fence_enable_signaling(int fence)
{
	return poll(&(struct pollfd){fence, POLLIN}, 1, 0) == 0;
}

static bool fence_wait(int fence)
{
	return poll(&(struct pollfd){fence, POLLIN}, 1, -1) == 1;
}

static void fence_signal(int fd, uint32_t handle,
			 const struct intel_execution_engine2 *ring_id,
			 const char *ring_name, int timeout)
{
#define NFENCES 512
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 obj;
	struct intel_execution_engine2 *__e;
	struct timespec start, now;
	unsigned engines[MAX_ENGINES];
	unsigned nengine;
	int *fences, n;
	unsigned long count, signal;

	igt_require(gem_has_exec_fence(fd));

	nengine = 0;
	if (!ring_id) {
		__for_each_physical_engine(fd, __e)
			engines[nengine++] = __e->flags;
	} else {
		engines[nengine++] = ring_id->flags;
	}
	igt_require(nengine);

	fences = malloc(sizeof(*fences) * NFENCES);
	igt_assert(fences);
	memset(fences, -1, sizeof(*fences) * NFENCES);

	memset(&obj, 0, sizeof(obj));
	obj.handle = handle;

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(&obj);
	execbuf.buffer_count = 1;
	execbuf.flags = I915_EXEC_FENCE_OUT;

	n = 0;
	count = 0;
	signal = 0;

	intel_detect_and_clear_missed_interrupts(fd);
	clock_gettime(CLOCK_MONOTONIC, &start);
	do {
		for (int e = 0; e < nengine; e++) {
			if (fences[n] != -1) {
				igt_assert(fence_wait(fences[n]));
				close(fences[n]);
			}

			execbuf.flags &= ~ENGINE_FLAGS;
			execbuf.flags |= engines[e];
			gem_execbuf_wr(fd, &execbuf);

			/* Enable signaling by doing a poll() */
			fences[n] = execbuf.rsvd2 >> 32;
			signal += fence_enable_signaling(fences[n]);

			n = (n + 1) % NFENCES;
		}

		count += nengine;
		clock_gettime(CLOCK_MONOTONIC, &now);
	} while (elapsed(&start, &now) < timeout);
	igt_assert_eq(intel_detect_and_clear_missed_interrupts(fd), 0);

	for (n = 0; n < NFENCES; n++)
		if (fences[n] != -1)
			close(fences[n]);
	free(fences);

	igt_info("Signal %s: %'lu cycles (%'lu signals): %.3fus\n",
		 ring_name, count, signal, elapsed(&start, &now) * 1e6 / count);
}

static void preempt(int fd, uint32_t handle,
		    const struct intel_execution_engine2 *e)
{
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 obj;
	struct timespec start, now;
	unsigned long count;
	uint32_t ctx[2];
	igt_spin_t *spin;

	ctx[0] = gem_context_clone_with_engines(fd, 0);
	gem_context_set_priority(fd, ctx[0], MIN_PRIO);

	ctx[1] = gem_context_clone_with_engines(fd, 0);
	gem_context_set_priority(fd, ctx[1], MAX_PRIO);

	memset(&obj, 0, sizeof(obj));
	obj.handle = handle;

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(&obj);
	execbuf.buffer_count = 1;
	execbuf.flags = e->flags;
	execbuf.flags |= I915_EXEC_HANDLE_LUT;
	execbuf.flags |= I915_EXEC_NO_RELOC;
	if (__gem_execbuf(fd, &execbuf)) {
		execbuf.flags = e->flags;
		gem_execbuf(fd, &execbuf);
	}
	execbuf.rsvd1 = ctx[1];
	intel_detect_and_clear_missed_interrupts(fd);

	count = 0;
	spin = __igt_spin_new(fd, .ctx_id = ctx[0], .engine = e->flags);
	clock_gettime(CLOCK_MONOTONIC, &start);
	do {
		gem_execbuf(fd, &execbuf);
		count++;
		clock_gettime(CLOCK_MONOTONIC, &now);
	} while (elapsed(&start, &now) < 20);
	igt_spin_free(fd, spin);
	igt_assert_eq(intel_detect_and_clear_missed_interrupts(fd), 0);

	gem_context_destroy(fd, ctx[1]);
	gem_context_destroy(fd, ctx[0]);

	igt_info("%s: %'lu cycles: %.3fus\n",
		 e->name, count, elapsed(&start, &now)*1e6 / count);
}

igt_main
{
	const struct intel_execution_engine2 *e;
	uint32_t handle = 0;
	int device = -1;

	igt_fixture {
		const uint32_t bbe = MI_BATCH_BUFFER_END;

		device = drm_open_driver(DRIVER_INTEL);
		igt_require_gem(device);
		gem_submission_print_method(device);
		gem_scheduler_print_capability(device);

		handle = gem_create(device, 4096);
		gem_write(device, handle, 0, &bbe, sizeof(bbe));

		igt_fork_hang_detector(device);
	}

	igt_subtest("basic-series")
		series(device, handle, 2);

	igt_subtest("basic-parallel")
		parallel(device, handle, 2);

	igt_subtest("basic-sequential")
		sequential(device, handle, 0, 2);

	igt_subtest_with_dynamic("single") {
		__for_each_physical_engine(device, e) {
			igt_dynamic_f("%s", e->name)
				single(device, handle, e);
		}
	}

	igt_subtest_with_dynamic("signal") {
		__for_each_physical_engine(device, e) {
			igt_dynamic_f("%s", e->name)
				fence_signal(device, handle, e,
					     e->name, 2);
		}
	}

	igt_subtest("signal-all")
		/* NULL value means all engines */
		fence_signal(device, handle, NULL, "all", 20);

	igt_subtest("series")
		series(device, handle, 20);

	igt_subtest("parallel")
		parallel(device, handle, 20);

	igt_subtest("independent")
		independent(device, handle, 20);

	igt_subtest_with_dynamic("multiple") {
		__for_each_physical_engine(device, e) {
			igt_dynamic_f("%s", e->name)
				multiple(device, e, 20);
		}
	}

	igt_subtest("sequential")
		sequential(device, handle, 0, 20);

	igt_subtest("forked-sequential")
		sequential(device, handle, FORKED, 20);

	igt_subtest("context-sequential")
		sequential(device, handle, FORKED | CONTEXT, 20);

	igt_subtest_group {
		igt_fixture {
			gem_require_contexts(device);
			igt_require(gem_scheduler_has_ctx_priority(device));
			igt_require(gem_scheduler_has_preemption(device));
		}
		igt_subtest_with_dynamic("preempt") {
			__for_each_physical_engine(device, e) {
				igt_dynamic_f("%s", e->name)
					preempt(device, handle, e);
			}
		}
	}

	igt_subtest_group {
		igt_fixture {
			igt_device_set_master(device);
		}

		igt_subtest_with_dynamic("poll") {
			__for_each_physical_engine(device, e) {
				/* Requires master for STORE_DWORD on gen4/5 */
				igt_dynamic_f("%s", e->name)
					poll_ring(device, e, 20);
			}
		}

		igt_subtest_with_dynamic("headless") {
			__for_each_physical_engine(device, e) {
				igt_dynamic_f("%s", e->name)
				/* Requires master for changing display modes */
					headless(device, handle, e);
			}
		}

		igt_subtest("poll-sequential")
			poll_sequential(device, "Sequential", 20);

	}

	igt_fixture {
		igt_stop_hang_detector();
		gem_close(device, handle);
		close(device);
	}
}
