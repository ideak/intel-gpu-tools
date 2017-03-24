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

#define _GNU_SOURCE
#include <sched.h>

#include "igt.h"
#include "igt_debugfs.h"
#include "igt_sysfs.h"

IGT_TEST_DESCRIPTION("Inject missed interrupts and make sure they are caught");

static void trigger_missed_interrupt(int fd, unsigned ring)
{
	const int gen = intel_gen(intel_get_drm_devid(fd));
	struct drm_i915_gem_exec_object2 obj;
	struct drm_i915_gem_relocation_entry reloc;
	struct drm_i915_gem_execbuffer2 execbuf;
	uint32_t *batch;
	int i;

	memset(&obj, 0, sizeof(obj));
	obj.handle = gem_create(fd, 4096);
	obj.relocs_ptr = (uintptr_t)&reloc;
	obj.relocation_count = 1;

	memset(&reloc, 0, sizeof(reloc));
	reloc.target_handle = obj.handle; /* recurse */
	reloc.presumed_offset = 0;
	reloc.offset = sizeof(uint32_t);
	reloc.delta = 0;
	reloc.read_domains = I915_GEM_DOMAIN_COMMAND;
	reloc.write_domain = 0;

	batch = gem_mmap__wc(fd, obj.handle, 0, 4096, PROT_WRITE);
	gem_set_domain(fd, obj.handle,
			I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);

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
			reloc.delta = 1;
		}
	}
	batch[1000] = 1;

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = (uintptr_t)&obj;
	execbuf.buffer_count = 1;
	execbuf.flags = ring;

	execbuf.flags = ring;
	if (__gem_execbuf(fd, &execbuf))
		goto out;

	igt_fork(child, 1) {
		/* We are now a low priority child on the *same* CPU as the
		 * parent. We will have to wait for our parent to sleep
		 * (gem_sync -> i915_wait_request) before we run.
		 */
		igt_assert(*((volatile uint32_t *)batch + 1000) == 0);
		igt_assert(gem_bo_busy(fd, obj.handle));

		*batch = MI_BATCH_BUFFER_END;
		__sync_synchronize();
	}

	batch[1000] = 0;
	gem_sync(fd, obj.handle);
	igt_waitchildren();

out:
	gem_close(fd, obj.handle);
	munmap(batch, 4096);
}

static void bind_to_cpu(int cpu)
{
	struct sched_param rt = {.sched_priority = 99 };

	igt_assert(sched_setscheduler(getpid(), SCHED_RR | SCHED_RESET_ON_FORK, &rt) == 0);
}

static void enable_missed_irq(int dir)
{
	igt_sysfs_printf(dir, "i915_ring_test_irq", "0x%x", -1);
}

static uint32_t disable_missed_irq(int dir)
{
	uint32_t mask = 0;

	igt_sysfs_scanf(dir, "i915_ring_test_irq", "%x", &mask);
	igt_sysfs_set(dir, "i915_ring_test_irq", "0");

	return mask;
}

static uint32_t engine_mask(int dir)
{
	enable_missed_irq(dir);
	return disable_missed_irq(dir);
}

igt_simple_main
{
	const struct intel_execution_engine *e;
	unsigned expect_rings;
	unsigned missed_rings;
	unsigned check_rings;
	int debugfs, device;

	igt_skip_on_simulation();
	bind_to_cpu(0);

	device = drm_open_driver(DRIVER_INTEL);
	igt_require_gem(device);
	gem_require_mmap_wc(device);
	igt_fork_hang_detector(device);

	debugfs = igt_debugfs_dir(device);

	expect_rings = engine_mask(debugfs);

	igt_debug("Clearing rings %x\n", expect_rings);
	intel_detect_and_clear_missed_interrupts(device);
	for (e = intel_execution_engines; e->name; e++) {
		if (expect_rings == -1 && e->exec_id)
			continue;

		if (expect_rings != -1 && e->exec_id == 0)
			continue;

		igt_debug("Clearing ring %s [%x]\n",
			  e->name, e->exec_id | e->flags);
		trigger_missed_interrupt(device, e->exec_id | e->flags);
	}
	igt_assert_eq(intel_detect_and_clear_missed_interrupts(device), 0);

	igt_debug("Testing rings %x\n", expect_rings);
	enable_missed_irq(debugfs);
	for (e = intel_execution_engines; e->name; e++) {
		if (expect_rings == -1 && e->exec_id)
			continue;

		if (expect_rings != -1 && e->exec_id == 0)
			continue;

		igt_debug("Executing on ring %s [%x]\n",
			  e->name, e->exec_id | e->flags);
		trigger_missed_interrupt(device, e->exec_id | e->flags);
	}
	missed_rings = intel_detect_and_clear_missed_interrupts(device);

	check_rings = disable_missed_irq(debugfs);
	igt_assert_eq_u32(check_rings, expect_rings);

	if (expect_rings == -1)
		igt_assert_eq_u32(missed_rings, 1);
	else
		igt_assert_eq_u32(missed_rings, expect_rings);

	close(debugfs);
	igt_stop_hang_detector();
	close(device);
}
