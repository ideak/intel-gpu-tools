// SPDX-License-Identifier: MIT
/*
 * Copyright © 2021 Intel Corporation
 */

#include "i915/gem.h"
#include "i915/gem_create.h"
#include "i915/gem_vm.h"
#include "i915/intel_memory_region.h"
#include "igt.h"
#include "igt_kmod.h"
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
#include <sys/wait.h>
#include "drm.h"
#include "i915/i915_blt.h"
#include "i915/intel_mocs.h"

IGT_TEST_DESCRIPTION("Exercise local memory swapping.");

#define __round_mask(x, y) ((__typeof__(x))((y) - 1))
#define round_up(x, y) ((((x) - 1) | __round_mask(x, y)) + 1)

#define PAGE_SIZE  (1ULL << 12)
#define SZ_64K	   (16 * PAGE_SIZE)

static const char *readable_unit(uint64_t size)
{
	return size >> 20 ? "MiB" : size >> 10 ? "KiB" : "Bytes";
}

static uint64_t readable_size(uint64_t size)
{
	return size >> 20 ? size >> 20 : size >> 10 ? size >> 10 : size;
}

struct {
	unsigned int seed;
	bool user_seed;
} opt;

struct params {
	struct {
		uint64_t min;
		uint64_t max;
	} size;
	unsigned int count;
	unsigned int loops;
	unsigned int mem_limit;
#define TEST_VERIFY	(1 << 0)
#define TEST_PARALLEL	(1 << 1)
#define TEST_HEAVY	(1 << 2)
#define TEST_RANDOM	(1 << 3)
#define TEST_ENGINES	(1 << 4)
#define TEST_MULTI	(1 << 5)
#define TEST_CCS	(1 << 6)
#define TEST_MASSIVE	(1 << 7)
	unsigned int flags;
	unsigned int seed;
	bool oom_test;
};

struct object {
	uint64_t size;
	uint32_t seed;
	uint32_t handle;
	struct blt_copy_object *blt_obj;
};

static void set_object(struct blt_copy_object *obj,
		       uint32_t handle, uint64_t size, uint32_t region,
		       uint8_t mocs, enum blt_tiling tiling,
		       enum blt_compression compression,
		       enum blt_compression_type compression_type)
{
	obj->handle = handle;
	obj->size = size;
	obj->region = region;
	obj->mocs = mocs;
	obj->tiling = tiling;
	obj->compression = compression;
	obj->compression_type = compression_type;
}

static void set_geom(struct blt_copy_object *obj, uint32_t pitch,
		     int16_t x1, int16_t y1, int16_t x2, int16_t y2,
		     uint16_t x_offset, uint16_t y_offset)
{
	obj->pitch = pitch;
	obj->x1 = x1;
	obj->y1 = y1;
	obj->x2 = x2;
	obj->y2 = y2;
	obj->x_offset = x_offset;
	obj->y_offset = y_offset;
}

static void set_batch(struct blt_copy_batch *batch,
		      uint32_t handle, uint64_t size, uint32_t region)
{
	batch->handle = handle;
	batch->size = size;
	batch->region = region;
}

static void set_object_ext(struct blt_block_copy_object_ext *obj,
			   uint8_t compression_format,
			   uint16_t surface_width, uint16_t surface_height,
			   enum blt_surface_type surface_type)
{
	obj->compression_format = compression_format;
	obj->surface_width = surface_width;
	obj->surface_height = surface_height;
	obj->surface_type = surface_type;
}

static uint32_t create_bo(int i915,
			  uint64_t *size,
			  struct drm_i915_gem_memory_class_instance *region,
			  bool do_oom_test)
{
	uint32_t handle;
	int ret;

retry:
	ret = __gem_create_in_memory_region_list(i915, &handle, size, 0, region, 1);
	if (do_oom_test && (ret == -ENOMEM || ret == -ENXIO))
		goto retry;
	igt_assert_eq(ret, 0);
	return handle;
}

static unsigned int __num_engines__;

static void
init_object(int i915, struct object *obj, unsigned long seed, unsigned int flags)
{
	unsigned int j;
	uint32_t *buf;

	obj->seed = seed;

	buf = gem_mmap_offset__fixed(i915, obj->handle, 0, obj->size, PROT_WRITE);

	for (j = 0; j < obj->size / sizeof(*buf); j++)
		buf[j] = seed++;

	munmap(buf, obj->size);
}

