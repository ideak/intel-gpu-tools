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
#include <sys/time.h>
#include <sys/signal.h>
#include <time.h>

#include "drm.h"

#include "igt_sysfs.h"
#include "igt_vgem.h"

#define LOCAL_I915_EXEC_NO_RELOC (1<<11)
#define LOCAL_I915_EXEC_HANDLE_LUT (1<<12)

#define LOCAL_I915_EXEC_BSD_SHIFT      (13)
#define LOCAL_I915_EXEC_BSD_MASK       (3 << LOCAL_I915_EXEC_BSD_SHIFT)

#define ENGINE_FLAGS  (I915_EXEC_RING_MASK | LOCAL_I915_EXEC_BSD_MASK)

#define CORK 0x1
#define PREEMPT 0x2

static unsigned int ring_size;

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

static void alarm_handler(int sig)
{
}

static void set_timeout(int seconds)
{
	struct sigaction sa = { .sa_handler = alarm_handler };

	sigaction(SIGALRM, seconds ? &sa : NULL, NULL);
	alarm(seconds);
}

static int __execbuf(int fd, struct drm_i915_gem_execbuffer2 *execbuf)
{
	return ioctl(fd, DRM_IOCTL_I915_GEM_EXECBUFFER2, execbuf);
}

static unsigned int measure_ring_size(int fd)
{
	struct drm_i915_gem_exec_object2 obj[2];
	struct drm_i915_gem_execbuffer2 execbuf;
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	unsigned int count;
	struct cork c;

	memset(obj, 0, sizeof(obj));
	obj[1].handle = gem_create(fd, 4096);
	gem_write(fd, obj[1].handle, 0, &bbe, sizeof(bbe));

	plug(fd, &c);
	obj[0].handle = c.handle;

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(obj);
	execbuf.buffer_count = 2;

	count = 0;
	set_timeout(1);
	while (__execbuf(fd, &execbuf) == 0)
		count++;
	set_timeout(0);

	unplug(&c);
	gem_close(fd, obj[1].handle);

	return count;
}

#define RCS_TIMESTAMP (0x2000 + 0x358)
static void latency_on_ring(int fd,
			    unsigned ring, const char *name,
			    unsigned flags)
{
	const int gen = intel_gen(intel_get_drm_devid(fd));
	const int has_64bit_reloc = gen >= 8;
	struct drm_i915_gem_exec_object2 obj[3];
	struct drm_i915_gem_relocation_entry reloc;
	struct drm_i915_gem_execbuffer2 execbuf;
	struct cork c;
	volatile uint32_t *reg;
	unsigned repeats = ring_size;
	uint32_t start, end, *map, *results;
	uint64_t offset;
	double gpu_latency;
	int i, j;

	reg = (volatile uint32_t *)((volatile char *)igt_global_mmio + RCS_TIMESTAMP);

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(&obj[1]);
	execbuf.buffer_count = 2;
	execbuf.flags = ring;
	execbuf.flags |= LOCAL_I915_EXEC_NO_RELOC | LOCAL_I915_EXEC_HANDLE_LUT;

	memset(obj, 0, sizeof(obj));
	obj[1].handle = gem_create(fd, 4096);
	obj[1].flags = EXEC_OBJECT_WRITE;
	results = gem_mmap__wc(fd, obj[1].handle, 0, 4096, PROT_READ);

	obj[2].handle = gem_create(fd, 64*1024);
	map = gem_mmap__wc(fd, obj[2].handle, 0, 64*1024, PROT_WRITE);
	gem_set_domain(fd, obj[2].handle,
		       I915_GEM_DOMAIN_GTT,
		       I915_GEM_DOMAIN_GTT);
	map[0] = MI_BATCH_BUFFER_END;
	gem_execbuf(fd, &execbuf);

	memset(&reloc,0, sizeof(reloc));
	obj[2].relocation_count = 1;
	obj[2].relocs_ptr = to_user_pointer(&reloc);

	gem_set_domain(fd, obj[2].handle,
		       I915_GEM_DOMAIN_GTT,
		       I915_GEM_DOMAIN_GTT);

	reloc.target_handle = flags & CORK ? 1 : 0;
	reloc.read_domains = I915_GEM_DOMAIN_INSTRUCTION;
	reloc.write_domain = I915_GEM_DOMAIN_INSTRUCTION;
	reloc.presumed_offset = obj[1].offset;

	for (j = 0; j < repeats; j++) {
		execbuf.batch_start_offset = 64 * j;
		reloc.offset =
			execbuf.batch_start_offset + sizeof(uint32_t);
		reloc.delta = sizeof(uint32_t) * j;

		offset = reloc.presumed_offset;
		offset += reloc.delta;

		i = 16 * j;
		/* MI_STORE_REG_MEM */
		map[i++] = 0x24 << 23 | 1;
		if (has_64bit_reloc)
			map[i-1]++;
		map[i++] = RCS_TIMESTAMP; /* ring local! */
		map[i++] = offset;
		if (has_64bit_reloc)
			map[i++] = offset >> 32;
		map[i++] = MI_BATCH_BUFFER_END;
	}

	if (flags & CORK) {
		plug(fd, &c);
		obj[0].handle = c.handle;
		execbuf.buffers_ptr = to_user_pointer(&obj[0]);
		execbuf.buffer_count = 3;
	}

	start = *reg;
	for (j = 0; j < repeats; j++) {
		uint64_t presumed_offset = reloc.presumed_offset;

		execbuf.batch_start_offset = 64 * j;
		reloc.offset =
			execbuf.batch_start_offset + sizeof(uint32_t);
		reloc.delta = sizeof(uint32_t) * j;

		gem_execbuf(fd, &execbuf);
		igt_assert(reloc.presumed_offset == presumed_offset);
	}
	end = *reg;
	igt_assert(reloc.presumed_offset == obj[1].offset);

	if (flags & CORK)
		unplug(&c);

	gem_set_domain(fd, obj[1].handle, I915_GEM_DOMAIN_GTT, 0);
	gpu_latency = (results[repeats-1] - results[0]) / (double)(repeats-1);

	gem_set_domain(fd, obj[2].handle,
		       I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);

	execbuf.batch_start_offset = 0;
	for (j = 0; j < repeats - 1; j++) {
		offset = obj[2].offset;
		offset += 64 * (j + 1);

		i = 16 * j + (has_64bit_reloc ? 4 : 3);
		map[i] = MI_BATCH_BUFFER_START;
		if (gen >= 8) {
			map[i] |= 1 << 8 | 1;
			map[i + 1] = offset;
			map[i + 2] = offset >> 32;
		} else if (gen >= 6) {
			map[i] |= 1 << 8;
			map[i + 1] = offset;
		} else {
			map[i] |= 2 << 6;
			map[i + 1] = offset;
			if (gen < 4)
				map[i] |= 1;
		}
	}
	offset = obj[2].offset;
	gem_execbuf(fd, &execbuf);
	igt_assert(offset == obj[2].offset);

	gem_set_domain(fd, obj[1].handle, I915_GEM_DOMAIN_GTT, 0);
	igt_info("%s: dispatch latency: %.2f, execution latency: %.2f (target %.2f)\n",
		 name,
		 (end - start) / (double)repeats,
		 gpu_latency, (results[repeats - 1] - results[0]) / (double)(repeats - 1));

	munmap(map, 64*1024);
	munmap(results, 4096);
	gem_close(fd, obj[1].handle);
	gem_close(fd, obj[2].handle);
}

