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

#include <time.h>
#include <pthread.h>

#include "i915/gem.h"
#include "i915/gem_ring.h"
#include "igt_debugfs.h"
#include "igt_dummyload.h"
#include "igt_gt.h"
#include "igt.h"
#include "igt_sysfs.h"

#define MAX_PRIO I915_CONTEXT_MAX_USER_PRIORITY
#define MIN_PRIO I915_CONTEXT_MIN_USER_PRIORITY

#define ENGINE_MASK  (I915_EXEC_RING_MASK | I915_EXEC_BSD_MASK)

IGT_TEST_DESCRIPTION("Basic check of ring<->ring write synchronisation.");

/*
 * Testcase: Basic check of sync
 *
 * Extremely efficient at catching missed irqs
 */

static double gettime(void)
{
	static clockid_t clock = -1;
	struct timespec ts;

	/* Stay on the same clock for consistency. */
	if (clock != (clockid_t)-1) {
		if (clock_gettime(clock, &ts))
			goto error;
		goto out;
	}

#ifdef CLOCK_MONOTONIC_RAW
	if (!clock_gettime(clock = CLOCK_MONOTONIC_RAW, &ts))
		goto out;
#endif
#ifdef CLOCK_MONOTONIC_COARSE
	if (!clock_gettime(clock = CLOCK_MONOTONIC_COARSE, &ts))
		goto out;
#endif
	if (!clock_gettime(clock = CLOCK_MONOTONIC, &ts))
		goto out;
error:
	igt_warn("Could not read monotonic time: %s\n",
		 strerror(errno));
	igt_assert(0);
	return 0;

out:
	return ts.tv_sec + 1e-9*ts.tv_nsec;
}

static void
filter_engines_can_store_dword(int fd, struct intel_engine_data *ied)
{
	unsigned int count = 0;

	for (unsigned int n = 0; n < ied->nengines; n++) {
		if (!gem_class_can_store_dword(fd, ied->engines[n].class))
			continue;

		if (count != n)
			memcpy(&ied->engines[count],
			       &ied->engines[n],
			       sizeof(ied->engines[0]));
		count++;
	}

	ied->nengines = count;
}

static struct intel_engine_data list_store_engines(int fd, unsigned ring)
{
	struct intel_engine_data ied = { };

	if (ring == ALL_ENGINES) {
		ied = intel_init_engine_list(fd, 0);
		filter_engines_can_store_dword(fd, &ied);
	} else {
		if (gem_has_ring(fd, ring) && gem_can_store_dword(fd, ring)) {
			ied.engines[ied.nengines].flags = ring;
			strcpy(ied.engines[ied.nengines].name, " ");
			ied.nengines++;
		}
	}

	return ied;
}

static struct intel_engine_data list_engines(int fd, unsigned ring)
{
	struct intel_engine_data ied = { };

	if (ring == ALL_ENGINES) {
		ied = intel_init_engine_list(fd, 0);
	} else {
		if (gem_has_ring(fd, ring)) {
			ied.engines[ied.nengines].flags = ring;
			strcpy(ied.engines[ied.nengines].name, " ");
			ied.nengines++;
		}
	}

	return ied;
}

static const char *ied_name(const struct intel_engine_data *ied, int idx)
{
	return ied->engines[idx % ied->nengines].name;
}

static unsigned int ied_flags(const struct intel_engine_data *ied, int idx)
{
	return ied->engines[idx % ied->nengines].flags;
}

static void xchg_engine(void *array, unsigned i, unsigned j)
{
	struct intel_execution_engine2 *E = array;

	igt_swap(E[i], E[j]);
}

static void
sync_ring(int fd, unsigned ring, int num_children, int timeout)
{
	struct intel_engine_data ied;

	ied = list_engines(fd, ring);
	igt_require(ied.nengines);
	num_children *= ied.nengines;

	intel_detect_and_clear_missed_interrupts(fd);
	igt_fork(child, num_children) {
		const uint32_t bbe = MI_BATCH_BUFFER_END;
		struct drm_i915_gem_exec_object2 object;
		struct drm_i915_gem_execbuffer2 execbuf;
		double start, elapsed;
		unsigned long cycles;

		memset(&object, 0, sizeof(object));
		object.handle = gem_create(fd, 4096);
		gem_write(fd, object.handle, 0, &bbe, sizeof(bbe));

		memset(&execbuf, 0, sizeof(execbuf));
		execbuf.buffers_ptr = to_user_pointer(&object);
		execbuf.buffer_count = 1;
		execbuf.flags = ied_flags(&ied, child);
		gem_execbuf(fd, &execbuf);
		gem_sync(fd, object.handle);

		start = gettime();
		cycles = 0;
		do {
			do {
				gem_execbuf(fd, &execbuf);
				gem_sync(fd, object.handle);
			} while (++cycles & 1023);
		} while ((elapsed = gettime() - start) < timeout);

		igt_info("%s %ld cycles: %.3f us\n",
			 ied_name(&ied, child), cycles, elapsed * 1e6 / cycles);

		gem_close(fd, object.handle);
	}
	igt_waitchildren_timeout(timeout+10, NULL);
	igt_assert_eq(intel_detect_and_clear_missed_interrupts(fd), 0);
}