static void
init_object_ccs(int i915, struct object *obj, struct blt_copy_object *tmp,
		unsigned long seed, const intel_ctx_t *ctx, uint32_t region,
		uint64_t ahnd)
{
	struct blt_block_copy_data_ext ext = {}, *pext = &ext;
	const struct intel_execution_engine2 *e;
	struct blt_copy_data blt = {};
	struct blt_copy_batch *cmd;
	uint64_t size = 4096;
	unsigned long *buf, j;

	obj->seed = seed;
	for_each_ctx_engine(i915, ctx, e) {
		if (gem_engine_can_block_copy(i915, e))
			break;
	}
	igt_assert_f(e, "Ctx don't have blt engine\n");

	cmd = calloc(1, sizeof(*cmd));
	igt_assert(cmd);
	cmd->handle = gem_create_from_pool(i915, &size, region);
	set_batch(cmd, cmd->handle, size, region);

	buf = gem_mmap__device_coherent(i915, tmp->handle, 0, obj->size, PROT_WRITE);
	gem_set_domain(i915, tmp->handle, I915_GEM_DOMAIN_WC, I915_GEM_DOMAIN_WC);

	for (j = 0; j < obj->size / sizeof(*buf); j++)
		buf[j] = seed++;
	munmap(buf, obj->size);

	memset(&blt, 0, sizeof(blt));
	blt.color_depth = CD_32bit;

	memcpy(&blt.src, tmp, sizeof(blt.src));
	memcpy(&blt.dst, obj->blt_obj, sizeof(blt.dst));
	memcpy(&blt.bb, cmd, sizeof(blt.bb));

	set_object_ext(&ext.src, 0, tmp->x2, tmp->y2, SURFACE_TYPE_2D);
	set_object_ext(&ext.dst, 0, obj->blt_obj->x2, obj->blt_obj->y2,
		       SURFACE_TYPE_2D);

	blt_block_copy(i915, ctx, e, ahnd, &blt, pext);
	free(cmd);
}

static void
verify_object(int i915, const struct object *obj,  unsigned int flags)
{
	unsigned long j;
	uint32_t *buf;

	buf = gem_mmap_offset__fixed(i915, obj->handle, 0, obj->size, PROT_READ);

	for (j = 0; j < obj->size / PAGE_SIZE; j++) {
		unsigned long x = (j * PAGE_SIZE + rand() % PAGE_SIZE) / sizeof(*buf);
		uint32_t val = obj->seed + x;

		igt_assert_f(buf[x] == val,
			     "Object mismatch at offset %zu - found %08x, expected %08x; difference:%08x!\n",
			     x * sizeof(*buf), buf[x], val, buf[x] ^ val);
	}

	munmap(buf, obj->size);
}

static void
verify_object_ccs(int i915, const struct object *obj,
		  struct blt_copy_object *tmp, const intel_ctx_t *ctx,
		  uint32_t region, uint64_t ahnd)
{
	struct blt_block_copy_data_ext ext = {}, *pext = &ext;
	const struct intel_execution_engine2 *e;
	struct blt_copy_data blt = {};
	struct blt_copy_batch *cmd;
	uint64_t size = 4096;
	unsigned long j, val, *buf;

	for_each_ctx_engine(i915, ctx, e) {
		if (gem_engine_can_block_copy(i915, e))
			break;
	}
	igt_assert_f(e, "Ctx don't have blt engine\n");

	cmd = calloc(1, sizeof(*cmd));
	igt_assert(cmd);
	cmd->handle = gem_create_from_pool(i915, &size, region);
	set_batch(cmd, cmd->handle, size, region);

	memset(&blt, 0, sizeof(blt));
	blt.color_depth = CD_32bit;

	memcpy(&blt.src, obj->blt_obj, sizeof(blt.src));
	memcpy(&blt.dst, tmp, sizeof(blt.dst));
	memcpy(&blt.bb, cmd, sizeof(blt.bb));

	blt.dst.x2 = min(obj->blt_obj->x2, tmp->x2);
	blt.dst.y2 = min(obj->blt_obj->y2, tmp->y2);

	set_object_ext(&ext.src, 0, obj->blt_obj->x2, obj->blt_obj->y2,
		       SURFACE_TYPE_2D);
	set_object_ext(&ext.dst, 0, tmp->x2, tmp->y2, SURFACE_TYPE_2D);
	blt_block_copy(i915, ctx, e, ahnd, &blt, pext);

	buf = gem_mmap__device_coherent(i915, tmp->handle, 0,
					obj->size, PROT_READ);
	gem_set_domain(i915, tmp->handle, I915_GEM_DOMAIN_WC, 0);

	for (j = 0; j < obj->size / PAGE_SIZE; j++) {
		unsigned long x = (j * PAGE_SIZE + rand() % PAGE_SIZE) / sizeof(*buf);

		val = obj->seed + x;
		igt_assert_f(buf[x] == val,
			     "Object mismatch at offset %lu - found %lx, expected %lx, difference:%lx!\n",
			     x * sizeof(*buf), buf[x], val, buf[x] ^ val);
	}

	munmap(buf, obj->size);
	free(cmd);
}