static void latency_from_ring(int fd,
			      unsigned ring, const char *name,
			      unsigned flags)
{
	const struct intel_execution_engine *e;
	const int gen = intel_gen(intel_get_drm_devid(fd));
	const int has_64bit_reloc = gen >= 8;
	struct drm_i915_gem_exec_object2 obj[3];
	struct drm_i915_gem_relocation_entry reloc;
	struct drm_i915_gem_execbuffer2 execbuf;
	const unsigned int repeats = ring_size / 2;
	uint32_t *map, *results;
	uint32_t ctx[2] = {};
	int i, j;

	if (flags & PREEMPT) {
		ctx[0] = gem_context_create(fd);
		gem_context_set_priority(fd, ctx[0], -1023);

		ctx[1] = gem_context_create(fd);
		gem_context_set_priority(fd, ctx[1], 1023);
	}

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(&obj[1]);
	execbuf.buffer_count = 2;
	execbuf.flags = ring;
	execbuf.flags |= LOCAL_I915_EXEC_NO_RELOC | LOCAL_I915_EXEC_HANDLE_LUT;
	execbuf.rsvd1 = ctx[1];

	memset(obj, 0, sizeof(obj));
	obj[1].handle = gem_create(fd, 4096);
	obj[1].flags = EXEC_OBJECT_WRITE;
	results = gem_mmap__wc(fd, obj[1].handle, 0, 4096, PROT_READ);

	obj[2].handle = gem_create(fd, 64*1024);
	map = gem_mmap__wc(fd, obj[2].handle, 0, 64*1024, PROT_WRITE);
	gem_set_domain(fd, obj[2].handle,
		       I915_GEM_DOMAIN_GTT,
		       I915_GEM_DOMAIN_GTT);
	map[0] = MI_BATCH_BUFFER_END;
	gem_execbuf(fd, &execbuf);

	memset(&reloc,0, sizeof(reloc));
	obj[2].relocation_count = 1;
	obj[2].relocs_ptr = to_user_pointer(&reloc);

	gem_set_domain(fd, obj[2].handle,
		       I915_GEM_DOMAIN_GTT,
		       I915_GEM_DOMAIN_GTT);

	reloc.read_domains = I915_GEM_DOMAIN_INSTRUCTION;
	reloc.write_domain = I915_GEM_DOMAIN_INSTRUCTION;
	reloc.presumed_offset = obj[1].offset;
	reloc.target_handle = flags & CORK ? 1 : 0;

	for (e = intel_execution_engines; e->name; e++) {
		igt_spin_t *spin = NULL;
		struct cork c;

		if (e->exec_id == 0)
			continue;

		if (!gem_has_ring(fd, e->exec_id | e->flags))
			continue;

		gem_set_domain(fd, obj[2].handle,
			       I915_GEM_DOMAIN_GTT,
			       I915_GEM_DOMAIN_GTT);

		if (flags & PREEMPT)
			spin = igt_spin_batch_new(fd, ctx[0], ring, 0);

		if (flags & CORK) {
			plug(fd, &c);
			obj[0].handle = c.handle;
			execbuf.buffers_ptr = to_user_pointer(&obj[0]);
			execbuf.buffer_count = 3;
		}

		for (j = 0; j < repeats; j++) {
			uint64_t offset;

			execbuf.flags &= ~ENGINE_FLAGS;
			execbuf.flags |= ring;

			execbuf.batch_start_offset = 64 * j;
			reloc.offset =
				execbuf.batch_start_offset + sizeof(uint32_t);
			reloc.delta = sizeof(uint32_t) * j;

			reloc.presumed_offset = obj[1].offset;
			offset = reloc.presumed_offset;
			offset += reloc.delta;

			i = 16 * j;
			/* MI_STORE_REG_MEM */
			map[i++] = 0x24 << 23 | 1;
			if (has_64bit_reloc)
				map[i-1]++;
			map[i++] = RCS_TIMESTAMP; /* ring local! */
			map[i++] = offset;
			if (has_64bit_reloc)
				map[i++] = offset >> 32;
			map[i++] = MI_BATCH_BUFFER_END;

			gem_execbuf(fd, &execbuf);

			execbuf.flags &= ~ENGINE_FLAGS;
			execbuf.flags |= e->exec_id | e->flags;

			execbuf.batch_start_offset = 64 * (j + repeats);
			reloc.offset =
				execbuf.batch_start_offset + sizeof(uint32_t);
			reloc.delta = sizeof(uint32_t) * (j + repeats);

			reloc.presumed_offset = obj[1].offset;
			offset = reloc.presumed_offset;
			offset += reloc.delta;

			i = 16 * (j + repeats);
			/* MI_STORE_REG_MEM */
			map[i++] = 0x24 << 23 | 1;
			if (has_64bit_reloc)
				map[i-1]++;
			map[i++] = RCS_TIMESTAMP; /* ring local! */
			map[i++] = offset;
			if (has_64bit_reloc)
				map[i++] = offset >> 32;
			map[i++] = MI_BATCH_BUFFER_END;

			gem_execbuf(fd, &execbuf);
		}

		if (flags & CORK)
			unplug(&c);
		gem_set_domain(fd, obj[1].handle,
			       I915_GEM_DOMAIN_GTT,
			       I915_GEM_DOMAIN_GTT);
		igt_spin_batch_free(fd, spin);

		igt_info("%s-%s delay: %.2f\n",
			 name, e->name, (results[2*repeats-1] - results[0]) / (double)repeats);
	}

	munmap(map, 64*1024);
	munmap(results, 4096);
	gem_close(fd, obj[1].handle);
	gem_close(fd, obj[2].handle);

	if (flags & PREEMPT) {
		gem_context_destroy(fd, ctx[1]);
		gem_context_destroy(fd, ctx[0]);
	}
}