static void
idle_ring(int fd, unsigned int ring, int num_children, int timeout)
{
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	struct drm_i915_gem_exec_object2 object;
	struct drm_i915_gem_execbuffer2 execbuf;
	double start, elapsed;
	unsigned long cycles;

	gem_require_ring(fd, ring);

	memset(&object, 0, sizeof(object));
	object.handle = gem_create(fd, 4096);
	gem_write(fd, object.handle, 0, &bbe, sizeof(bbe));

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(&object);
	execbuf.buffer_count = 1;
	execbuf.flags = ring;
	gem_execbuf(fd, &execbuf);
	gem_sync(fd, object.handle);

	intel_detect_and_clear_missed_interrupts(fd);
	start = gettime();
	cycles = 0;
	do {
		do {
			gem_execbuf(fd, &execbuf);
			gem_quiescent_gpu(fd);
		} while (++cycles & 1023);
	} while ((elapsed = gettime() - start) < timeout);
	igt_assert_eq(intel_detect_and_clear_missed_interrupts(fd), 0);

	igt_info("Completed %ld cycles: %.3f us\n",
		 cycles, elapsed * 1e6 / cycles);

	gem_close(fd, object.handle);
}

static void
wakeup_ring(int fd, unsigned ring, int timeout, int wlen)
{
	struct intel_engine_data ied;

	ied = list_store_engines(fd, ring);
	igt_require(ied.nengines);

	intel_detect_and_clear_missed_interrupts(fd);
	igt_fork(child, ied.nengines) {
		const uint32_t bbe = MI_BATCH_BUFFER_END;
		struct drm_i915_gem_exec_object2 object;
		struct drm_i915_gem_execbuffer2 execbuf;
		double end, this, elapsed, now, baseline;
		unsigned long cycles;
		igt_spin_t *spin;

		memset(&object, 0, sizeof(object));
		object.handle = gem_create(fd, 4096);
		gem_write(fd, object.handle, 0, &bbe, sizeof(bbe));

		memset(&execbuf, 0, sizeof(execbuf));
		execbuf.buffers_ptr = to_user_pointer(&object);
		execbuf.buffer_count = 1;
		execbuf.flags = ied_flags(&ied, child);

		spin = __igt_spin_new(fd,
				      .engine = execbuf.flags,
				      .flags = (IGT_SPIN_POLL_RUN |
						IGT_SPIN_FAST));
		igt_assert(igt_spin_has_poll(spin));

		gem_execbuf(fd, &execbuf);

		igt_spin_end(spin);
		gem_sync(fd, object.handle);

		for (int warmup = 0; warmup <= 1; warmup++) {
			end = gettime() + timeout/10.;
			elapsed = 0;
			cycles = 0;
			do {
				igt_spin_reset(spin);

				gem_execbuf(fd, &spin->execbuf);
				igt_spin_busywait_until_started(spin);

				this = gettime();
				igt_spin_end(spin);
				gem_sync(fd, spin->handle);
				now = gettime();

				elapsed += now - this;
				cycles++;
			} while (now < end);
			baseline = elapsed / cycles;
		}
		igt_info("%s baseline %ld cycles: %.3f us\n",
			 ied_name(&ied, child), cycles, elapsed * 1e6 / cycles);

		end = gettime() + timeout;
		elapsed = 0;
		cycles = 0;
		do {
			igt_spin_reset(spin);

			gem_execbuf(fd, &spin->execbuf);
			igt_spin_busywait_until_started(spin);

			for (int n = 0; n < wlen; n++)
				gem_execbuf(fd, &execbuf);

			this = gettime();
			igt_spin_end(spin);
			gem_sync(fd, object.handle);
			now = gettime();

			elapsed += now - this;
			cycles++;
		} while (now < end);
		elapsed -= cycles * baseline;

		igt_info("%s completed %ld cycles: %.3f + %.3f us\n",
			 ied_name(&ied, child),
			 cycles, 1e6 * baseline, elapsed * 1e6 / cycles);

		igt_spin_free(fd, spin);
		gem_close(fd, object.handle);
	}
	igt_waitchildren_timeout(2*timeout, NULL);
	igt_assert_eq(intel_detect_and_clear_missed_interrupts(fd), 0);
}

static void active_ring(int fd, unsigned int ring,
			int num_children, int timeout)
{
	struct intel_engine_data ied;

	ied = list_store_engines(fd, ring);
	igt_require(ied.nengines);

	intel_detect_and_clear_missed_interrupts(fd);
	igt_fork(child, ied.nengines) {
		double start, end, elapsed;
		unsigned long cycles;
		igt_spin_t *spin[2];

		spin[0] = __igt_spin_new(fd,
					 .engine = ied_flags(&ied, child),
					 .flags = IGT_SPIN_FAST);

		spin[1] = __igt_spin_new(fd,
					 .engine = ied_flags(&ied, child),
					 .flags = IGT_SPIN_FAST);

		start = gettime();
		end = start + timeout;
		cycles = 0;
		do {
			for (int loop = 0; loop < 1024; loop++) {
				igt_spin_t *s = spin[loop & 1];

				igt_spin_end(s);
				gem_sync(fd, s->handle);

				igt_spin_reset(s);

				gem_execbuf(fd, &s->execbuf);
			}
			cycles += 1024;
		} while ((elapsed = gettime()) < end);
		igt_spin_free(fd, spin[1]);
		igt_spin_free(fd, spin[0]);

		igt_info("%s %ld cycles: %.3f us\n",
			 ied_name(&ied, child),
			 cycles, (elapsed - start) * 1e6 / cycles);
	}
	igt_waitchildren_timeout(2*timeout, NULL);
	igt_assert_eq(intel_detect_and_clear_missed_interrupts(fd), 0);
}

