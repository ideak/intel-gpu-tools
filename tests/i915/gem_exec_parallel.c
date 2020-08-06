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
 */

/** @file gem_exec_parallel.c
 *
 * Exercise using many, many writers into a buffer.
 */

#include <pthread.h>

#include "i915/gem.h"
#include "igt.h"
#include "igt_gt.h"

#define ENGINE_MASK  (I915_EXEC_RING_MASK | I915_EXEC_BSD_MASK)

#define VERIFY 0

static inline uint32_t hash32(uint32_t val)
{
#define GOLDEN_RATIO_32 0x61C88647
	return val * GOLDEN_RATIO_32;
}

#define CONTEXTS 0x1
#define FDS 0x2
#define USERPTR 0x4

#define NUMOBJ 16

struct thread {
	pthread_t thread;
	pthread_mutex_t *mutex;
	pthread_cond_t *cond;
	unsigned flags;
	uint32_t *scratch;
	unsigned id;
	unsigned engine;
	uint32_t used;
	int fd, gen, *go;
};

static void *thread(void *data)
{
	struct thread *t = data;
	struct drm_i915_gem_exec_object2 obj[2];
	struct drm_i915_gem_relocation_entry reloc;
	struct drm_i915_gem_execbuffer2 execbuf;
	uint32_t batch[16];
	uint16_t used;
	int fd, i;

	pthread_mutex_lock(t->mutex);
	while (*t->go == 0)
		pthread_cond_wait(t->cond, t->mutex);
	pthread_mutex_unlock(t->mutex);

	if (t->flags & FDS) {
		fd = drm_open_driver(DRIVER_INTEL);
		gem_context_copy_engines(t->fd, 0, fd, 0);
	} else {
		fd = t->fd;
	}

	i = 0;
	batch[i] = MI_STORE_DWORD_IMM | (t->gen < 6 ? 1 << 22 : 0);
	if (t->gen >= 8) {
		batch[++i] = 4*t->id;
		batch[++i] = 0;
	} else if (t->gen >= 4) {
		batch[++i] = 0;
		batch[++i] = 4*t->id;
	} else {
		batch[i]--;
		batch[++i] = 4*t->id;
	}
	batch[++i] = t->id;
	batch[++i] = MI_BATCH_BUFFER_END;

	memset(obj, 0, sizeof(obj));
	obj[0].flags = EXEC_OBJECT_WRITE;

	memset(&reloc, 0, sizeof(reloc));
	reloc.offset = sizeof(uint32_t);
	if (t->gen < 8 && t->gen >= 4)
		reloc.offset += sizeof(uint32_t);
	reloc.read_domains = I915_GEM_DOMAIN_INSTRUCTION;
	reloc.write_domain = I915_GEM_DOMAIN_INSTRUCTION;
	reloc.delta = 4*t->id;
	obj[1].handle = gem_create(fd, 4096);
	obj[1].relocs_ptr = to_user_pointer(&reloc);
	obj[1].relocation_count = 1;
	gem_write(fd, obj[1].handle, 0, batch, sizeof(batch));

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(obj);
	execbuf.buffer_count = 2;
	execbuf.flags = t->engine;
	execbuf.flags |= I915_EXEC_HANDLE_LUT;
	execbuf.flags |= I915_EXEC_NO_RELOC;
	if (t->gen < 6)
		execbuf.flags |= I915_EXEC_SECURE;
	if (t->flags & CONTEXTS) {
		execbuf.rsvd1 = gem_context_clone_with_engines(fd, 0);
	}

	used = 0;
	igt_until_timeout(1) {
		unsigned int x = rand() % NUMOBJ;

		used |= 1u << x;
		obj[0].handle = t->scratch[x];

		if (t->flags & FDS)
			obj[0].handle = gem_open(fd, obj[0].handle);

		gem_execbuf(fd, &execbuf);

		if (t->flags & FDS)
			gem_close(fd, obj[0].handle);
	}

	if (t->flags & CONTEXTS)
		gem_context_destroy(fd, execbuf.rsvd1);
	gem_close(fd, obj[1].handle);
	if (t->flags & FDS)
		close(fd);

	t->used = used;
	return NULL;
}

