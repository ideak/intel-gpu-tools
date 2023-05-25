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

#include "igt.h"
#include "igt_kmod.h"
/**
 * TEST: i915 selftest
 * Description: Basic unit tests for i915.ko
 *
 * SUBTEST: live
 * Feature: gem_core
 * Run type: BAT
 *
 * SUBTEST: live@active
 * Category: Selftest
 * Functionality: semaphore
 * Sub-category: i915
 * Test category: GEM_Legacy
 *
 * SUBTEST: live@blt
 * Category: Selftest
 * Description: Blitter validation
 * Functionality: command streamer
 * Sub-category: i915 / HW
 * Test category: GEM_Legacy
 *
 * SUBTEST: live@client
 * Category: Selftest
 * Description: Internal API over blitter
 * Functionality: API checks
 * Sub-category: i915
 * Test category: GEM_Legacy
 *
 * SUBTEST: live@coherency
 * Category: Selftest
 * Description: Cache management
 * Functionality: memory management
 * Sub-category: i915 / HW
 * Test category: GEM_Legacy
 *
 * SUBTEST: live@debugger
 * Category: Selftest
 * Functionality: device management
 * Sub-category: debugger
 * Test category: GEM_Legacy
 *
 * SUBTEST: live@display
 * Category: Selftest
 * Functionality: display sanity
 * Sub-category: i915
 * Test category: GEM_Legacy
 *
 * SUBTEST: live@dmabuf
 * Category: Selftest
 * Functionality: buffer management
 * Sub-category: i915
 * Test category: GEM_Legacy
 *
 * SUBTEST: live@evict
 * Category: Selftest
 * Functionality: GTT eviction
 * Sub-category: i915
 * Test category: GEM_Legacy
 *
 * SUBTEST: live@execlists
 * Category: Selftest
 * Description: command submission backend
 * Functionality: command submission
 * Sub-category: i915
 * Test category: GEM_Legacy
 *
 * SUBTEST: live@gem
 * Category: Selftest
 * Functionality: command submission
 * Sub-category: i915
 * Test category: GEM_Legacy
 *
 * SUBTEST: live@gem_contexts
 * Category: Selftest
 * Description: User isolation and execution at the context level
 * Functionality: context management
 * Sub-category: i915 / HW
 * Test category: GEM_Legacy
 *
 * SUBTEST: live@gem_execbuf
 * Category: Selftest
 * Description: command submission support
 * Functionality: command submission
 * Sub-category: i915
 * Test category: GEM_Legacy
 *
 * SUBTEST: live@gt_ccs_mode
 * Category: Selftest
 * Description: Multi-ccs internal validation
 * Functionality: multii-ccs
 * Sub-category: i915
 * Test category: GEM_Legacy
 *
 * SUBTEST: live@gt_contexts
 * Category: Selftest
 * Description: HW isolation and HW context validation
 * Functionality: context management
 * Sub-category: HW
 * Test category: GEM_Legacy
 *
 * SUBTEST: live@gt_engines
 * Category: Selftest
 * Description: command submission topology validation
 * Functionality: command submission
 * Sub-category: i915
 * Test category: GEM_Legacy
 *
 * SUBTEST: live@gt_gtt
 * Category: Selftest
 * Description: Validation of virtual address management and execution
 * Functionality: memory management
 * Sub-category: HW
 * Test category: GEM_Legacy
 *
 * SUBTEST: live@gt_heartbeat
 * Category: Selftest
 * Description: Stall detection interface validation
 * Functionality: reset
 * Sub-category: i915
 * Test category: GEM_Legacy
 *
 * SUBTEST: live@gt_lrc
 * Category: Selftest
 * Description: HW isolation and HW context validation
 * Functionality: context management
 * Sub-category: HW
 * Test category: GEM_Legacy
 *
 * SUBTEST: live@gt_mocs
 * Category: Selftest
 * Description: Verification of mocs registers
 * Functionality: mocs
 * Sub-category: i915 / HW
 * Test category: GEM_Legacy
 *
 * SUBTEST: live@gt_pm
 * Category: Selftest
 * Description: Basic i915 driver module selftests
 * Feature: rps, rc6
 * Test category: rps, rc6
 *
 * SUBTEST: live@gt_timelines
 * Category: Selftest
 * Description: semaphore tracking
 * Functionality: semaphore
 * Sub-category: i915
 * Test category: GEM_Legacy
 *
 * SUBTEST: live@gt_tlb
 * Category: Selftest
 * Test category: Memory Management
 *
 * SUBTEST: live@gtt
 * Category: Selftest
 * Description: Virtual address management interface validation
 * Functionality: memory management
 * Sub-category: i915
 * Test category: GEM_Legacy
 *
 * SUBTEST: live@gtt_l4wa
 * Category: Selftest
 * Description: Check the L4WA is enabled when it was required
 * Functionality: workarounds
 * Sub-category: i915
 * Test category: GEM_Legacy
 *
 * SUBTEST: live@guc
 * Category: Selftest
 * Feature: GuC
 * Functionality: Guc specific selftests
 * Test category: GuC
 *
 * SUBTEST: live@guc_doorbells
 * Category: Selftest
 * Feature: GuC
 * Functionality: Guc specific selftests
 * Test category: GuC
 *
 * SUBTEST: live@guc_hang
 * Category: Selftest
 * Feature: GuC
 * Functionality: Guc specific selftests
 * Test category: GuC
 *
 * SUBTEST: live@guc_multi_lrc
 * Category: Selftest
 * Feature: GuC
 * Functionality: Guc specific selftests
 * Test category: GuC
 *
 * SUBTEST: live@hangcheck
 * Category: Selftest
 * Description: reset handling after stall detection
 * Functionality: reset
 * Sub-category: i915
 * Test category: GEM_Legacy
 *
 * SUBTEST: live@hugepages
 * Category: Selftest
 * Description: Large page support validation
 * Functionality: memory management
 * Sub-category: i915
 * Test category: GEM_Legacy
 *
 * SUBTEST: live@late_gt_pm
 * Category: Selftest
 * Feature: rc6
 * Functionality: Basic i915 driver module selftests
 * Test category: rc6
 *
 * SUBTEST: live@lmem
 * Category: Selftest
 *
 * SUBTEST: live@memory_region
 * Category: Selftest
 * Description: memory topology validation and migration checks
 * Functionality: memory management
 * Sub-category: i915 / HW
 * Test category: GEM_Legacy
 *
 * SUBTEST: live@memory_region_cross_tile
 * Category: Selftest
 * Functionality: Multi-tile memory topology validation
 * Test category: MultiTile
 *
 * SUBTEST: live@mman
 * Category: Selftest
 * Description: memory management validation
 * Functionality: memory management
 * Sub-category: i915
 * Test category: GEM_Legacy
 *
 * SUBTEST: live@obj_lock
 * Category: Selftest
 * Description: Validation of per-object locking patterns
 * Functionality: per-object lockling
 * Sub-category: i915
 * Test category: GEM_Legacy
 *
 * SUBTEST: live@objects
 * Category: Selftest
 * Description: User object allocation and isolation checks
 * Functionality: buffer management
 * Sub-category: i915
 * Test category: GEM_Legacy
 *
 * SUBTEST: live@perf
 * Category: Selftest
 * Feature: i915 perf selftests
 * Functionality: Basic i915 module perf unit selftests
 * Test category: Perf
 *
 * SUBTEST: live@remote_tiles
 * Category: Selftest
 * Functionality: Tile meta data validation
 * Test category: MultiTile
 *
 * SUBTEST: live@requests
 * Category: Selftest
 * Description: Validation of internal i915 command submission interface
 * Functionality: command submission
 * Sub-category: i915
 * Test category: GEM_Legacy
 *
 * SUBTEST: live@reset
 * Category: Selftest
 * Description: engine/GT resets
 * Functionality: reset
 * Sub-category: HW
 * Test category: GEM_Legacy
 *
 * SUBTEST: live@sanitycheck
 * Category: Selftest
 * Description: Checks the selftest infrastructure itself
 * Functionality: selftests
 * Sub-category: i915
 * Test category: GEM_Legacy
 *
 * SUBTEST: live@scheduler
 * Category: Selftest
 * Test category: Cmd Submission
 *
 * SUBTEST: live@semaphores
 * Category: Selftest
 * Description: GuC semaphore management
 * Functionality: semaphore
 * Sub-category: HW
 * Test category: GEM_Legacy
 *
 * SUBTEST: live@slpc
 * Category: Selftest
 * Feature: slpc / pm_rps
 * Functionality: Basic i915 driver module selftests
 * Test category: slpc / pm_rps
 *
 * SUBTEST: live@uncore
 * Category: Selftest
 * Description: Basic i915 driver module selftests
 * Feature: forcewake
 * Test category: forcewake
 *
 * SUBTEST: live@vma
 * Category: Selftest
 * Description: Per-object virtual address management
 * Functionality: memory management
 * Sub-category: i915
 * Test category: GEM_Legacy
 *
 * SUBTEST: live@win_blt_copy
 * Category: Selftest
 * Description: Validation of migration interface
 * Functionality: migration interface
 * Sub-category: i915 / HW
 * Test category: GEM_Legacy
 *
 * SUBTEST: live@workarounds
 * Category: Selftest
 * Description: Check workarounds persist or are reapplied after resets and other power management events
 * Functionality: workarounds
 * Sub-category: HW
 * Test category: GEM_Legacy
 *
 * SUBTEST: mock
 * Feature: gem_core
 * Run type: FULL
 *
 * SUBTEST: mock@buddy
 * Category: Selftest
 * Description: Buddy allocation
 * Functionality: memory management
 * Sub-category: DRM
 * Test category: GEM_Legacy
 *
 * SUBTEST: mock@contexts
 * Category: Selftest
 * Description: GEM context internal API checks
 * Functionality: API checks
 * Sub-category: i915
 * Test category: GEM_Legacy
 *
 * SUBTEST: mock@dmabuf
 * Category: Selftest
 * Description: dma-buf (buffer management) API checks
 * Functionality: API checks
 * Sub-category: DRM
 * Test category: GEM_Legacy
 *
 * SUBTEST: mock@engine
 * Category: Selftest
 * Description: Engine topology API checks
 * Functionality: API checks
 * Sub-category: i915
 * Test category: GEM_Legacy
 *
 * SUBTEST: mock@evict
 * Category: Selftest
 * Description: GTT eviction API checks
 * Functionality: API checks
 * Sub-category: i915
 * Test category: GEM_Legacy
 *
 * SUBTEST: mock@fence
 * Category: Selftest
 * Description: semaphore API checks
 * Functionality: API checks
 * Sub-category: i915
 * Test category: GEM_Legacy
 *
 * SUBTEST: mock@gtt
 * Category: Selftest
 * Description: Virtual address management API checks
 * Functionality: API checks
 * Sub-category: i915
 * Test category: GEM_Legacy
 *
 * SUBTEST: mock@hugepages
 * Category: Selftest
 * Description: Hugepage API checks
 * Functionality: API checks
 * Sub-category: i915
 * Test category: GEM_Legacy
 *
 * SUBTEST: mock@memory_region
 * Category: Selftest
 * Description: Memory region API checks
 * Functionality: API checks
 * Sub-category: i915
 * Test category: GEM_Legacy
 *
 * SUBTEST: mock@objects
 * Category: Selftest
 * Description: Buffer object API checks
 * Functionality: API checks
 * Sub-category: i915
 * Test category: GEM_Legacy
 *
 * SUBTEST: mock@phys
 * Category: Selftest
 * Description: legacy physical object API checks
 * Functionality: API checks
 * Sub-category: i915
 * Test category: GEM_Legacy
 *
 * SUBTEST: mock@requests
 * Category: Selftest
 * Description: Internal command submission API checks
 * Functionality: API checks
 * Sub-category: i915
 * Test category: GEM_Legacy
 *
 * SUBTEST: mock@ring
 * Category: Selftest
 * Description: Ringbuffer management API checks
 * Functionality: API checks
 * Sub-category: i915
 * Test category: GEM_Legacy
 *
 * SUBTEST: mock@sanitycheck
 * Category: Selftest
 * Description: Selftest for the selftest
 * Functionality: selftests
 * Sub-category: i915
 * Test category: GEM_Legacy
 *
 * SUBTEST: mock@scatterlist
 * Category: Selftest
 * Description: Scatterlist API checks
 * Functionality: API checks
 * Sub-category: i915
 * Test category: GEM_Legacy
 *
 * SUBTEST: mock@shmem
 * Category: Selftest
 * Description: SHM utils API checks
 * Functionality: API checks
 * Sub-category: i915
 * Test category: GEM_Legacy
 *
 * SUBTEST: mock@syncmap
 * Category: Selftest
 * Description: API checks for the contracted radixtree
 * Functionality: API checks
 * Sub-category: i915
 * Test category: GEM_Legacy
 *
 * SUBTEST: mock@timelines
 * Category: Selftest
 * Description: API checks for semaphore tracking
 * Functionality: API checks
 * Sub-category: i915
 * Test category: GEM_Legacy
 *
 * SUBTEST: mock@tlb
 * Category: Selftest
 * Test category: Memory Management
 *
 * SUBTEST: mock@uncore
 * Category: Selftest
 * Description: Basic i915 driver module selftests
 * Feature: forcewake
 * Test category: forcewake
 *
 * SUBTEST: mock@vma
 * Category: Selftest
 * Description: API checks for virtual address management
 * Functionality: API checks
 * Sub-category: i915
 * Test category: GEM_Legacy
 *
 * SUBTEST: perf
 * Feature: oa
 * Run type: FULL
 *
 * SUBTEST: perf@blt
 * Category: Selftest
 * Feature: i915 perf selftests
 * Functionality: Basic i915 module perf unit selftests
 * Test category: Perf
 *
 * SUBTEST: perf@engine_cs
 * Category: Selftest
 * Feature: i915 perf selftests
 * Functionality: Basic i915 module perf unit selftests
 * Test category: Perf
 *
 * SUBTEST: perf@region
 * Category: Selftest
 * Feature: i915 perf selftests
 * Functionality: Basic i915 module perf unit selftests
 * Test category: Perf
 *
 * SUBTEST: perf@request
 * Category: Selftest
 * Functionality: Basic i915 module perf unit selftests
 * Test category: Perf
 *
 * SUBTEST: perf@scheduler
 * Category: Selftest
 * Functionality: Basic i915 module perf unit selftests
 * Test category: Perf
 */

IGT_TEST_DESCRIPTION("Basic unit tests for i915.ko");

igt_main
{
	const char *env = getenv("SELFTESTS") ?: "";
	char opts[1024];

	igt_assert(snprintf(opts, sizeof(opts),
			    "mock_selftests=-1 disable_display=1 st_filter=%s",
			    env) < sizeof(opts));
	igt_kselftests("i915", opts, NULL, "mock");

	igt_assert(snprintf(opts, sizeof(opts),
			    "live_selftests=-1 disable_display=1 st_filter=%s",
			    env) < sizeof(opts));
	igt_kselftests("i915", opts, "live_selftests", "live");

	igt_assert(snprintf(opts, sizeof(opts),
			    "perf_selftests=-1 disable_display=1 st_filter=%s",
			    env) < sizeof(opts));
	igt_kselftests("i915", opts, "perf_selftests", "perf");
}