igt_main
{
	const struct intel_execution_engine *e;
	int device = -1;

	igt_fixture {
		device = drm_open_driver(DRIVER_INTEL);
		igt_require_gem(device);
		gem_require_mmap_wc(device);

		gem_submission_print_method(device);

		ring_size = measure_ring_size(device);
		igt_info("Ring size: %d batches\n", ring_size);
		igt_require(ring_size > 8);
		ring_size -= 8; /* leave some spare */
		if (ring_size > 1024)
			ring_size = 1024;

		intel_register_access_init(intel_get_pci_device(), false, device);
	}

	igt_subtest_group {
		igt_fixture
			igt_require(intel_gen(intel_get_drm_devid(device)) >= 7);

		for (e = intel_execution_engines; e->name; e++) {
			if (e->exec_id == 0)
				continue;

			igt_subtest_group {
				igt_fixture {
					gem_require_ring(device, e->exec_id | e->flags);
				}

				igt_subtest_f("%s-dispatch", e->name)
					latency_on_ring(device,
							e->exec_id | e->flags,
							e->name, 0);

				igt_subtest_f("%s-dispatch-queued", e->name)
					latency_on_ring(device,
							e->exec_id | e->flags,
							e->name, CORK);

				igt_subtest_f("%s-synchronisation", e->name)
					latency_from_ring(device,
							  e->exec_id | e->flags,
							  e->name, 0);

				igt_subtest_f("%s-synchronisation-queued", e->name)
					latency_from_ring(device,
							  e->exec_id | e->flags,
							  e->name, CORK);

				igt_subtest_group {
					igt_fixture {
						gem_require_contexts(device);
						igt_require(gem_scheduler_has_preemption(device));
					}

					igt_subtest_f("%s-preemption", e->name)
						latency_from_ring(device,
								  e->exec_id | e->flags,
								  e->name, PREEMPT);
				}
			}
		}
	}

	igt_fixture {
		close(device);
	}
}