static void check_bo(int fd, uint32_t handle, int pass, struct thread *threads)
{
	uint32_t x = hash32(handle * pass) % 1024;
	uint32_t result;

	if (!(threads[x].used & (1 << pass)))
		return;

	igt_debug("Verifying result (pass=%d, handle=%d, thread %d)\n",
		  pass, handle, x);
	gem_read(fd, handle, x * sizeof(result), &result, sizeof(result));
	igt_assert_eq_u32(result, x);
}

static uint32_t handle_create(int fd, unsigned int flags, void **data)
{
	if (flags & USERPTR) {
		uint32_t handle;
		void *ptr;

		posix_memalign(&ptr, 4096, 4096);
		gem_userptr(fd, ptr, 4096, 0, 0, &handle);
		*data = ptr;

		return handle;
	}

	return gem_create(fd, 4096);
}

static void handle_close(int fd, unsigned int flags, uint32_t handle, void *data)
{
	if (flags & USERPTR)
		free(data);

	gem_close(fd, handle);
}

static void all(int fd, struct intel_execution_engine2 *engine, unsigned flags)
{
	const unsigned int gen = intel_gen(intel_get_drm_devid(fd));
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	struct thread *threads;
	uint32_t scratch[NUMOBJ], handle[NUMOBJ];
	unsigned engines[16], nengine;
	void *arg[NUMOBJ];
	int go;
	int i;

	if (flags & CONTEXTS)
		gem_require_contexts(fd);

	if (flags & FDS) {
		igt_require(gen > 5);
		igt_require(igt_allow_unlimited_files());
	}

	nengine = 0;
	if (!engine) {
		struct intel_execution_engine2 *e;
		__for_each_physical_engine(fd, e) {
			if (gem_class_can_store_dword(fd, e->class))
				engines[nengine++] = e->flags;
		}
	} else {
		engines[nengine++] = engine->flags;
	}
	igt_require(nengine);

	for (i = 0; i < NUMOBJ; i++) {
		scratch[i] = handle[i] = handle_create(fd, flags, &arg[i]);
		if (flags & FDS)
			scratch[i] = gem_flink(fd, handle[i]);
	}

	threads = calloc(1024, sizeof(struct thread));
	igt_assert(threads);

	intel_detect_and_clear_missed_interrupts(fd);
	pthread_mutex_init(&mutex, 0);
	pthread_cond_init(&cond, 0);
	go = 0;

	for (i = 0; i < 1024; i++) {
		threads[i].id = i;
		threads[i].fd = fd;
		threads[i].gen = gen;
		threads[i].engine = engines[i % nengine];
		threads[i].flags = flags;
		threads[i].scratch = scratch;
		threads[i].mutex = &mutex;
		threads[i].cond = &cond;
		threads[i].go = &go;

		pthread_create(&threads[i].thread, 0, thread, &threads[i]);
	}

	pthread_mutex_lock(&mutex);
	go = 1024;
	pthread_cond_broadcast(&cond);
	pthread_mutex_unlock(&mutex);

	for (i = 0; i < 1024; i++)
		pthread_join(threads[i].thread, NULL);

	for (i = 0; i < NUMOBJ; i++) {
		check_bo(fd, handle[i], i, threads);
		handle_close(fd, flags, handle[i], arg[i]);
	}

	igt_assert_eq(intel_detect_and_clear_missed_interrupts(fd), 0);
	free(threads);
}

igt_main
{
	struct intel_execution_engine2 *e;

	const struct mode {
		const char *name;
		unsigned flags;
	} modes[] = {
		{ "basic", 0 },
		{ "contexts", CONTEXTS },
		{ "fds", FDS },
		{ "userptr", USERPTR },
		{ NULL }
	};
	int fd;

	igt_fixture {
		fd = drm_open_driver_master(DRIVER_INTEL);
		igt_require_gem(fd);

		igt_fork_hang_detector(fd);
	}

	igt_subtest_with_dynamic("engines") {
		for (const struct mode *m = modes; m->name; m++)
			igt_dynamic(m->name)
				/* NULL value means all engines */
				all(fd, NULL, m->flags);
	}

	for (const struct mode *m = modes; m->name; m++) {
		igt_subtest_with_dynamic(m->name) {
			__for_each_physical_engine(fd, e) {
				if (gem_class_can_store_dword(fd, e->class))
					igt_dynamic(e->name)
						all(fd, e, m->flags);
			}
		}
	}

	igt_fixture {
		igt_stop_hang_detector();
		close(fd);
	}
}