static void move_to_lmem(int i915,
			 const intel_ctx_t *ctx,
			 struct object *list,
			 unsigned int num,
			 unsigned int region,
			 unsigned int engine,
			 bool do_oom_test)
{
	struct drm_i915_gem_exec_object2 obj[1 + num];
	struct drm_i915_gem_execbuffer2 eb = {
		.buffers_ptr = to_user_pointer(obj),
		.buffer_count = 1 + num,
		.flags = I915_EXEC_NO_RELOC | I915_EXEC_HANDLE_LUT | engine,
		.rsvd1 = ctx->id,
	};
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	unsigned int i, ret, batch;
	uint64_t size;

	size = 4096;
	batch = gem_create_from_pool(i915, &size, region);
	gem_write(i915, batch, 0, &bbe, sizeof(bbe));

	memset(obj, 0, sizeof(obj));

	for (i = 0; i < num; i++) {
		obj[i].handle = list[i].handle;
		obj[i].flags = EXEC_OBJECT_SUPPORTS_48B_ADDRESS;
	}

	obj[i].handle = batch;
retry:
	ret = __gem_execbuf(i915, &eb);
	if (do_oom_test && (ret == -ENOMEM || ret == -ENXIO))
		goto retry;
	igt_assert_eq(ret, 0);
}

