/*
 * Copyright © 2014 Intel Corporation
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

/*
 * Testcase: Exercise a suspect workaround required for FORCEWAKE_MT
 *
 */

#include "igt.h"
#include <sys/types.h>
#include <pthread.h>
#include <string.h>

#include "drm.h"

IGT_TEST_DESCRIPTION("Exercise a suspect workaround required for"
		     " FORCEWAKE_MT.");

#define FORCEWAKE_MT 0xa188
#define READ_ONCE(x) (*(volatile typeof(x) *)(&(x)))

struct thread {
	pthread_t thread;
	pthread_mutex_t *lock;
	volatile uint32_t *forcewake_mt;
	int fd;
	int bit;
	bool done;
};

static const struct pci_id_match match[] = {
	INTEL_IVB_D_IDS(NULL),
	INTEL_IVB_M_IDS(NULL),

	INTEL_HSW_IDS(NULL),

	{ 0, 0, 0 },
};

static struct pci_device *__igfx_get(void)
{
	struct pci_device *dev;

	if (pci_system_init())
		return 0;

	dev = pci_device_find_by_slot(0, 0, 2, 0);
	if (dev == NULL || dev->vendor_id != 0x8086) {
		struct pci_device_iterator *iter;

		iter = pci_id_match_iterator_create(match);
		if (!iter)
			return 0;

		dev = pci_device_next(iter);
		pci_iterator_destroy(iter);
	}

	pci_device_probe(dev);
	return dev;
}

static volatile uint32_t *igfx_mmio_forcewake_mt(void)
{
	struct pci_device *pci = __igfx_get();

	igt_require(pci && intel_gen(pci->device_id) == 7);

	intel_mmio_use_pci_bar(pci);

	return (volatile uint32_t *)((char *)igt_global_mmio + FORCEWAKE_MT);
}

static void *thread(void *arg)
{
	static const char acquire_error[] = "acquire";
	static const char release_error[] = "release";

	struct thread *t = arg;
	const uint32_t bit = 1 << t->bit;
	volatile uint32_t *forcewake_mt = t->forcewake_mt;
	void *result = NULL;

	while (!result && !READ_ONCE(t->done)) {
		/*
		 * The HW is fubar; concurrent mmio access to even
		 * the FORCEWAKE_MT results in a machine lockup, nullifying
		 * the entire purpose of FORCEWAKE_MT... Sigh.
		 */
		pthread_mutex_lock(t->lock);

		*forcewake_mt = bit << 16 | bit;
		if (!igt_wait(*forcewake_mt & bit, 50, 1))
			result = (void *)acquire_error;

		/* Sleep to let another thread poke at a different bit */
		pthread_mutex_unlock(t->lock);
		usleep(1000);
		pthread_mutex_lock(t->lock);

		*forcewake_mt = bit << 16;
		if (!igt_wait((*forcewake_mt & bit) == 0, 50, 1))
			result = (void *)release_error;

		pthread_mutex_unlock(t->lock);
	}

	return result;
}

#define MI_STORE_REGISTER_MEM                   (0x24<<23)

igt_simple_main
{
	struct thread t[16];
	pthread_mutex_t lock;
	bool success = true;
	int i;

	igt_assert(pthread_mutex_init(&lock, NULL) == 0);

	t[0].lock = &lock;
	t[0].fd = drm_open_driver(DRIVER_INTEL);
	t[0].forcewake_mt = igfx_mmio_forcewake_mt();
	t[0].done = false;

	for (i = 2; i < 16; i++) {
		t[i] = t[0];
		t[i].bit = i;
		if (pthread_create(&t[i].thread, NULL, thread, &t[i])) {
			igt_warn("Failed to create thread for BIT(%d)\n", i);
			success = false;
			goto error;
		}
	}

	sleep(2);

	igt_until_timeout(2) {
		uint32_t *p;
		struct drm_i915_gem_execbuffer2 execbuf;
		struct drm_i915_gem_exec_object2 exec[2];
		struct drm_i915_gem_relocation_entry reloc[2];
		uint32_t b[] = {
			MI_LOAD_REGISTER_IMM,
			FORCEWAKE_MT,
			2 << 16 | 2,
			MI_STORE_REGISTER_MEM | 1,
			FORCEWAKE_MT,
			0, // to be patched
			MI_LOAD_REGISTER_IMM,
			FORCEWAKE_MT,
			2 << 16,
			MI_STORE_REGISTER_MEM | 1,
			FORCEWAKE_MT,
			1 * sizeof(uint32_t), // to be patched
			MI_BATCH_BUFFER_END,
			0
		};

		memset(exec, 0, sizeof(exec));
		exec[1].handle = gem_create(t[0].fd, 4096);
		exec[1].relocation_count = 2;
		exec[1].relocs_ptr = (uintptr_t)reloc;
		gem_write(t[0].fd, exec[1].handle, 0, b, sizeof(b));
		exec[0].handle = gem_create(t[0].fd, 4096);

		reloc[0].offset = 5 * sizeof(uint32_t);
		reloc[0].delta = 0;
		reloc[0].target_handle = exec[0].handle;
		reloc[0].read_domains = I915_GEM_DOMAIN_RENDER;
		reloc[0].write_domain = I915_GEM_DOMAIN_RENDER;
		reloc[0].presumed_offset = 0;

		reloc[1].offset = 11 * sizeof(uint32_t);
		reloc[1].delta = 1 * sizeof(uint32_t);
		reloc[1].target_handle = exec[0].handle;
		reloc[1].read_domains = I915_GEM_DOMAIN_RENDER;
		reloc[1].write_domain = I915_GEM_DOMAIN_RENDER;
		reloc[1].presumed_offset = 0;

		memset(&execbuf, 0, sizeof(execbuf));
		execbuf.buffers_ptr = (uintptr_t)&exec;
		execbuf.buffer_count = 2;
		execbuf.batch_len = sizeof(b);
		execbuf.flags = I915_EXEC_SECURE;

		pthread_mutex_lock(t[0].lock);
		gem_execbuf(t[0].fd, &execbuf);
		gem_sync(t[0].fd, exec[1].handle);
		pthread_mutex_unlock(t[0].lock);

		p = gem_mmap__gtt(t[0].fd, exec[0].handle, 4096, PROT_READ);

		igt_debug("[%d]={ %08x %08x }\n", i, p[0], p[1]);
		if ((p[0] & 2) == 0) {
			igt_warn("Failed to acquire forcewake BIT(1) from batch\n");
			success = false;
		}
		if ((p[1] & 2)) {
			igt_warn("Failed to release forcewake BIT(1) from batch\n");
			success = false;
		}

		munmap(p, 4096);
		gem_close(t[0].fd, exec[0].handle);
		gem_close(t[0].fd, exec[1].handle);
		if (!success)
			break;

		usleep(1000);
	}

error:
	while (--i >= 2) {
		void *result = (char *)"pthread_join to";

		t[i].done = true;
		pthread_join(t[i].thread, &result);
		if (result) {
			igt_warn("Thread BIT(%d) failed to %s forcewake\n", i, (char *)result);
			success = false;
		}
	}

	/* And clear all forcewake bits before disappearing */
	*t[0].forcewake_mt = 0xfffe << 16;

	igt_assert(success);
}