static void
active_wakeup_ring(int fd, unsigned ring, int timeout, int wlen)
{
	struct intel_engine_data ied;

	ied = list_store_engines(fd, ring);
	igt_require(ied.nengines);

	intel_detect_and_clear_missed_interrupts(fd);
	igt_fork(child, ied.nengines) {
		const uint32_t bbe = MI_BATCH_BUFFER_END;
		struct drm_i915_gem_exec_object2 object;
		struct drm_i915_gem_execbuffer2 execbuf;
		double end, this, elapsed, now, baseline;
		unsigned long cycles;
		igt_spin_t *spin[2];

		memset(&object, 0, sizeof(object));
		object.handle = gem_create(fd, 4096);
		gem_write(fd, object.handle, 0, &bbe, sizeof(bbe));

		memset(&execbuf, 0, sizeof(execbuf));
		execbuf.buffers_ptr = to_user_pointer(&object);
		execbuf.buffer_count = 1;
		execbuf.flags = ied_flags(&ied, child);

		spin[0] = __igt_spin_new(fd,
					 .engine = execbuf.flags,
					 .flags = (IGT_SPIN_POLL_RUN |
						   IGT_SPIN_FAST));
		igt_assert(igt_spin_has_poll(spin[0]));

		spin[1] = __igt_spin_new(fd,
					 .engine = execbuf.flags,
					 .flags = (IGT_SPIN_POLL_RUN |
						   IGT_SPIN_FAST));

		gem_execbuf(fd, &execbuf);

		igt_spin_end(spin[1]);
		igt_spin_end(spin[0]);
		gem_sync(fd, object.handle);

		for (int warmup = 0; warmup <= 1; warmup++) {
			igt_spin_reset(spin[0]);

			gem_execbuf(fd, &spin[0]->execbuf);

			end = gettime() + timeout/10.;
			elapsed = 0;
			cycles = 0;
			do {
				igt_spin_busywait_until_started(spin[0]);

				igt_spin_reset(spin[1]);

				gem_execbuf(fd, &spin[1]->execbuf);

				this = gettime();
				igt_spin_end(spin[0]);
				gem_sync(fd, spin[0]->handle);
				now = gettime();

				elapsed += now - this;
				cycles++;
				igt_swap(spin[0], spin[1]);
			} while (now < end);
			igt_spin_end(spin[0]);
			baseline = elapsed / cycles;
		}
		igt_info("%s baseline %ld cycles: %.3f us\n",
			 ied_name(&ied, child), cycles, elapsed * 1e6  /cycles);

		igt_spin_reset(spin[0]);

		gem_execbuf(fd, &spin[0]->execbuf);

		end = gettime() + timeout;
		elapsed = 0;
		cycles = 0;
		do {
			igt_spin_busywait_until_started(spin[0]);

			for (int n = 0; n < wlen; n++)
				gem_execbuf(fd, &execbuf);

			igt_spin_reset(spin[1]);

			gem_execbuf(fd, &spin[1]->execbuf);

			this = gettime();
			igt_spin_end(spin[0]);
			gem_sync(fd, object.handle);
			now = gettime();

			elapsed += now - this;
			cycles++;
			igt_swap(spin[0], spin[1]);
		} while (now < end);
		igt_spin_end(spin[0]);
		elapsed -= cycles * baseline;

		igt_info("%s completed %ld cycles: %.3f + %.3f us\n",
			 ied_name(&ied, child),
			 cycles, 1e6 * baseline, elapsed * 1e6 / cycles);

		igt_spin_free(fd, spin[1]);
		igt_spin_free(fd, spin[0]);
		gem_close(fd, object.handle);
	}
	igt_waitchildren_timeout(2*timeout, NULL);
	igt_assert_eq(intel_detect_and_clear_missed_interrupts(fd), 0);
}