static void __do_evict(int i915,
		       const intel_ctx_t *ctx,
		       struct drm_i915_gem_memory_class_instance *region,
		       struct params *params,
		       unsigned int seed)
{
	uint32_t region_id = INTEL_MEMORY_REGION_ID(region->memory_class,
						    region->memory_instance);
	const unsigned int max_swap_in = params->count / 100 + 1;
	struct object *objects, *obj, *list;
	const uint32_t bpp = 32;
	uint32_t width, height, stride;
	const intel_ctx_t *blt_ctx;
	struct blt_copy_object *tmp;
	unsigned int engine = 0;
	unsigned int i, l;
	uint64_t size, ahnd;
	struct timespec t = {};
	unsigned int num;

	width = PAGE_SIZE / (bpp / 8);
	height = params->size.max / (bpp / 8) /  width;
	stride = width * 4;

	if (params->flags & TEST_CCS) {
		tmp = calloc(1, sizeof(*tmp));
		igt_assert(tmp);

		blt_ctx = intel_ctx_create(i915, &ctx->cfg);
		__gem_context_set_persistence(i915, blt_ctx->id, false);
	}

	objects = calloc(params->count, sizeof(*objects));
	igt_assert(objects);

	list = calloc(max_swap_in, sizeof(*list));
	igt_assert(list);

	srand(seed);

	/* Create the initial working set of objects. */
	if (params->flags & TEST_CCS) {
		ahnd = intel_allocator_open_full(i915, blt_ctx->id, 0, 0,
						 INTEL_ALLOCATOR_SIMPLE,
						 ALLOC_STRATEGY_LOW_TO_HIGH, 0);

		tmp->handle = gem_create_in_memory_regions(i915, params->size.max,
				   INTEL_MEMORY_REGION_ID(I915_SYSTEM_MEMORY, 0));
		set_object(tmp, tmp->handle, params->size.max,
			   INTEL_MEMORY_REGION_ID(I915_SYSTEM_MEMORY, 0),
			   intel_get_uc_mocs(i915), T_LINEAR,
			   COMPRESSION_DISABLED, COMPRESSION_TYPE_3D);
		set_geom(tmp, stride, 0, 0, width, height, 0, 0);
	}

	size = 0;
	for (i = 0, obj = objects; i < params->count; i++, obj++) {
		if (params->flags & TEST_RANDOM)
			obj->size = rand() %
				(params->size.max - params->size.min) +
				params->size.min;
		else
			obj->size = params->size.min;

		obj->size = ALIGN(obj->size, 4096);
		size += obj->size;
		if ((size >> 20) > params->mem_limit) {
			params->count = i;
			break;
		}
		obj->handle = create_bo(i915, &obj->size, region, params->oom_test);

		if (params->flags & TEST_CCS) {
			width = PAGE_SIZE / (bpp / 8);
			height = obj->size / (bpp / 8) /  width;
			stride = width * 4;

			obj->blt_obj = calloc(1, sizeof(*obj->blt_obj));
			igt_assert(obj->blt_obj);
			set_object(obj->blt_obj, obj->handle, obj->size, region_id,
				   intel_get_uc_mocs(i915), T_LINEAR,
				   COMPRESSION_ENABLED, COMPRESSION_TYPE_3D);
			set_geom(obj->blt_obj, stride, 0, 0, width, height, 0, 0);
			init_object_ccs(i915, obj, tmp, rand(), blt_ctx,
					region_id, ahnd);
		} else if (params->flags & TEST_VERIFY) {
			init_object(i915, obj, rand(), params->flags);
			move_to_lmem(i915, ctx, objects + i, 1, region_id, engine,
				     params->oom_test);
		} else {
			move_to_lmem(i915, ctx, objects + i, 1, region_id, engine,
				     params->oom_test);
		}
	}

	igt_debug("obj size min/max=%lu %s/%lu %s, count=%u, seed: %u\n",
		  readable_size(params->size.min), readable_unit(params->size.min),
		  readable_size(params->size.max), readable_unit(params->size.max),
		  params->count, seed);

	/*
	 * Move random objects back into lmem.
	 * For TEST_MULTI runs, make each object counts a loop to
	 * avoid excessive run times.
	 */
	for (l = 0; l < params->loops && igt_seconds_elapsed(&t) < 45; l += num) {
		unsigned int idx = rand() % params->count;

		num = params->flags & TEST_MULTI ? rand() % max_swap_in + 1 : 1;
		for (i = 0; i < num; i++) {
			list[i] = objects[idx];
			idx = (idx + 1) % params->count;
		}

		move_to_lmem(i915, ctx, list, num, region_id, engine,
			     params->oom_test);

		if (params->flags & TEST_ENGINES)
			engine = (engine + 1) % __num_engines__;

		if (params->flags & TEST_CCS) {
			for (i = 0; i < num; i++)
				verify_object_ccs(i915, &list[i], tmp,
						  blt_ctx, region_id, ahnd);
			/* Update random object - may swap it back in. */
			i = rand() % params->count;
			init_object_ccs(i915, &objects[i], tmp, rand(),
					blt_ctx, region_id, ahnd);
		} else if (params->flags & TEST_VERIFY) {
			for (i = 0; i < num; i++)
				verify_object(i915, &list[i], params->flags);

			/* Update random object - may swap it back in. */
			i = rand() % params->count;
			init_object(i915, &objects[i], rand(), params->flags);
		}
	}

	for (i = 0; i < params->count; i++) {
		gem_close(i915, objects[i].handle);
		free(objects[i].blt_obj);
	}

	free(list);
	free(objects);

	if (params->flags & TEST_CCS) {
		gem_close(i915, tmp->handle);
		free(tmp);
		intel_ctx_destroy(i915, blt_ctx);
		put_ahnd(ahnd);
	}
}

