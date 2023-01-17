// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023 Intel Corporation
 */

#include "igt.h"
#include "drm.h"
#include "i915/gem.h"
#include "i915/gem_create.h"
#include "lib/intel_chipset.h"
#include "i915/i915_blt.h"
#include "i915/intel_mocs.h"

IGT_TEST_DESCRIPTION("Exercise blitter commands");

static struct param {
	int tiling;
	bool write_png;
	bool print_bb;
	bool print_surface_info;
	int width;
	int height;
} param = {
	.tiling = -1,
	.write_png = false,
	.print_bb = false,
	.print_surface_info = false,
	.width = 512,
	.height = 512,
};

#define PRINT_SURFACE_INFO(name, obj) do { \
	if (param.print_surface_info) \
		blt_surface_info((name), (obj)); } while (0)

#define WRITE_PNG(fd, id, name, obj, w, h) do { \
	if (param.write_png) \
		blt_surface_to_png((fd), (id), (name), (obj), (w), (h)); } while (0)

static void fast_copy(int i915, const intel_ctx_t *ctx,
		      const struct intel_execution_engine2 *e,
		      uint32_t region1, uint32_t region2,
		      enum blt_tiling_type mid_tiling)
{
	struct blt_copy_data blt = {};
	struct blt_copy_object *src, *mid, *dst;
	const uint32_t bpp = 32;
	uint64_t bb_size = 4096;
	uint64_t ahnd = intel_allocator_open_full(i915, ctx->id, 0, 0,
						  INTEL_ALLOCATOR_SIMPLE,
						  ALLOC_STRATEGY_LOW_TO_HIGH, 0);
	uint32_t bb;
	uint32_t width = param.width, height = param.height;
	int result;

	igt_assert(__gem_create_in_memory_regions(i915, &bb, &bb_size, region1) == 0);

	src = blt_create_object(i915, region1, width, height, bpp, 0,
				T_LINEAR, COMPRESSION_DISABLED, 0, true);
	mid = blt_create_object(i915, region2, width, height, bpp, 0,
				mid_tiling, COMPRESSION_DISABLED, 0, true);
	dst = blt_create_object(i915, region1, width, height, bpp, 0,
				T_LINEAR, COMPRESSION_DISABLED, 0, true);
	igt_assert(src->size == dst->size);

	blt_surface_fill_rect(i915, src, width, height);

	memset(&blt, 0, sizeof(blt));
	blt.color_depth = CD_32bit;
	blt.print_bb = param.print_bb;
	blt_set_copy_object(&blt.src, src);
	blt_set_copy_object(&blt.dst, mid);
	blt_set_batch(&blt.bb, bb, bb_size, region1);

	blt_fast_copy(i915, ctx, e, ahnd, &blt);
	gem_sync(i915, mid->handle);

	WRITE_PNG(i915, mid_tiling, "src", &blt.src, width, height);
	WRITE_PNG(i915, mid_tiling, "mid", &blt.dst, width, height);

	memset(&blt, 0, sizeof(blt));
	blt.color_depth = CD_32bit;
	blt.print_bb = param.print_bb;
	blt_set_copy_object(&blt.src, mid);
	blt_set_copy_object(&blt.dst, dst);
	blt_set_batch(&blt.bb, bb, bb_size, region1);

	blt_fast_copy(i915, ctx, e, ahnd, &blt);
	gem_sync(i915, blt.dst.handle);

	WRITE_PNG(i915, mid_tiling, "dst", &blt.dst, width, height);

	result = memcmp(src->ptr, blt.dst.ptr, src->size);

	blt_destroy_object(i915, src);
	blt_destroy_object(i915, mid);
	blt_destroy_object(i915, dst);
	gem_close(i915, bb);
	put_ahnd(ahnd);

	igt_assert_f(!result, "source and destination surfaces differs!\n");
}

static void fast_copy_test(int i915,
			   const intel_ctx_t *ctx,
			   struct igt_collection *set)
{
	struct igt_collection *regions;
	const struct intel_execution_engine2 *e;
	int tiling;

	for_each_tiling(tiling) {
		if (!blt_fast_copy_supports_tiling(i915, tiling))
			continue;

		for_each_ctx_engine(i915, ctx, e) {
			if (e->class != I915_ENGINE_CLASS_COPY)
				continue;
			for_each_variation_r(regions, 2, set) {
				uint32_t region1, region2;
				char *regtxt;

				region1 = igt_collection_get_value(regions, 0);
				region2 = igt_collection_get_value(regions, 1);
				regtxt = memregion_dynamic_subtest_name(regions);

				igt_dynamic_f("%s-%s",
					      blt_tiling_name(tiling), regtxt) {
					fast_copy(i915, ctx, e,
						  region1, region2,
						  tiling);
				}

				free(regtxt);
			}
		}
	}
}

static int opt_handler(int opt, int opt_index, void *data)
{
	switch (opt) {
	case 'b':
		param.print_bb = true;
		igt_debug("Print bb: %d\n", param.print_bb);
		break;
	case 'p':
		param.write_png = true;
		igt_debug("Write png: %d\n", param.write_png);
		break;
	case 's':
		param.print_surface_info = true;
		igt_debug("Print surface info: %d\n", param.print_surface_info);
		break;
	case 't':
		param.tiling = atoi(optarg);
		igt_debug("Tiling: %d\n", param.tiling);
		break;
	case 'W':
		param.width = atoi(optarg);
		igt_debug("Width: %d\n", param.width);
		break;
	case 'H':
		param.height = atoi(optarg);
		igt_debug("Height: %d\n", param.height);
		break;
	default:
		return IGT_OPT_HANDLER_ERROR;
	}

	return IGT_OPT_HANDLER_SUCCESS;
}

const char *help_str =
	"  -b\tPrint bb\n"
	"  -p\tWrite PNG\n"
	"  -s\tPrint surface info\n"
	"  -t\tTiling format (0 - linear, 1 - XMAJOR, 2 - YMAJOR, 3 - TILE4, 4 - TILE64, 5 - YFMAJOR)\n"
	"  -W\tWidth (default 512)\n"
	"  -H\tHeight (default 512)"
	;

igt_main_args("b:pst:W:H:", NULL, help_str, opt_handler, NULL)
{
	struct drm_i915_query_memory_regions *query_info;
	struct igt_collection *set;
	const intel_ctx_t *ctx;
	int i915;
	igt_hang_t hang;

	igt_fixture {
		i915 = drm_open_driver(DRIVER_INTEL);
		igt_require_gem(i915);
		igt_require(blt_has_fast_copy(i915));

		igt_require(gem_uses_full_ppgtt(i915));

		query_info = gem_get_query_memory_regions(i915);
		igt_require(query_info);

		set = get_memory_region_set(query_info,
					    I915_SYSTEM_MEMORY,
					    I915_DEVICE_MEMORY);
		ctx = intel_ctx_create_all_physical(i915);
		hang = igt_allow_hang(i915, ctx->id, 0);
	}

	igt_describe("Check fast-copy blit");
	igt_subtest_with_dynamic("fast-copy") {
		fast_copy_test(i915, ctx, set);
	}

	igt_fixture {
		igt_disallow_hang(i915, hang);
		close(i915);
	}
}
