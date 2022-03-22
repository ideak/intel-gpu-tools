// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include <errno.h>
#include <glib.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <malloc.h>
#include "drm.h"
#include "igt.h"
#include "i915/gem.h"
#include "i915/gem_create.h"
#include "lib/intel_chipset.h"
#include "i915/i915_blt.h"
#include "i915/intel_mocs.h"

IGT_TEST_DESCRIPTION("Exercise gen12 blitter with and without flatccs compression");

static struct param {
	int compression_format;
	int tiling;
	bool write_png;
	bool print_bb;
	bool print_surface_info;
	int width;
	int height;
} param = {
	.compression_format = 0,
	.tiling = -1,
	.write_png = false,
	.print_bb = false,
	.print_surface_info = false,
	.width = 512,
	.height = 512,
};

struct test_config {
	bool compression;
	bool inplace;
	bool surfcopy;
	bool new_ctx;
	bool suspend_resume;
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

	/* Ensure mip tail won't overlap lod */
	obj->mip_tail_start_lod = 0xf;
}

static void set_surf_object(struct blt_ctrl_surf_copy_object *obj,
			    uint32_t handle, uint32_t region, uint64_t size,
			    uint8_t mocs, enum blt_access_type access_type)
{
	obj->handle = handle;
	obj->region = region;
	obj->size = size;
	obj->mocs = mocs;
	obj->access_type = access_type;
}

static struct blt_copy_object *
create_object(int i915, uint32_t region,
	      uint32_t width, uint32_t height, uint32_t bpp, uint8_t mocs,
	      enum blt_tiling tiling,
	      enum blt_compression compression,
	      enum blt_compression_type compression_type,
	      bool create_mapping)
{
	struct blt_copy_object *obj;
	uint64_t size = width * height * bpp / 8;
	uint32_t stride = tiling == T_LINEAR ? width * 4 : width;
	uint32_t handle;

	obj = calloc(1, sizeof(*obj));

	obj->size = size;
	igt_assert(__gem_create_in_memory_regions(i915, &handle,
						  &size, region) == 0);

	set_object(obj, handle, size, region, mocs, tiling,
		   compression, compression_type);
	set_geom(obj, stride, 0, 0, width, height, 0, 0);

	if (create_mapping)
		obj->ptr = gem_mmap__device_coherent(i915, handle, 0, size,
						     PROT_READ | PROT_WRITE);

	return obj;
}

static void destroy_object(int i915, struct blt_copy_object *obj)
{
	if (obj->ptr)
		munmap(obj->ptr, obj->size);

	gem_close(i915, obj->handle);
}

static void set_blt_object(struct blt_copy_object *obj,
			   const struct blt_copy_object *orig)
{
	memcpy(obj, orig, sizeof(*obj));
}

#define PRINT_SURFACE_INFO(name, obj) do { \
	if (param.print_surface_info) \
		blt_surface_info((name), (obj)); } while (0)

#define WRITE_PNG(fd, id, name, obj, w, h) do { \
	if (param.write_png) \
		blt_surface_to_png((fd), (id), (name), (obj), (w), (h)); } while (0)