static void fill_params(int i915, struct params *params,
			struct drm_i915_memory_region_info *region,
			unsigned int flags,
			unsigned int nproc,
			bool do_oom_test)
{
	const int swap_mb = /* For lmem, swap is total of smem + swap. */
		igt_get_total_ram_mb() + igt_get_total_swap_mb();
	const int max_swap_pct = 75;
	unsigned int size;
	/*
	 * In random mode, add 85% hard limit to use system memory.
	 * noticed that 88.8% can trigger OOM on some system.
	 */
	const int mem_limit_pct = 85;
	int spill_mb;
	uint32_t handle;

	size = 1 << 20;
	if (flags & TEST_MASSIVE)
		size = 1u << 31;

	if (flags & TEST_RANDOM) {
		params->size.min = 4096;
		handle = create_bo(i915, &params->size.min, &region->region,
				   do_oom_test);
		gem_close(i915, handle);
		/*
		 * Big enough to ensure we need to split the copy (both the
		 * actual pages and aux CCS state) into multiple packets. Also
		 * larger enough to ensure we get a mix of 64K and 2M GTT pages
		 * (1G GTT pages are still missing from the kernel).
		 */
		params->size.max = (1ul << 24) + size + params->size.min;
		size = params->size.max;
	} else {
		params->size.min = size;
		params->size.max = size;
	}

	params->count = (region->probed_size + (size - 1)) / size * 3 / 2;
	spill_mb = (size >> 20) * params->count - (region->probed_size >> 20);
	/* Don't use all RAM for swapout. */
	igt_require(spill_mb <= swap_mb * max_swap_pct / 100);

	if (flags & TEST_HEAVY) {
		params->count *= 2;
		spill_mb = (size >> 20) * params->count -
			(region->probed_size >> 20);

		if (spill_mb > swap_mb * max_swap_pct / 100) {
			unsigned int count;
			unsigned long set;

			igt_warn("Reducing working set due low RAM + swap! (Need %d MiB, have %d MiB.)\n",
				 spill_mb, swap_mb);
			set = region->probed_size +
				(((unsigned long)swap_mb * max_swap_pct / 100) << 20);
			count = set / size;
			/* No point if heavy test is too similar to normal. */
			igt_require(count > (params->count / 2) * 133 / 100);
			params->count = count;
		}
	}

	params->loops = params->count;
	params->seed = opt.user_seed ? opt.seed : time(NULL);

	/*
	 * If run in parallel, reduce per process buffer count to keep the
	 * total the same, but don't reduce loops since we gain some
	 * efficiency by the parallel execution
	 */
	if (flags & TEST_PARALLEL)
		params->count /= nproc;

	/*
	 * For heavy tests, reduce the loop count to avoid excessive
	 * run-times
	 */
	if (flags & TEST_HEAVY)
		params->loops = params->loops / 2 + 1;

	params->flags = flags;
	params->oom_test = do_oom_test;

	params->mem_limit = swap_mb * mem_limit_pct / 100 +
		(region->probed_size >> 20);
	igt_info("Memory: system-total %dMiB, lmem-region %lldMiB, usage-limit %dMiB\n",
		 swap_mb, (region->probed_size >> 20), params->mem_limit);
	igt_info("Using %u thread(s), %u loop(s), %u objects of %lu %s - %lu %s, seed: %u, oom: %s\n",
		 params->flags & TEST_PARALLEL ? nproc : 1,
		 params->loops,
		 params->count,
		 readable_size(params->size.min),
		 readable_unit(params->size.min),
		 readable_size(params->size.max),
		 readable_unit(params->size.max),
		 params->seed,
		 do_oom_test ? "yes" : "no");
}

static void test_evict(int i915,
		       const intel_ctx_t *ctx,
		       struct drm_i915_memory_region_info *region,
		       unsigned int flags)
{
	const unsigned int nproc = sysconf(_SC_NPROCESSORS_ONLN) + 1;
	struct params params;

	if (flags & TEST_CCS)
		igt_require(HAS_FLATCCS(intel_get_drm_devid(i915)));

	fill_params(i915, &params, region, flags, nproc, false);

	if (flags & TEST_PARALLEL) {
		int fd = gem_reopen_driver(i915);

		intel_allocator_multiprocess_start();
		ctx = intel_ctx_create_all_physical(fd);
		__gem_context_set_persistence(fd, ctx->id, false);

		igt_fork(child, nproc)
			__do_evict(fd, ctx, &region->region, &params,
				   params.seed + child + 1);

		igt_waitchildren();
		intel_ctx_destroy(fd, ctx);
		close(fd);
		intel_allocator_multiprocess_stop();
	} else {
		__do_evict(i915, ctx, &region->region, &params, params.seed);
	}
}