static void
store_ring(int fd, unsigned ring, int num_children, int timeout)
{
	const unsigned int gen = intel_gen(intel_get_drm_devid(fd));
	struct intel_engine_data ied;

	ied = list_store_engines(fd, ring);
	igt_require(ied.nengines);
	num_children *= ied.nengines;

	intel_detect_and_clear_missed_interrupts(fd);
	igt_fork(child, num_children) {
		const uint32_t bbe = MI_BATCH_BUFFER_END;
		struct drm_i915_gem_exec_object2 object[2];
		struct drm_i915_gem_relocation_entry reloc[1024];
		struct drm_i915_gem_execbuffer2 execbuf;
		double start, elapsed;
		unsigned long cycles;
		uint32_t *batch, *b;

		memset(&execbuf, 0, sizeof(execbuf));
		execbuf.buffers_ptr = to_user_pointer(object);
		execbuf.flags = ied_flags(&ied, child);
		execbuf.flags |= I915_EXEC_NO_RELOC;
		execbuf.flags |= I915_EXEC_HANDLE_LUT;
		if (gen < 6)
			execbuf.flags |= I915_EXEC_SECURE;

		memset(object, 0, sizeof(object));
		object[0].handle = gem_create(fd, 4096);
		gem_write(fd, object[0].handle, 0, &bbe, sizeof(bbe));
		execbuf.buffer_count = 1;
		gem_execbuf(fd, &execbuf);

		object[0].flags |= EXEC_OBJECT_WRITE;
		object[1].handle = gem_create(fd, 20*1024);

		object[1].relocs_ptr = to_user_pointer(reloc);
		object[1].relocation_count = 1024;

		batch = gem_mmap__cpu(fd, object[1].handle, 0, 20*1024,
				      PROT_WRITE | PROT_READ);
		gem_set_domain(fd, object[1].handle,
			       I915_GEM_DOMAIN_CPU, I915_GEM_DOMAIN_CPU);

		memset(reloc, 0, sizeof(reloc));
		b = batch;
		for (int i = 0; i < 1024; i++) {
			uint64_t offset;

			reloc[i].presumed_offset = object[0].offset;
			reloc[i].offset = (b - batch + 1) * sizeof(*batch);
			reloc[i].delta = i * sizeof(uint32_t);
			reloc[i].read_domains = I915_GEM_DOMAIN_INSTRUCTION;
			reloc[i].write_domain = I915_GEM_DOMAIN_INSTRUCTION;

			offset = object[0].offset + reloc[i].delta;
			*b++ = MI_STORE_DWORD_IMM | (gen < 6 ? 1 << 22 : 0);
			if (gen >= 8) {
				*b++ = offset;
				*b++ = offset >> 32;
			} else if (gen >= 4) {
				*b++ = 0;
				*b++ = offset;
				reloc[i].offset += sizeof(*batch);
			} else {
				b[-1] -= 1;
				*b++ = offset;
			}
			*b++ = i;
		}
		*b++ = MI_BATCH_BUFFER_END;
		igt_assert((b - batch)*sizeof(uint32_t) < 20*1024);
		munmap(batch, 20*1024);
		execbuf.buffer_count = 2;
		gem_execbuf(fd, &execbuf);
		gem_sync(fd, object[1].handle);

		start = gettime();
		cycles = 0;
		do {
			do {
				gem_execbuf(fd, &execbuf);
				gem_sync(fd, object[1].handle);
			} while (++cycles & 1023);
		} while ((elapsed = gettime() - start) < timeout);
		igt_info("%s completed %ld cycles: %.3f us\n",
			 ied_name(&ied, child), cycles, elapsed  *1e6 / cycles);

		gem_close(fd, object[1].handle);
		gem_close(fd, object[0].handle);
	}
	igt_waitchildren_timeout(timeout+10, NULL);
	igt_assert_eq(intel_detect_and_clear_missed_interrupts(fd), 0);
}

static void
switch_ring(int fd, unsigned ring, int num_children, int timeout)
{
	const unsigned int gen = intel_gen(intel_get_drm_devid(fd));
	struct intel_engine_data ied;

	gem_require_contexts(fd);

	ied = list_store_engines(fd, ring);
	igt_require(ied.nengines);
	num_children *= ied.nengines;

	intel_detect_and_clear_missed_interrupts(fd);
	igt_fork(child, num_children) {
		struct context {
			struct drm_i915_gem_exec_object2 object[2];
			struct drm_i915_gem_relocation_entry reloc[1024];
			struct drm_i915_gem_execbuffer2 execbuf;
		} contexts[2];
		double elapsed, baseline;
		unsigned long cycles;

		for (int i = 0; i < ARRAY_SIZE(contexts); i++) {
			const uint32_t bbe = MI_BATCH_BUFFER_END;
			const uint32_t sz = 32 << 10;
			struct context *c = &contexts[i];
			uint32_t *batch, *b;

			memset(&c->execbuf, 0, sizeof(c->execbuf));
			c->execbuf.buffers_ptr = to_user_pointer(c->object);
			c->execbuf.flags = ied_flags(&ied, child);
			c->execbuf.flags |= I915_EXEC_NO_RELOC;
			c->execbuf.flags |= I915_EXEC_HANDLE_LUT;
			if (gen < 6)
				c->execbuf.flags |= I915_EXEC_SECURE;
			c->execbuf.rsvd1 = gem_context_create(fd);

			memset(c->object, 0, sizeof(c->object));
			c->object[0].handle = gem_create(fd, 4096);
			gem_write(fd, c->object[0].handle, 0, &bbe, sizeof(bbe));
			c->execbuf.buffer_count = 1;
			gem_execbuf(fd, &c->execbuf);

			c->object[0].flags |= EXEC_OBJECT_WRITE;
			c->object[1].handle = gem_create(fd, sz);

			c->object[1].relocs_ptr = to_user_pointer(c->reloc);
			c->object[1].relocation_count = 1024 * i;

			batch = gem_mmap__cpu(fd, c->object[1].handle, 0, sz,
					      PROT_WRITE | PROT_READ);
			gem_set_domain(fd, c->object[1].handle,
				       I915_GEM_DOMAIN_CPU, I915_GEM_DOMAIN_CPU);

			memset(c->reloc, 0, sizeof(c->reloc));
			b = batch;
			for (int r = 0; r < c->object[1].relocation_count; r++) {
				uint64_t offset;

				c->reloc[r].presumed_offset = c->object[0].offset;
				c->reloc[r].offset = (b - batch + 1) * sizeof(*batch);
				c->reloc[r].delta = r * sizeof(uint32_t);
				c->reloc[r].read_domains = I915_GEM_DOMAIN_INSTRUCTION;
				c->reloc[r].write_domain = I915_GEM_DOMAIN_INSTRUCTION;

				offset = c->object[0].offset + c->reloc[r].delta;
				*b++ = MI_STORE_DWORD_IMM | (gen < 6 ? 1 << 22 : 0);
				if (gen >= 8) {
					*b++ = offset;
					*b++ = offset >> 32;
				} else if (gen >= 4) {
					*b++ = 0;
					*b++ = offset;
					c->reloc[r].offset += sizeof(*batch);
				} else {
					b[-1] -= 1;
					*b++ = offset;
				}
				*b++ = r;
				*b++ = 0x5 << 23;
			}
			*b++ = MI_BATCH_BUFFER_END;
			igt_assert((b - batch)*sizeof(uint32_t) < sz);
			munmap(batch, sz);
			c->execbuf.buffer_count = 2;
			gem_execbuf(fd, &c->execbuf);
			gem_sync(fd, c->object[1].handle);
		}

		cycles = 0;
		baseline = 0;
		igt_until_timeout(timeout) {
			do {
				double this;

				gem_execbuf(fd, &contexts[1].execbuf);
				gem_execbuf(fd, &contexts[0].execbuf);

				this = gettime();
				gem_sync(fd, contexts[1].object[1].handle);
				gem_sync(fd, contexts[0].object[1].handle);
				baseline += gettime() - this;
			} while (++cycles & 1023);
		}
		baseline /= cycles;

		cycles = 0;
		elapsed = 0;
		igt_until_timeout(timeout) {
			do {
				double this;

				gem_execbuf(fd, &contexts[1].execbuf);
				gem_execbuf(fd, &contexts[0].execbuf);

				this = gettime();
				gem_sync(fd, contexts[0].object[1].handle);
				elapsed += gettime() - this;

				gem_sync(fd, contexts[1].object[1].handle);
			} while (++cycles & 1023);
		}
		elapsed /= cycles;

		igt_info("%s completed %ld cycles: %.3f us, baseline %.3f us\n",
			 ied_name(&ied, child),
			 cycles, elapsed * 1e6, baseline * 1e6);

		for (int i = 0; i < ARRAY_SIZE(contexts); i++) {
			gem_close(fd, contexts[i].object[1].handle);
			gem_close(fd, contexts[i].object[0].handle);
			gem_context_destroy(fd, contexts[i].execbuf.rsvd1);
		}
	}
	igt_waitchildren_timeout(timeout+10, NULL);
	igt_assert_eq(intel_detect_and_clear_missed_interrupts(fd), 0);
}

