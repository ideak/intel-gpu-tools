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

struct blt_fast_copy_data {
	int i915;
	struct blt_copy_object src;
	struct blt_copy_object mid;
	struct blt_copy_object dst;

	struct blt_copy_batch bb;
	enum blt_color_depth color_depth;

	/* debug stuff */
	bool print_bb;
};

static int fast_copy_one_bb(int i915,
			    const intel_ctx_t *ctx,
			    const struct intel_execution_engine2 *e,
			    uint64_t ahnd,
			    const struct blt_fast_copy_data *blt)
{
	struct drm_i915_gem_execbuffer2 execbuf = {};
	struct drm_i915_gem_exec_object2 obj[4] = {};
	struct blt_copy_data blt_tmp;
	uint64_t src_offset, mid_offset, dst_offset, bb_offset, alignment;
	uint64_t bb_pos = 0;
	uint32_t flags;
	int ret;

	alignment = gem_detect_safe_alignment(i915);

	src_offset = get_offset(ahnd, blt->src.handle, blt->src.size, alignment);
	mid_offset = get_offset(ahnd, blt->mid.handle, blt->mid.size, alignment);
	dst_offset = get_offset(ahnd, blt->dst.handle, blt->dst.size, alignment);
	bb_offset = get_offset(ahnd, blt->bb.handle, blt->bb.size, alignment);

	/* First blit */
	memset(&blt_tmp, 0, sizeof(blt_tmp));
	blt_tmp.src = blt->src;
	blt_tmp.dst = blt->mid;
	blt_tmp.bb = blt->bb;
	blt_tmp.color_depth = blt->color_depth;
	blt_tmp.print_bb = blt->print_bb;
	bb_pos = emit_blt_fast_copy(i915, ahnd, &blt_tmp, bb_pos, false);

	/* Second blit */
	memset(&blt_tmp, 0, sizeof(blt_tmp));
	blt_tmp.src = blt->mid;
	blt_tmp.dst = blt->dst;
	blt_tmp.bb = blt->bb;
	blt_tmp.color_depth = blt->color_depth;
	blt_tmp.print_bb = blt->print_bb;
	bb_pos = emit_blt_fast_copy(i915, ahnd, &blt_tmp, bb_pos, true);

	flags = EXEC_OBJECT_PINNED | EXEC_OBJECT_SUPPORTS_48B_ADDRESS;

	obj[0].handle = blt->src.handle;
	obj[0].offset = CANONICAL(src_offset);
	obj[0].flags = flags;

	obj[1].handle = blt->mid.handle;
	obj[1].offset = CANONICAL(mid_offset);
	obj[1].flags = flags;

	obj[2].handle = blt->dst.handle;
	obj[2].offset = CANONICAL(dst_offset);
	obj[2].flags = flags | EXEC_OBJECT_WRITE;

	obj[3].handle = blt->bb.handle;
	obj[3].offset = CANONICAL(bb_offset);
	obj[3].flags = flags;

	execbuf.buffer_count = 4;
	execbuf.buffers_ptr = to_user_pointer(obj);
	execbuf.rsvd1 = ctx ? ctx->id : 0;
	execbuf.flags = e ? e->flags : I915_EXEC_BLT;
	ret = __gem_execbuf(i915, &execbuf);

	gem_sync(i915, blt->bb.handle);

	return ret;
}

static void fast_copy_emit(int i915, const intel_ctx_t *ctx,
			   const struct intel_execution_engine2 *e,
			   uint32_t region1, uint32_t region2,
			   enum blt_tiling_type mid_tiling)
{
	struct blt_fast_copy_data blt = {};
	struct blt_copy_object *src, *mid, *dst;
	const uint32_t bpp = 32;
	uint64_t bb_size = 4096;
	uint64_t ahnd = intel_allocator_open_full(i915, ctx->id, 0, 0,
						  INTEL_ALLOCATOR_SIMPLE,
						  ALLOC_STRATEGY_LOW_TO_HIGH, 0);
	uint32_t bb, width = param.width, height = param.height;
	int result;

	igt_assert(__gem_create_in_memory_regions(i915, &bb, &bb_size, region1) == 0);