static void leak(uint64_t alloc)
{
	char *ptr;

	ptr = mmap(NULL, alloc, PROT_READ | PROT_WRITE,
		   MAP_ANON | MAP_PRIVATE | MAP_POPULATE, -1, 0);
	if (ptr == MAP_FAILED)
		return;

	while (alloc) {
		alloc -= 4096;
		ptr[alloc] = 0;
	}
}

static void gem_leak(int fd, uint64_t alloc)
{
	uint32_t handle = gem_create(fd, alloc);
	void *buf;

	buf = gem_mmap_offset__fixed(fd, handle, 0, PAGE_SIZE, PROT_WRITE);
	memset(buf, 0, PAGE_SIZE);
	munmap(buf, PAGE_SIZE);

	gem_madvise(fd, handle, I915_MADV_DONTNEED);
}

static int *lmem_done;

static void smem_oom_exit_handler(int sig)
{
	(*lmem_done)++;
}

static void test_smem_oom(int i915,
			  const intel_ctx_t *ctx,
			  struct drm_i915_memory_region_info *region)
{
	const uint64_t smem_size = igt_get_total_ram_mb() +
		igt_get_total_swap_mb();
	const unsigned int alloc = 256 * 1024 * 1024;
	const unsigned int num_alloc = 1 + smem_size / (alloc >> 20);
	struct igt_helper_process smem_proc = {};
	unsigned int n;

	lmem_done = mmap(0, sizeof(*lmem_done), PROT_WRITE,
			 MAP_SHARED | MAP_ANON, -1, 0);
	igt_assert(lmem_done != MAP_FAILED);
	*lmem_done = 0;

	/* process for testing lmem eviction */
	igt_fork(child, 1) {
		int fd = gem_reopen_driver(i915);
		struct params params;

		fill_params(i915, &params, region, 0, 1, true);

		ctx = intel_ctx_create_all_physical(fd);
		__gem_context_set_persistence(fd, ctx->id, false);

		igt_install_exit_handler(smem_oom_exit_handler);
		__do_evict(fd, ctx, &region->region, &params,
			   params.seed + child + 1);

		intel_ctx_destroy(fd, ctx);
		close(fd);
	}

	/* smem memory hog process, respawn till the lmem process completes */
	while (!READ_ONCE(*lmem_done)) {
		igt_fork_helper(&smem_proc) {
			igt_fork(child, 1) {
				for (int pass = 0; pass < num_alloc; pass++) {
					if (READ_ONCE(*lmem_done))
						break;
					leak(alloc);
				}
			}
			igt_fork(child, 1) {
				int fd = gem_reopen_driver(i915);

				for (int pass = 0; pass < num_alloc; pass++) {
					if (READ_ONCE(*lmem_done))
						break;
					gem_leak(fd, alloc);
				}
				close(fd);
			}
			/*
			 * Wait for grand-child processes to finish or be
			 * killed by the oom killer, don't call
			 * igt_waitchildren because of the noise
			 */
			for (n = 0; n < 2; n++)
				wait(NULL);
		}
		igt_wait_helper(&smem_proc);
	}
	munmap(lmem_done, sizeof(*lmem_done));
	/* Reap exit status of the lmem process */
	igt_waitchildren();
}

#define dynamic_lmem_subtest(reg, regs, subtest_name...) \
	igt_subtest_with_dynamic(subtest_name) \
		for (unsigned int i = 0; i < (regs)->num_regions; i++) \
			for_each_if (((reg) = &(regs)->regions[i])->region.memory_class == I915_MEMORY_CLASS_DEVICE) \
				igt_dynamic_f("lmem%u", (reg)->region.memory_instance)

static int opt_handler(int option, int option_index, void *input)
{
	switch (option) {
	case 's':
		opt.user_seed = true;
		opt.seed = strtoul(optarg, NULL, 0);
		break;
	default:
		return IGT_OPT_HANDLER_ERROR;
	}

	return IGT_OPT_HANDLER_SUCCESS;
}

const char *help_str =
	"  --seed       Seed for random number generator";

struct option long_options[] = {
	{ "seed",    required_argument, NULL, 's'},
	{ 0, 0, 0, 0 }
};