static void xchg(void *array, unsigned i, unsigned j)
{
	uint32_t *u32 = array;
	uint32_t tmp = u32[i];
	u32[i] = u32[j];
	u32[j] = tmp;
}

struct waiter {
	pthread_t thread;
	pthread_mutex_t mutex;
	pthread_cond_t cond;

	int ready;
	volatile int *done;

	int fd;
	struct drm_i915_gem_exec_object2 object;
	uint32_t handles[64];
};

static void *waiter(void *arg)
{
	struct waiter *w = arg;

	do {
		pthread_mutex_lock(&w->mutex);
		w->ready = 0;
		pthread_cond_signal(&w->cond);
		while (!w->ready)
			pthread_cond_wait(&w->cond, &w->mutex);
		pthread_mutex_unlock(&w->mutex);
		if (*w->done < 0)
			return NULL;

		gem_sync(w->fd, w->object.handle);
		for (int n = 0;  n < ARRAY_SIZE(w->handles); n++)
			gem_sync(w->fd, w->handles[n]);
	} while (1);
}

static void
__store_many(int fd, unsigned ring, int timeout, unsigned long *cycles)
{
	const unsigned int gen = intel_gen(intel_get_drm_devid(fd));
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	struct drm_i915_gem_exec_object2 object[2];
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_relocation_entry reloc[1024];
	struct waiter threads[64];
	int order[64];
	uint32_t *batch, *b;
	int done;

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(object);
	execbuf.flags = ring;
	execbuf.flags |= I915_EXEC_NO_RELOC;
	execbuf.flags |= I915_EXEC_HANDLE_LUT;
	if (gen < 6)
		execbuf.flags |= I915_EXEC_SECURE;

	memset(object, 0, sizeof(object));
	object[0].handle = gem_create(fd, 4096);
	gem_write(fd, object[0].handle, 0, &bbe, sizeof(bbe));
	execbuf.buffer_count = 1;
	gem_execbuf(fd, &execbuf);
	object[0].flags |= EXEC_OBJECT_WRITE;

	object[1].relocs_ptr = to_user_pointer(reloc);
	object[1].relocation_count = 1024;
	execbuf.buffer_count = 2;

	memset(reloc, 0, sizeof(reloc));
	b = batch = malloc(20*1024);
	for (int i = 0; i < 1024; i++) {
		uint64_t offset;

		reloc[i].presumed_offset = object[0].offset;
		reloc[i].offset = (b - batch + 1) * sizeof(*batch);
		reloc[i].delta = i * sizeof(uint32_t);
		reloc[i].read_domains = I915_GEM_DOMAIN_INSTRUCTION;
		reloc[i].write_domain = I915_GEM_DOMAIN_INSTRUCTION;

		offset = object[0].offset + reloc[i].delta;
		*b++ = MI_STORE_DWORD_IMM | (gen < 6 ? 1 << 22 : 0);
		if (gen >= 8) {
			*b++ = offset;
			*b++ = offset >> 32;
		} else if (gen >= 4) {
			*b++ = 0;
			*b++ = offset;
			reloc[i].offset += sizeof(*batch);
		} else {
			b[-1] -= 1;
			*b++ = offset;
		}
		*b++ = i;
	}
	*b++ = MI_BATCH_BUFFER_END;
	igt_assert((b - batch)*sizeof(uint32_t) < 20*1024);

	done = 0;
	for (int i = 0; i < ARRAY_SIZE(threads); i++) {
		threads[i].fd = fd;
		threads[i].object = object[1];
		threads[i].object.handle = gem_create(fd, 20*1024);
		gem_write(fd, threads[i].object.handle, 0, batch, 20*1024);

		pthread_cond_init(&threads[i].cond, NULL);
		pthread_mutex_init(&threads[i].mutex, NULL);
		threads[i].done = &done;
		threads[i].ready = 0;

		pthread_create(&threads[i].thread, NULL, waiter, &threads[i]);
		order[i] = i;
	}
	free(batch);

	for (int i = 0; i < ARRAY_SIZE(threads); i++) {
		for (int j = 0; j < ARRAY_SIZE(threads); j++)
			threads[i].handles[j] = threads[j].object.handle;
	}

	igt_until_timeout(timeout) {
		for (int i = 0; i < ARRAY_SIZE(threads); i++) {
			pthread_mutex_lock(&threads[i].mutex);
			while (threads[i].ready)
				pthread_cond_wait(&threads[i].cond,
						  &threads[i].mutex);
			pthread_mutex_unlock(&threads[i].mutex);
			igt_permute_array(threads[i].handles,
					  ARRAY_SIZE(threads[i].handles),
					  xchg);
		}

		igt_permute_array(order, ARRAY_SIZE(threads), xchg);
		for (int i = 0; i < ARRAY_SIZE(threads); i++) {
			object[1] = threads[i].object;
			gem_execbuf(fd, &execbuf);
			threads[i].object = object[1];
		}
		++*cycles;

		for (int i = 0; i < ARRAY_SIZE(threads); i++) {
			struct waiter *w = &threads[order[i]];

			w->ready = 1;
			pthread_cond_signal(&w->cond);
		}
	}

	for (int i = 0; i < ARRAY_SIZE(threads); i++) {
		pthread_mutex_lock(&threads[i].mutex);
		while (threads[i].ready)
			pthread_cond_wait(&threads[i].cond, &threads[i].mutex);
		pthread_mutex_unlock(&threads[i].mutex);
	}
	done = -1;
	for (int i = 0; i < ARRAY_SIZE(threads); i++) {
		threads[i].ready = 1;
		pthread_cond_signal(&threads[i].cond);
		pthread_join(threads[i].thread, NULL);
		gem_close(fd, threads[i].object.handle);
	}

	gem_close(fd, object[0].handle);
}