static void surf_copy(int i915,
		      const intel_ctx_t *ctx,
		      const struct intel_execution_engine2 *e,
		      uint64_t ahnd,
		      const struct blt_copy_object *src,
		      const struct blt_copy_object *mid,
		      const struct blt_copy_object *dst,
		      int run_id, bool suspend_resume)
{
	struct blt_copy_data blt = {};
	struct blt_block_copy_data_ext ext = {};
	struct blt_ctrl_surf_copy_data surf = {};
	uint32_t bb, ccs, ccs2, *ccsmap, *ccsmap2;
	uint64_t bb_size, ccssize = mid->size / CCS_RATIO;
	uint32_t *ccscopy;
	uint8_t uc_mocs = intel_get_uc_mocs(i915);
	int result;

	igt_assert(mid->compression);
	ccscopy = (uint32_t *) malloc(ccssize);
	bb_size = 4096;
	bb = gem_create_from_pool(i915, &bb_size, REGION_SMEM);
	ccs = gem_create(i915, ccssize);
	ccs2 = gem_create(i915, ccssize);

	surf.i915 = i915;
	surf.print_bb = param.print_bb;
	set_surf_object(&surf.src, mid->handle, mid->region, mid->size,
			uc_mocs, INDIRECT_ACCESS);
	set_surf_object(&surf.dst, ccs, REGION_SMEM, ccssize,
			uc_mocs, DIRECT_ACCESS);
	set_batch(&surf.bb, bb, bb_size, REGION_SMEM);
	blt_ctrl_surf_copy(i915, ctx, e, ahnd, &surf);
	gem_sync(i915, surf.dst.handle);

	ccsmap = gem_mmap__device_coherent(i915, ccs, 0, surf.dst.size,
					   PROT_READ | PROT_WRITE);
	memcpy(ccscopy, ccsmap, ccssize);

	if (suspend_resume) {
		char *orig, *orig2, *newsum, *newsum2;

		orig = g_compute_checksum_for_data(G_CHECKSUM_SHA1, (void *)ccsmap, surf.dst.size);
		orig2 = g_compute_checksum_for_data(G_CHECKSUM_SHA1, (void *)mid->ptr, mid->size);

		igt_system_suspend_autoresume(SUSPEND_STATE_FREEZE, SUSPEND_TEST_NONE);

		set_surf_object(&surf.dst, ccs2, REGION_SMEM, ccssize,
				0, DIRECT_ACCESS);
		blt_ctrl_surf_copy(i915, ctx, e, ahnd, &surf);
		gem_sync(i915, surf.dst.handle);

		ccsmap2 = gem_mmap__device_coherent(i915, ccs2, 0, surf.dst.size,
						    PROT_READ | PROT_WRITE);
		newsum = g_compute_checksum_for_data(G_CHECKSUM_SHA1, (void *)ccsmap2, surf.dst.size);
		newsum2 = g_compute_checksum_for_data(G_CHECKSUM_SHA1, (void *)mid->ptr, mid->size);

		munmap(ccsmap2, ccssize);
		igt_assert(!strcmp(orig, newsum));
		igt_assert(!strcmp(orig2, newsum2));
		g_free(orig);
		g_free(orig2);
		g_free(newsum);
		g_free(newsum2);
	}

	/* corrupt ccs */
	for (int i = 0; i < surf.dst.size / sizeof(uint32_t); i++)
		ccsmap[i] = i;
	set_surf_object(&surf.src, ccs, REGION_SMEM, ccssize,
			uc_mocs, DIRECT_ACCESS);
	set_surf_object(&surf.dst, mid->handle, mid->region, mid->size,
			uc_mocs, INDIRECT_ACCESS);
	blt_ctrl_surf_copy(i915, ctx, e, ahnd, &surf);

	memset(&blt, 0, sizeof(blt));
	blt.color_depth = CD_32bit;
	blt.print_bb = param.print_bb;
	set_blt_object(&blt.src, mid);
	set_blt_object(&blt.dst, dst);
	set_object_ext(&ext.src, mid->compression_type, mid->x2, mid->y2, SURFACE_TYPE_2D);
	set_object_ext(&ext.dst, 0, dst->x2, dst->y2, SURFACE_TYPE_2D);
	bb_size = 4096;
	bb = gem_create_from_pool(i915, &bb_size, REGION_SMEM);
	set_batch(&blt.bb, bb, bb_size, REGION_SMEM);
	blt_block_copy(i915, ctx, e, ahnd, &blt, &ext);
	gem_sync(i915, blt.dst.handle);
	WRITE_PNG(i915, run_id, "corrupted", &blt.dst, dst->x2, dst->y2);
	result = memcmp(src->ptr, dst->ptr, src->size);
	igt_assert(result != 0);

	/* retrieve back ccs */
	memcpy(ccsmap, ccscopy, ccssize);
	blt_ctrl_surf_copy(i915, ctx, e, ahnd, &surf);

	blt_block_copy(i915, ctx, e, ahnd, &blt, &ext);
	gem_sync(i915, blt.dst.handle);
	WRITE_PNG(i915, run_id, "corrected", &blt.dst, dst->x2, dst->y2);
	result = memcmp(src->ptr, dst->ptr, src->size);
	igt_assert(result == 0);

	munmap(ccsmap, ccssize);
	gem_close(i915, ccs);
}