	src = blt_create_object(i915, region1, width, height, bpp, 0,
				T_LINEAR, COMPRESSION_DISABLED, 0, true);
	mid = blt_create_object(i915, region2, width, height, bpp, 0,
				mid_tiling, COMPRESSION_DISABLED, 0, true);
	dst = blt_create_object(i915, region1, width, height, bpp, 0,
				T_LINEAR, COMPRESSION_DISABLED, 0, true);
	igt_assert(src->size == dst->size);

	PRINT_SURFACE_INFO("src", src);
	PRINT_SURFACE_INFO("mid", mid);
	PRINT_SURFACE_INFO("dst", dst);

	blt_surface_fill_rect(i915, src, width, height);
	WRITE_PNG(i915, mid_tiling, "src", src, width, height);

	memset(&blt, 0, sizeof(blt));
	blt.color_depth = CD_32bit;
	blt.print_bb = param.print_bb;
	blt_set_copy_object(&blt.src, src);
	blt_set_copy_object(&blt.mid, mid);
	blt_set_copy_object(&blt.dst, dst);
	blt_set_batch(&blt.bb, bb, bb_size, region1);

	fast_copy_one_bb(i915, ctx, e, ahnd, &blt);
	gem_sync(i915, blt.dst.handle);

	WRITE_PNG(i915, mid_tiling, "mid", &blt.mid, width, height);
	WRITE_PNG(i915, mid_tiling, "dst", &blt.dst, width, height);

	result = memcmp(src->ptr, blt.dst.ptr, src->size);

	blt_destroy_object(i915, src);
	blt_destroy_object(i915, mid);
	blt_destroy_object(i915, dst);
	gem_close(i915, bb);
	put_ahnd(ahnd);

	munmap(&bb, bb_size);

	igt_assert_f(!result, "source and destination surfaces differs!\n");
}

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

enum fast_copy_func {
	FAST_COPY,
	FAST_COPY_EMIT
};

static char
	*full_subtest_str(char *regtxt, enum blt_tiling_type tiling,
			  enum fast_copy_func func)
{
	char *name;
	uint32_t len;

	len = asprintf(&name, "%s-%s%s", blt_tiling_name(tiling), regtxt,
		       func == FAST_COPY_EMIT ? "-emit" : "");

	igt_assert_f(len >= 0, "asprintf failed!\n");

	return name;
}

static void fast_copy_test(int i915,
			   const intel_ctx_t *ctx,
			   struct igt_collection *set,
			   enum fast_copy_func func)
{
	struct igt_collection *regions;
	const struct intel_execution_engine2 *e;
	void (*copy_func)(int i915, const intel_ctx_t *ctx,
			  const struct intel_execution_engine2 *e,
			  uint32_t r1, uint32_t r2, enum blt_tiling_type tiling);
	int tiling;

	for_each_tiling(tiling) {
		if (!blt_fast_copy_supports_tiling(i915, tiling))
			continue;

		for_each_ctx_engine(i915, ctx, e) {
			if (e->class != I915_ENGINE_CLASS_COPY)
				continue;
			for_each_variation_r(regions, 2, set) {
				uint32_t region1, region2;
				char *regtxt, *test_name;

				region1 = igt_collection_get_value(regions, 0);
				region2 = igt_collection_get_value(regions, 1);

				copy_func = (func == FAST_COPY) ? fast_copy : fast_copy_emit;
				regtxt = memregion_dynamic_subtest_name(regions);
				test_name = full_subtest_str(regtxt, tiling, func);

				igt_dynamic_f("%s", test_name) {
					copy_func(i915, ctx, e,
						  region1, region2,
						  tiling);
				}

				free(regtxt);
				free(test_name);
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
		fast_copy_test(i915, ctx, set, FAST_COPY);
	}

	igt_describe("Check multiple fast-copy in one batch");
	igt_subtest_with_dynamic("fast-copy-emit") {
		fast_copy_test(i915, ctx, set, FAST_COPY_EMIT);
	}

	igt_fixture {
		igt_disallow_hang(i915, hang);
		close(i915);
	}
}