static void
store_many(int fd, unsigned int ring, int num_children, int timeout)
{
	struct intel_engine_data ied;
	unsigned long *shared;

	shared = mmap(NULL, 4096, PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
	igt_assert(shared != MAP_FAILED);

	ied = list_store_engines(fd, ring);
	igt_require(ied.nengines);

	intel_detect_and_clear_missed_interrupts(fd);

	for (int n = 0; n < ied.nengines; n++) {
		igt_fork(child, 1)
			__store_many(fd,
				     ied_flags(&ied, n),
				     timeout,
				     &shared[n]);
	}
	igt_waitchildren();

	for (int n = 0; n < ied.nengines; n++) {
		igt_info("%s completed %ld cycles\n",
			 ied_name(&ied, n), shared[n]);
	}
	igt_assert_eq(intel_detect_and_clear_missed_interrupts(fd), 0);
	munmap(shared, 4096);
}

static void
sync_all(int fd, int num_children, int timeout)
{
	struct intel_engine_data ied;

	ied = list_engines(fd, ALL_ENGINES);
	igt_require(ied.nengines);

	intel_detect_and_clear_missed_interrupts(fd);
	igt_fork(child, num_children) {
		const uint32_t bbe = MI_BATCH_BUFFER_END;
		struct drm_i915_gem_exec_object2 object;
		struct drm_i915_gem_execbuffer2 execbuf;
		double start, elapsed;
		unsigned long cycles;

		memset(&object, 0, sizeof(object));
		object.handle = gem_create(fd, 4096);
		gem_write(fd, object.handle, 0, &bbe, sizeof(bbe));

		memset(&execbuf, 0, sizeof(execbuf));
		execbuf.buffers_ptr = to_user_pointer(&object);
		execbuf.buffer_count = 1;
		gem_execbuf(fd, &execbuf);
		gem_sync(fd, object.handle);

		start = gettime();
		cycles = 0;
		do {
			do {
				for (int n = 0; n < ied.nengines; n++) {
					execbuf.flags = ied_flags(&ied, n);
					gem_execbuf(fd, &execbuf);
				}
				gem_sync(fd, object.handle);
			} while (++cycles & 1023);
		} while ((elapsed = gettime() - start) < timeout);
		igt_info("Completed %ld cycles: %.3f us\n",
			 cycles, elapsed * 1e6 / cycles);

		gem_close(fd, object.handle);
	}
	igt_waitchildren_timeout(timeout+10, NULL);
	igt_assert_eq(intel_detect_and_clear_missed_interrupts(fd), 0);
}

static void
store_all(int fd, int num_children, int timeout)
{
	const unsigned int gen = intel_gen(intel_get_drm_devid(fd));
	struct intel_engine_data ied;

	ied = list_store_engines(fd, ALL_ENGINES);
	igt_require(ied.nengines);

	intel_detect_and_clear_missed_interrupts(fd);
	igt_fork(child, num_children) {
		const uint32_t bbe = MI_BATCH_BUFFER_END;
		struct drm_i915_gem_exec_object2 object[2];
		struct drm_i915_gem_relocation_entry reloc[1024];
		struct drm_i915_gem_execbuffer2 execbuf;
		double start, elapsed;
		unsigned long cycles;
		uint32_t *batch, *b;

		memset(&execbuf, 0, sizeof(execbuf));
		execbuf.buffers_ptr = to_user_pointer(object);
		execbuf.flags |= I915_EXEC_NO_RELOC;
		execbuf.flags |= I915_EXEC_HANDLE_LUT;
		if (gen < 6)
			execbuf.flags |= I915_EXEC_SECURE;

		memset(object, 0, sizeof(object));
		object[0].handle = gem_create(fd, 4096);
		gem_write(fd, object[0].handle, 0, &bbe, sizeof(bbe));
		execbuf.buffer_count = 1;
		gem_execbuf(fd, &execbuf);

		object[0].flags |= EXEC_OBJECT_WRITE;
		object[1].handle = gem_create(fd, 1024*16 + 4096);

		object[1].relocs_ptr = to_user_pointer(reloc);
		object[1].relocation_count = 1024;

		batch = gem_mmap__cpu(fd, object[1].handle, 0, 16*1024 + 4096,
				      PROT_WRITE | PROT_READ);
		gem_set_domain(fd, object[1].handle,
			       I915_GEM_DOMAIN_CPU, I915_GEM_DOMAIN_CPU);

		memset(reloc, 0, sizeof(reloc));
		b = batch;
		for (int i = 0; i < 1024; i++) {
			uint64_t offset;

			reloc[i].presumed_offset = object[0].offset;
			reloc[i].offset = (b - batch + 1) * sizeof(*batch);
			reloc[i].delta = i * sizeof(uint32_t);
			reloc[i].read_domains = I915_GEM_DOMAIN_INSTRUCTION;
			reloc[i].write_domain = I915_GEM_DOMAIN_INSTRUCTION;

			offset = object[0].offset + reloc[i].delta;
			*b++ = MI_STORE_DWORD_IMM | (gen < 6 ? 1 << 22 : 0);
			if (gen >= 8) {
				*b++ = offset;
				*b++ = offset >> 32;
			} else if (gen >= 4) {
				*b++ = 0;
				*b++ = offset;
				reloc[i].offset += sizeof(*batch);
			} else {
				b[-1] -= 1;
				*b++ = offset;
			}
			*b++ = i;
		}
		*b++ = MI_BATCH_BUFFER_END;
		igt_assert((b - batch)*sizeof(uint32_t) < 20*1024);
		munmap(batch, 16*1024+4096);
		execbuf.buffer_count = 2;
		gem_execbuf(fd, &execbuf);
		gem_sync(fd, object[1].handle);

		start = gettime();
		cycles = 0;
		do {
			do {
				igt_permute_array(ied.engines, ied.nengines, xchg_engine);
				for (int n = 0; n < ied.nengines; n++) {
					execbuf.flags &= ~ENGINE_MASK;
					execbuf.flags |= ied_flags(&ied, n);
					gem_execbuf(fd, &execbuf);
				}
				gem_sync(fd, object[1].handle);
			} while (++cycles & 1023);
		} while ((elapsed = gettime() - start) < timeout);
		igt_info("Completed %ld cycles: %.3f us\n",
			 cycles, elapsed * 1e6 / cycles);

		gem_close(fd, object[1].handle);
		gem_close(fd, object[0].handle);
	}
	igt_waitchildren_timeout(timeout+10, NULL);
	igt_assert_eq(intel_detect_and_clear_missed_interrupts(fd), 0);
}

static void
preempt(int fd, unsigned ring, int num_children, int timeout)
{
	struct intel_engine_data ied;
	uint32_t ctx[2];

	ied = list_engines(fd, ALL_ENGINES);
	igt_require(ied.nengines);
	num_children *= ied.nengines;

	ctx[0] = gem_context_create(fd);
	gem_context_set_priority(fd, ctx[0], MIN_PRIO);

	ctx[1] = gem_context_create(fd);
	gem_context_set_priority(fd, ctx[1], MAX_PRIO);

	intel_detect_and_clear_missed_interrupts(fd);
	igt_fork(child, num_children) {
		const uint32_t bbe = MI_BATCH_BUFFER_END;
		struct drm_i915_gem_exec_object2 object;
		struct drm_i915_gem_execbuffer2 execbuf;
		double start, elapsed;
		unsigned long cycles;

		memset(&object, 0, sizeof(object));
		object.handle = gem_create(fd, 4096);
		gem_write(fd, object.handle, 0, &bbe, sizeof(bbe));

		memset(&execbuf, 0, sizeof(execbuf));
		execbuf.buffers_ptr = to_user_pointer(&object);
		execbuf.buffer_count = 1;
		execbuf.flags = ied_flags(&ied, child);
		execbuf.rsvd1 = ctx[1];
		gem_execbuf(fd, &execbuf);
		gem_sync(fd, object.handle);

		start = gettime();
		cycles = 0;
		do {
			igt_spin_t *spin =
				__igt_spin_new(fd,
					       .ctx = ctx[0],
					       .engine = execbuf.flags);

			do {
				gem_execbuf(fd, &execbuf);
				gem_sync(fd, object.handle);
			} while (++cycles & 1023);

			igt_spin_free(fd, spin);
		} while ((elapsed = gettime() - start) < timeout);

		igt_info("%s %ld cycles: %.3f us\n",
			 ied_name(&ied, child), cycles, elapsed * 1e6/cycles);

		gem_close(fd, object.handle);
	}
	igt_waitchildren_timeout(timeout+10, NULL);
	igt_assert_eq(intel_detect_and_clear_missed_interrupts(fd), 0);

	gem_context_destroy(fd, ctx[1]);
	gem_context_destroy(fd, ctx[0]);
}

igt_main
{
	const int ncpus = sysconf(_SC_NPROCESSORS_ONLN);
	const struct {
		const char *name;
		void (*func)(int fd, unsigned int engine,
			     int num_children, int timeout);
		int num_children;
		int timeout;
	} all[] = {
		{ "basic-each", sync_ring, 1, 2 },
		{ "basic-store-each", store_ring, 1, 2 },
		{ "basic-many-each", store_many, 0, 2 },
		{ "switch-each", switch_ring, 1, 20 },
		{ "forked-switch-each", switch_ring, ncpus, 20 },
		{ "forked-each", sync_ring, ncpus, 20 },
		{ "forked-store-each", store_ring, ncpus, 20 },
		{ "active-each", active_ring, 0, 20 },
		{ "wakeup-each", wakeup_ring, 20, 1 },
		{ "active-wakeup-each", active_wakeup_ring, 20, 1 },
		{ "double-wakeup-each", wakeup_ring, 20, 2 },
		{}
	}, individual[] = {
		{ "default", sync_ring, 1, 20 },
		{ "idle", idle_ring, 0, 20 },
		{ "active", active_ring, 0, 20 },
		{ "wakeup", wakeup_ring, 20, 1 },
		{ "active-wakeup", active_wakeup_ring, 20, 1 },
		{ "double-wakeup", wakeup_ring, 20, 2 },
		{ "store", store_ring, 1, 20 },
		{ "switch", switch_ring, 1, 20 },
		{ "forked-switch", switch_ring, ncpus, 20 },
		{ "many", store_many, 0, 20 },
		{ "forked", sync_ring, ncpus, 20 },
		{ "forked-store", store_ring, ncpus, 20 },
		{}
	};
#define for_each_test(t, T) for(typeof(*T) *t = T; t->name; t++)

	const struct intel_execution_engine2 *e;
	int fd = -1;

	igt_fixture {
		fd = drm_open_driver(DRIVER_INTEL);
		igt_require_gem(fd);
		gem_submission_print_method(fd);
		gem_scheduler_print_capability(fd);

		igt_fork_hang_detector(fd);
	}

	/* Legacy for selecting rings. */
	for_each_test(t, individual) {
		igt_subtest_with_dynamic_f("%s", t->name) {
			for (const struct intel_execution_ring *l = intel_execution_rings; l->name; l++) {
				igt_dynamic_f("%s", l->name) {
					t->func(fd, eb_ring(l),
						t->num_children, t->timeout);
				}
			}
		}
	}

	igt_subtest("basic-all")
		sync_all(fd, 1, 2);
	igt_subtest("basic-store-all")
		store_all(fd, 1, 2);

	igt_subtest("all")
		sync_all(fd, 1, 20);
	igt_subtest("store-all")
		store_all(fd, 1, 20);
	igt_subtest("forked-all")
		sync_all(fd, ncpus, 20);
	igt_subtest("forked-store-all")
		store_all(fd, ncpus, 20);

	for_each_test(t, all) {
		igt_subtest_f("%s", t->name)
			t->func(fd, ALL_ENGINES, t->num_children, t->timeout);
	}

	/* New way of selecting engines. */
	for_each_test(t, individual) {
		igt_subtest_with_dynamic_f("%s", t->name) {
			__for_each_physical_engine(fd, e) {
				igt_dynamic_f("%s", e->name) {
					t->func(fd, e->flags,
						t->num_children, t->timeout);
				}
			}
		}
	}

	igt_subtest_group {
		igt_fixture {
			gem_require_contexts(fd);
			igt_require(gem_scheduler_has_ctx_priority(fd));
			igt_require(gem_scheduler_has_preemption(fd));
		}

		igt_subtest("preempt-all")
			preempt(fd, ALL_ENGINES, 1, 20);
		igt_subtest_with_dynamic("preempt") {
			__for_each_physical_engine(fd, e) {
				igt_dynamic_f("%s", e->name)
					preempt(fd, e->flags, ncpus, 20);
			}
		}
	}

	igt_fixture {
		igt_stop_hang_detector();
		close(fd);
	}
}