static void block_copy(int i915,
		       const intel_ctx_t *ctx,
		       const struct intel_execution_engine2 *e,
		       uint32_t region1, uint32_t region2,
		       enum blt_tiling mid_tiling,
		       const struct test_config *config)
{
	struct blt_copy_data blt = {};
	struct blt_block_copy_data_ext ext = {}, *pext = &ext;
	struct blt_copy_object *src, *mid, *dst;
	const uint32_t bpp = 32;
	uint64_t bb_size = 4096;
	uint64_t ahnd = intel_allocator_open_full(i915, ctx->id, 0, 0,
						  INTEL_ALLOCATOR_SIMPLE,
						  ALLOC_STRATEGY_LOW_TO_HIGH, 0);
	uint32_t run_id = mid_tiling;
	uint32_t mid_region = region2, bb;
	uint32_t width = param.width, height = param.height;
	enum blt_compression mid_compression = config->compression;
	int mid_compression_format = param.compression_format;
	enum blt_compression_type comp_type = COMPRESSION_TYPE_3D;
	uint8_t uc_mocs = intel_get_uc_mocs(i915);
	int result;

	igt_assert(__gem_create_in_memory_regions(i915, &bb, &bb_size, region1) == 0);

	if (!blt_supports_compression(i915))
		pext = NULL;

	src = create_object(i915, region1, width, height, bpp, uc_mocs,
			    T_LINEAR, COMPRESSION_DISABLED, comp_type, true);
	mid = create_object(i915, mid_region, width, height, bpp, uc_mocs,
			    mid_tiling, mid_compression, comp_type, true);
	dst = create_object(i915, region1, width, height, bpp, uc_mocs,
			    T_LINEAR, COMPRESSION_DISABLED, comp_type, true);
	igt_assert(src->size == dst->size);
	PRINT_SURFACE_INFO("src", src);
	PRINT_SURFACE_INFO("mid", mid);
	PRINT_SURFACE_INFO("dst", dst);

	blt_surface_fill_rect(i915, src, width, height);
	WRITE_PNG(i915, run_id, "src", src, width, height);

	memset(&blt, 0, sizeof(blt));
	blt.color_depth = CD_32bit;
	blt.print_bb = param.print_bb;
	set_blt_object(&blt.src, src);
	set_blt_object(&blt.dst, mid);
	set_object_ext(&ext.src, 0, width, height, SURFACE_TYPE_2D);
	set_object_ext(&ext.dst, mid_compression_format, width, height, SURFACE_TYPE_2D);
	set_batch(&blt.bb, bb, bb_size, region1);

	blt_block_copy(i915, ctx, e, ahnd, &blt, pext);
	gem_sync(i915, mid->handle);

	/* We expect mid != src if there's compression */
	if (mid->compression)
		igt_assert(memcmp(src->ptr, mid->ptr, src->size) != 0);

	WRITE_PNG(i915, run_id, "src", &blt.src, width, height);
	WRITE_PNG(i915, run_id, "mid", &blt.dst, width, height);

	if (config->surfcopy && pext) {
		const intel_ctx_t *surf_ctx = ctx;
		uint64_t surf_ahnd = ahnd;
		struct intel_execution_engine2 surf_e = *e;

		if (config->new_ctx) {
			intel_ctx_cfg_t cfg = {};

			cfg.num_engines = 1;
			cfg.engines[0].engine_class = e->class;
			cfg.engines[0].engine_instance = e->instance;
			surf_ctx = intel_ctx_create(i915, &cfg);
			surf_e.flags = 0;
			surf_ahnd = intel_allocator_open_full(i915, surf_ctx->id, 0, 0,
							      INTEL_ALLOCATOR_SIMPLE,
							      ALLOC_STRATEGY_LOW_TO_HIGH, 0);
		}

		surf_copy(i915, surf_ctx, &surf_e, surf_ahnd, src, mid, dst, run_id,
			  config->suspend_resume);

		if (surf_ctx != ctx) {
			intel_ctx_destroy(i915, surf_ctx);
			put_ahnd(surf_ahnd);
		}
	}

	memset(&blt, 0, sizeof(blt));
	blt.color_depth = CD_32bit;
	blt.print_bb = param.print_bb;
	set_blt_object(&blt.src, mid);
	set_blt_object(&blt.dst, dst);
	set_object_ext(&ext.src, mid_compression_format, width, height, SURFACE_TYPE_2D);
	set_object_ext(&ext.dst, 0, width, height, SURFACE_TYPE_2D);
	if (config->inplace) {
		set_object(&blt.dst, mid->handle, dst->size, mid->region, 0,
			   T_LINEAR, COMPRESSION_DISABLED, comp_type);
		blt.dst.ptr = mid->ptr;
	}

	set_batch(&blt.bb, bb, bb_size, region1);
	blt_block_copy(i915, ctx, e, ahnd, &blt, pext);
	gem_sync(i915, blt.dst.handle);
	WRITE_PNG(i915, run_id, "dst", &blt.dst, width, height);

	result = memcmp(src->ptr, blt.dst.ptr, src->size);

	destroy_object(i915, src);
	destroy_object(i915, mid);
	destroy_object(i915, dst);
	gem_close(i915, bb);
	put_ahnd(ahnd);

	igt_assert_f(!result, "source and destination surfaces differs!\n");
}