igt_main_args("", long_options, help_str, opt_handler, NULL)
{
	struct drm_i915_query_memory_regions *regions;
	struct drm_i915_memory_region_info *region;
	struct test {
		const char *name;
		unsigned int flags;
	} *test, tests[] = {
		{ "basic", 0 },
		{ "massive",  TEST_MASSIVE }, /* check for (int) overflows */
		{ "massive-random",  TEST_RANDOM | TEST_MASSIVE },
		{ "random", TEST_RANDOM },
		{ "random-engines", TEST_RANDOM | TEST_ENGINES },
		{ "heavy-random", TEST_RANDOM | TEST_HEAVY },
		{ "heavy-multi", TEST_RANDOM | TEST_HEAVY | TEST_ENGINES | TEST_MULTI },
		{ "verify", TEST_VERIFY},
		{ "verify-random", TEST_VERIFY | TEST_RANDOM},
		{ "heavy-verify-random", TEST_VERIFY | TEST_RANDOM | TEST_HEAVY },
		{ "heavy-verify-multi", TEST_VERIFY | TEST_RANDOM | TEST_HEAVY | TEST_ENGINES | TEST_MULTI },
		{ "parallel-random", TEST_PARALLEL | TEST_RANDOM },
		{ "parallel-random-engines", TEST_PARALLEL | TEST_RANDOM | TEST_ENGINES },
		{ "parallel-random-verify", TEST_PARALLEL | TEST_RANDOM | TEST_VERIFY },
		{ "parallel-multi", TEST_PARALLEL | TEST_RANDOM | TEST_VERIFY | TEST_ENGINES | TEST_MULTI },
		{ "verify-ccs", TEST_CCS },
		{ "verify-random-ccs", TEST_CCS | TEST_RANDOM },
		{ "heavy-verify-random-ccs", TEST_CCS | TEST_RANDOM | TEST_HEAVY },
		{ "heavy-verify-multi-ccs", TEST_CCS | TEST_RANDOM | TEST_HEAVY | TEST_ENGINES | TEST_MULTI },
		{ "parallel-random-verify-ccs", TEST_PARALLEL | TEST_RANDOM | TEST_CCS },
		{ }
	};
	const intel_ctx_t *ctx;
	int i915 = -1;

	igt_fixture {
		struct intel_execution_engine2 *e;
		char *tmp;

		/*
		 * If the driver is already loaded, check that it has
		 * lmem before unloading to prevent a needless
		 * unload-load cycle on integrated platforms.
		 */
		if (igt_kmod_is_loaded("i915")) {
			i915 = __drm_open_driver(DRIVER_INTEL);
			igt_require_fd(i915);
			igt_require_gem(i915);
			igt_require(gem_has_lmem(i915));
			close(i915);
		}

		igt_i915_driver_unload();
		igt_assert_eq(igt_i915_driver_load("lmem_size=4096"), 0);

		i915 = __drm_open_driver(DRIVER_INTEL);
		igt_require_fd(i915);
		/*
		 * Even if we did the lmem check above, do it again in
		 * case the reload messed something up (as unlikely it
		 * is)
		 */
		igt_require_gem(i915);
		igt_require(gem_has_lmem(i915));

		tmp = __igt_params_get(i915, "lmem_size");
		if (!tmp)
			igt_info("lmem_size modparam not supported on this kernel. Continuing with full lmem size. This may result in CI timeouts.\n");
		free(tmp);

		regions = gem_get_query_memory_regions(i915);
		igt_require(regions);

		for_each_physical_engine(i915, e)
			__num_engines__++;
		igt_require(__num_engines__);
		ctx = intel_ctx_create_all_physical(i915);
		__gem_context_set_persistence(i915, ctx->id, false);

	}

	for (test = tests; test->name; test++) {
		igt_describe("Exercise local memory swapping to system memory");
		dynamic_lmem_subtest(region, regions, test->name)
			test_evict(i915, ctx, region, test->flags);
	}

	igt_describe("Exercise local memory swapping during exhausting system memory");
	dynamic_lmem_subtest(region, regions, "smem-oom")
		test_smem_oom(i915, ctx, region);

	igt_fixture {
		intel_ctx_destroy(i915, ctx);
		free(regions);
		close(i915);
		igt_i915_driver_unload();
	}

	igt_exit();
}