static void block_copy_test(int i915,
			    const struct test_config *config,
			    const intel_ctx_t *ctx,
			    struct igt_collection *set)
{
	struct igt_collection *regions;
	const struct intel_execution_engine2 *e;

	if (config->compression && !blt_supports_compression(i915))
		return;

	if (config->inplace && !config->compression)
		return;

	for (int tiling = T_LINEAR; tiling <= T_TILE64; tiling++) {
		if (!blt_supports_tiling(i915, tiling) ||
		    (param.tiling >= 0 && param.tiling != tiling))
			continue;

		for_each_ctx_engine(i915, ctx, e) {
			if (!gem_engine_can_block_copy(i915, e))
				continue;

			for_each_variation_r(regions, 2, set) {
				uint32_t region1, region2;
				char *regtxt;

				region1 = igt_collection_get_value(regions, 0);
				region2 = igt_collection_get_value(regions, 1);

				/* Compressed surface must be in device memory */
				if (config->compression && !IS_DEVICE_MEMORY_REGION(region2))
					continue;

				regtxt = memregion_dynamic_subtest_name(regions);
				igt_dynamic_f("%s-%s-compfmt%d-%s",
					      blt_tiling_name(tiling),
					      config->compression ?
						      "compressed" : "uncompressed",
					      param.compression_format, regtxt) {
					block_copy(i915, ctx, e,
						   region1, region2,
						   tiling, config);
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
	case 'f':
		param.compression_format = atoi(optarg);
		igt_debug("Compression format: %d\n", param.compression_format);
		igt_assert((param.compression_format & ~0x1f) == 0);
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
	"  -f\tCompression format (0-31)\n"
	"  -p\tWrite PNG\n"
	"  -s\tPrint surface info\n"
	"  -t\tTiling format (0 - linear, 1 - XMAJOR, 2 - YMAJOR, 3 - TILE4, 4 - TILE64)\n"
	"  -W\tWidth (default 512)\n"
	"  -H\tHeight (default 512)"
	;

igt_main_args("bf:pst:W:H:", NULL, help_str, opt_handler, NULL)
{
	struct drm_i915_query_memory_regions *query_info;
	struct igt_collection *set;
	const intel_ctx_t *ctx;
	int i915;
	igt_hang_t hang;

	igt_fixture {
		i915 = drm_open_driver(DRIVER_INTEL);
		igt_require_gem(i915);
		igt_require(AT_LEAST_GEN(intel_get_drm_devid(i915), 12) > 0);

		query_info = gem_get_query_memory_regions(i915);
		igt_require(query_info);

		set = get_memory_region_set(query_info,
					    I915_SYSTEM_MEMORY,
					    I915_DEVICE_MEMORY);
		ctx = intel_ctx_create_all_physical(i915);
		hang = igt_allow_hang(i915, ctx->id, 0);
	}

	igt_describe("Check block-copy uncompressed blit");
	igt_subtest_with_dynamic("block-copy-uncompressed") {
		struct test_config config = {};

		block_copy_test(i915, &config, ctx, set);
	}

	igt_describe("Check block-copy flatccs compressed blit");
	igt_subtest_with_dynamic("block-copy-compressed") {
		struct test_config config = { .compression = true };

		block_copy_test(i915, &config, ctx, set);
	}

	igt_describe("Check block-copy flatccs inplace decompression blit");
	igt_subtest_with_dynamic("block-copy-inplace") {
		struct test_config config = { .compression = true,
					      .inplace = true };

		block_copy_test(i915, &config, ctx, set);
	}

	igt_describe("Check flatccs data can be copied from/to surface");
	igt_subtest_with_dynamic("ctrl-surf-copy") {
		struct test_config config = { .compression = true,
					      .surfcopy = true };

		block_copy_test(i915, &config, ctx, set);
	}

	igt_describe("Check flatccs data are physically tagged and visible"
		     " in different contexts");
	igt_subtest_with_dynamic("ctrl-surf-copy-new-ctx") {
		struct test_config config = { .compression = true,
					      .surfcopy = true,
					      .new_ctx = true };

		block_copy_test(i915, &config, ctx, set);
	}

	igt_describe("Check flatccs data persists after suspend / resume (S0)");
	igt_subtest_with_dynamic("suspend-resume") {
		struct test_config config = { .compression = true,
					      .surfcopy = true,
					      .suspend_resume = true };

		block_copy_test(i915, &config, ctx, set);
	}

	igt_fixture {
		igt_disallow_hang(i915, hang);
		close(i915);
	}
}
