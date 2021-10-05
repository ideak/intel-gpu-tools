// SPDX-License-Identifier: MIT
/*
 * Copyright © 2021 Intel Corporation
 */

#include "igt.h"
#include "i915/gem.h"
#include "i915/gem_create.h"
#include <fcntl.h>

IGT_TEST_DESCRIPTION("Test PXP that manages protected content through arbitrated HW-PXP-session");
/* Note: PXP = "Protected Xe Path" */

/* Struct and definitions for power management. */
struct powermgt_data {
	int debugfsdir;
	bool has_runtime_pm;
};

struct simple_exec_assets {
	uint32_t ctx;
	uint32_t fencebo;
	struct intel_buf *fencebuf;
	struct buf_ops *bops;
	struct intel_bb *ibb;
};

static int create_bo_ext(int i915, uint32_t size, bool protected_is_true, uint32_t *bo_out)
{
	int ret;
	uint64_t size64 = size;
	struct i915_user_extension *ext = NULL;

	struct drm_i915_gem_create_ext_protected_content protected_ext = {
		.base = { .name = I915_GEM_CREATE_EXT_PROTECTED_CONTENT },
		.flags = 0,
	};

	if (protected_is_true)
		ext = &protected_ext.base;

	*bo_out = 0;
	ret = __gem_create_ext(i915, &size64, bo_out, ext);

	return ret;
}

static void test_bo_alloc_pxp_nohw(int i915)
{
	int ret;
	uint32_t bo;

	ret = create_bo_ext(i915, 4096, false, &bo);
	igt_assert_eq(ret, 0);
	gem_close(i915, bo);

	ret = create_bo_ext(i915, 4096, true, &bo);
	igt_assert_eq(ret, -ENODEV);
	igt_assert_eq(bo, 0);
}

static void test_bo_alloc_pxp_off(int i915)
{
	int ret;
	uint32_t bo;

	ret = create_bo_ext(i915, 4096, false, &bo);
	igt_assert_eq(ret, 0);
	gem_close(i915, bo);
}

static void test_bo_alloc_pxp_on(int i915)
{
	int ret;
	uint32_t bo;

	ret = create_bo_ext(i915, 4096, true, &bo);
	igt_assert_eq(ret, 0);
	gem_close(i915, bo);
}

static int create_ctx_with_params(int i915, bool with_protected_param, bool protected_is_true,
				  bool with_recoverable_param, bool recoverable_is_true,
				  uint32_t *ctx_out)
{
	uint32_t flags = 0;
	uint64_t extensions = 0;

	struct drm_i915_gem_context_create_ext_setparam p_prot = {
		.base = {
			.name = I915_CONTEXT_CREATE_EXT_SETPARAM,
			.next_extension = 0,
		},
		.param = {
			.param = I915_CONTEXT_PARAM_PROTECTED_CONTENT,
			.value = 0,
		}
	};
	struct drm_i915_gem_context_create_ext_setparam p_norecover = {
		.base = {
			.name = I915_CONTEXT_CREATE_EXT_SETPARAM,
			.next_extension = 0,
		},
		.param = {
			.param = I915_CONTEXT_PARAM_RECOVERABLE,
			.value = 0,
		}
	};

	p_prot.param.value = protected_is_true;
	p_norecover.param.value = recoverable_is_true;

	if (with_protected_param && with_recoverable_param) {
		p_norecover.base.next_extension = to_user_pointer(&(p_prot.base));
		flags = I915_CONTEXT_CREATE_FLAGS_USE_EXTENSIONS;
		extensions = to_user_pointer(&(p_norecover.base));
	} else if (!with_protected_param && with_recoverable_param) {
		p_norecover.base.next_extension = 0;
		flags = I915_CONTEXT_CREATE_FLAGS_USE_EXTENSIONS;
		extensions = to_user_pointer(&(p_norecover.base));
	} else if (with_protected_param && !with_recoverable_param) {
		p_prot.base.next_extension = 0;
		flags = I915_CONTEXT_CREATE_FLAGS_USE_EXTENSIONS;
		extensions = to_user_pointer(&(p_prot.base));
	}

	*ctx_out = 0;
	return __gem_context_create_ext(i915, flags, extensions, ctx_out);

}

#define CHANGE_PARAM_PROTECTED 0x0001
#define CHANGE_PARAM_RECOVERY 0x0002

static int modify_ctx_param(int i915, uint32_t ctx_id, uint32_t param_mask, bool param_value)
{
	int ret;

	struct drm_i915_gem_context_param ctx_param = {
		.ctx_id = ctx_id,
		.param = 0,
		.value = 0,
	};

	if (param_mask == CHANGE_PARAM_PROTECTED) {
		ctx_param.param = I915_CONTEXT_PARAM_PROTECTED_CONTENT;
		ctx_param.value = (int)param_value;
	} else if (param_mask == CHANGE_PARAM_RECOVERY) {
		ctx_param.param = I915_CONTEXT_PARAM_RECOVERABLE;
		ctx_param.value = (int)param_value;
	}

	ret = igt_ioctl(i915, DRM_IOCTL_I915_GEM_CONTEXT_SETPARAM, &ctx_param);

	return ret;
}

static int get_ctx_protected_param(int i915, uint32_t ctx_id)
{
	int ret;

	struct drm_i915_gem_context_param ctx_param = {
		.ctx_id = ctx_id,
		.param = I915_CONTEXT_PARAM_PROTECTED_CONTENT,
	};

	ret = igt_ioctl(i915, DRM_IOCTL_I915_GEM_CONTEXT_GETPARAM, &ctx_param);
	igt_assert_eq(ret, 0);

	return ctx_param.value;
}

static int get_ctx_recovery_param(int i915, uint32_t ctx_id)
{
	int ret;

	struct drm_i915_gem_context_param ctx_param = {
		.ctx_id = ctx_id,
		.param = I915_CONTEXT_PARAM_RECOVERABLE,
	};

	ret = igt_ioctl(i915, DRM_IOCTL_I915_GEM_CONTEXT_GETPARAM, &ctx_param);
	igt_assert_eq(ret, 0);

	return ctx_param.value;
}

static bool is_pxp_hw_supported(int i915)
{
	uint32_t tmpctx;
	int i = 0, ret;

	/* when running too soon after boot, its possible the component interface
	 * between i915 and MEI may have not yet established, give it some time
	 */
	while (i++ < 50) {
		ret = create_ctx_with_params(i915, true, true, true, false, &tmpctx);
		if (ret == 0) {
			gem_context_destroy(i915, tmpctx);
			return true;
		}
		usleep(50*1000);
	}
	return false;
}

static void test_ctx_alloc_pxp_nohw(int i915)
{
	uint32_t ctx;

	igt_assert_eq(create_ctx_with_params(i915, true, true, true, false, &ctx), -ENODEV);
	igt_assert_eq(create_ctx_with_params(i915, true, false, true, false, &ctx), 0);
	igt_assert_eq(get_ctx_protected_param(i915, ctx), 0);
	igt_assert_eq(get_ctx_recovery_param(i915, ctx), 0);
	gem_context_destroy(i915, ctx);
}

static void test_ctx_alloc_recover_off_protect_off(int i915)
{
	uint32_t ctx;

	igt_assert_eq(create_ctx_with_params(i915, true, false, true, false, &ctx), 0);
	igt_assert_eq(get_ctx_protected_param(i915, ctx), 0);
	igt_assert_eq(get_ctx_recovery_param(i915, ctx), 0);
	gem_context_destroy(i915, ctx);
}

static void test_ctx_alloc_recover_off_protect_on(int i915)
{
	uint32_t ctx;

	igt_assert_eq(create_ctx_with_params(i915, true, true, true, false, &ctx), 0);
	igt_assert_eq(get_ctx_protected_param(i915, ctx), 1);
	igt_assert_eq(get_ctx_recovery_param(i915, ctx), 0);
	gem_context_destroy(i915, ctx);
}

static void test_ctx_alloc_recover_on_protect_off(int i915)
{
	uint32_t ctx;

	igt_assert_eq(create_ctx_with_params(i915, true, false, true, true, &ctx), 0);
	igt_assert_eq(get_ctx_protected_param(i915, ctx), 0);
	igt_assert_eq(get_ctx_recovery_param(i915, ctx), 1);
	gem_context_destroy(i915, ctx);
}

static void test_ctx_alloc_recover_on_protect_on(int i915)
{
	uint32_t ctx;

	igt_assert_eq(create_ctx_with_params(i915, true, true, true, true, &ctx), -EPERM);
	igt_assert_eq(create_ctx_with_params(i915, true, true, false, false, &ctx), -EPERM);
}

static void test_ctx_mod_regular_to_all_valid(int i915)
{
	uint32_t ctx;

	igt_assert_eq(create_ctx_with_params(i915, false, false, false, false, &ctx), 0);
	igt_assert_eq(get_ctx_protected_param(i915, ctx), 0);
	igt_assert_eq(get_ctx_recovery_param(i915, ctx), 1);
	igt_assert_eq(modify_ctx_param(i915, ctx, CHANGE_PARAM_RECOVERY, false), 0);
	igt_assert_eq(modify_ctx_param(i915, ctx, CHANGE_PARAM_PROTECTED, true), -EPERM);
	igt_assert_eq(get_ctx_protected_param(i915, ctx), 0);
	igt_assert_eq(get_ctx_recovery_param(i915, ctx), 0);
	gem_context_destroy(i915, ctx);
}

static void test_ctx_mod_recover_off_to_on(int i915)
{
	uint32_t ctx;

	igt_assert_eq(create_ctx_with_params(i915, true, true, true, false, &ctx), 0);
	igt_assert_eq(get_ctx_protected_param(i915, ctx), 1);
	igt_assert_eq(get_ctx_recovery_param(i915, ctx), 0);
	igt_assert_eq(modify_ctx_param(i915, ctx, CHANGE_PARAM_RECOVERY, true), -EPERM);
	igt_assert_eq(get_ctx_recovery_param(i915, ctx), 0);
	gem_context_destroy(i915, ctx);
}

static void test_ctx_mod_protected_on_to_off(int i915)
{
	uint32_t ctx;

	igt_assert_eq(create_ctx_with_params(i915, true, true, true, false, &ctx), 0);
	igt_assert_eq(get_ctx_protected_param(i915, ctx), 1);
	igt_assert_eq(get_ctx_recovery_param(i915, ctx), 0);
	igt_assert_eq(modify_ctx_param(i915, ctx, CHANGE_PARAM_PROTECTED, false), -EPERM);
	igt_assert_eq(get_ctx_protected_param(i915, ctx), 1);
	igt_assert_eq(get_ctx_recovery_param(i915, ctx), 0);
	gem_context_destroy(i915, ctx);
}

static void test_ctx_mod_protected_to_all_invalid(int i915)
{
	uint32_t ctx;

	igt_assert_eq(create_ctx_with_params(i915, true, true, true, false, &ctx), 0);
	igt_assert_eq(get_ctx_protected_param(i915, ctx), 1);
	igt_assert_eq(get_ctx_recovery_param(i915, ctx), 0);
	igt_assert_eq(modify_ctx_param(i915, ctx, CHANGE_PARAM_RECOVERY, true), -EPERM);
	igt_assert_eq(modify_ctx_param(i915, ctx, CHANGE_PARAM_PROTECTED, false), -EPERM);
	igt_assert_eq(get_ctx_protected_param(i915, ctx), 1);
	igt_assert_eq(get_ctx_recovery_param(i915, ctx), 0);
	gem_context_destroy(i915, ctx);
}

static void fill_bo_content(int i915, uint32_t bo, uint32_t size, uint32_t initcolor)
{
	uint32_t *ptr, *ptrtmp;
	int loop = 0;

	ptr = gem_mmap__device_coherent(i915, bo, 0, size, PROT_WRITE);
	ptrtmp = ptr;

	/* read and count all dword matches till size */
	while (loop++ < (size/4)) {
		*ptrtmp = initcolor;
		++ptrtmp;
	}

	igt_assert(gem_munmap(ptr, size) == 0);
}

#define COMPARE_COLOR_READIBLE     1
#define COMPARE_COLOR_UNREADIBLE   2
#define COMPARE_BUFFER_READIBLE    3
#define COMPARE_BUFFER_UNREADIBLE  4
#define COPY_BUFFER                5
#define COMPARE_N_PIXELS_VERBOSELY 0

static void assert_bo_content_check(int i915, uint32_t bo, int compare_op, uint32_t size,
				    uint32_t color, uint32_t *auxptr, int auxsize)
{
	uint32_t *ptr, *ptrtmp, *auxtmp;
	int loop = 0, num_matches = 0;
	uint32_t value;
	bool op_readible = ((compare_op == COMPARE_COLOR_READIBLE) ||
		 (compare_op == COMPARE_BUFFER_READIBLE));
	bool chk_buff = ((compare_op == COMPARE_BUFFER_READIBLE) ||
		 (compare_op == COMPARE_BUFFER_UNREADIBLE));
	bool copy_buff = (compare_op == COPY_BUFFER);

	ptr = gem_mmap__device_coherent(i915, bo, 0, size, PROT_READ);
	ptrtmp = ptr;

	if (chk_buff || copy_buff) {
		if (auxsize < size)
			auxptr = NULL;
		igt_assert(auxptr);
		auxtmp = auxptr;
	}

	if (COMPARE_N_PIXELS_VERBOSELY) {
		igt_info("--------->>>\n");
		while (loop < COMPARE_N_PIXELS_VERBOSELY && loop < (size/4)) {
			value = *ptrtmp;
			if (chk_buff)
				color = *auxtmp;
			if (copy_buff)
				igt_info("Color copy = 0x%08x\n", value);
			else {
				igt_info("Color read = 0x%08x ", value);
				igt_info("expected %c= 0x%08x)\n", op_readible?'=':'!', color);
			}
			++auxtmp;
			++ptrtmp;
			++loop;
		}
		igt_info("<<<---------\n");
		auxtmp = auxptr;
		ptrtmp = ptr;
		loop = 0;
	}

	/* count all pixels for matches */
	while (loop++ < (size/4)) {
		value = *ptrtmp;
		switch (compare_op) {
		case COMPARE_COLOR_READIBLE:
		case COMPARE_COLOR_UNREADIBLE:
			if (value == color)
				++num_matches;
			break;
		case COMPARE_BUFFER_READIBLE:
		case COMPARE_BUFFER_UNREADIBLE:
			if (value == (*auxtmp))
				++num_matches;
			++auxtmp;
			break;
		case COPY_BUFFER:
			*auxtmp = value;
			++auxtmp;
			break;
		default:
			break;
		}
		++ptrtmp;
	}

	if (op_readible)
		igt_assert_eq(num_matches, (size/4));
	else
		igt_assert_eq(num_matches, 0);

	igt_assert(gem_munmap(ptr, size) == 0);
}

static uint32_t alloc_and_fill_dest_buff(int i915, bool protected, uint32_t size,
					 uint32_t init_color)
{
	uint32_t bo;
	int ret;

	ret = create_bo_ext(i915, size, protected, &bo);
	igt_assert_eq(ret, 0);
	igt_assert(bo);
	fill_bo_content(i915, bo, size, init_color);
	assert_bo_content_check(i915, bo, COMPARE_COLOR_READIBLE,
				size, init_color, NULL, 0);

	return bo;
}

/*
 * Rendering tests surface attributes, keep it simple:
 * page aligned width==stride, thus, and size
 */
#define TSTSURF_WIDTH       1024
#define TSTSURF_HEIGHT      128
#define TSTSURF_BYTESPP     4
#define TSTSURF_STRIDE      (TSTSURF_WIDTH*TSTSURF_BYTESPP)
#define TSTSURF_SIZE        (TSTSURF_STRIDE*TSTSURF_HEIGHT)
#define TSTSURF_FILLCOLOR1  0xfaceface
#define TSTSURF_FILLCOLOR2  0xdeaddead
#define TSTSURF_INITCOLOR1  0x12341234
#define TSTSURF_INITCOLOR2  0x56785678
#define TSTSURF_INITCOLOR3  0xabcdabcd

static void test_render_baseline(int i915)
{
	uint32_t ctx, srcbo, dstbo;
	struct intel_buf *srcbuf, *dstbuf;
	struct buf_ops *bops;
	struct intel_bb *ibb;
	uint32_t devid;
	int ret;

	devid = intel_get_drm_devid(i915);
	igt_assert(devid);

	bops = buf_ops_create(i915);
	igt_assert(bops);

	/* Perform a regular 3d copy as a control checkpoint */
	ret = create_ctx_with_params(i915, false, false, false, false, &ctx);
	igt_assert_eq(ret, 0);
	ibb = intel_bb_create_with_context(i915, ctx, 4096);
	igt_assert(ibb);

	dstbo = alloc_and_fill_dest_buff(i915, false, TSTSURF_SIZE, TSTSURF_INITCOLOR1);
	dstbuf = intel_buf_create_using_handle(bops, dstbo, TSTSURF_WIDTH, TSTSURF_HEIGHT,
					       TSTSURF_BYTESPP*8, 0, I915_TILING_NONE, 0);

	srcbo = alloc_and_fill_dest_buff(i915, false, TSTSURF_SIZE, TSTSURF_FILLCOLOR1);
	srcbuf = intel_buf_create_using_handle(bops, srcbo, TSTSURF_WIDTH, TSTSURF_HEIGHT,
					       TSTSURF_BYTESPP*8, 0, I915_TILING_NONE, 0);

	gen12_render_copyfunc(ibb, srcbuf, 0, 0, TSTSURF_WIDTH, TSTSURF_HEIGHT, dstbuf, 0, 0);
	gem_sync(i915, dstbo);

	assert_bo_content_check(i915, dstbo, COMPARE_COLOR_READIBLE,
				TSTSURF_SIZE, TSTSURF_FILLCOLOR1, NULL, 0);

	intel_bb_destroy(ibb);
	intel_buf_destroy(srcbuf);
	gem_close(i915, srcbo);
	intel_buf_destroy(dstbuf);
	gem_close(i915, dstbo);
	gem_context_destroy(i915, ctx);
	buf_ops_destroy(bops);
}

static void __test_render_pxp_src_to_protdest(int i915, uint32_t *outpixels, int outsize)
{
	uint32_t ctx, srcbo, dstbo;
	struct intel_buf *srcbuf, *dstbuf;
	struct buf_ops *bops;
	struct intel_bb *ibb;
	uint32_t devid;
	int ret;

	devid = intel_get_drm_devid(i915);
	igt_assert(devid);

	bops = buf_ops_create(i915);
	igt_assert(bops);

	/*
	 * Perform a protected render operation but only label
	 * the dest as protected. After rendering, the content
	 * should be encrypted
	 */
	ret = create_ctx_with_params(i915, true, true, true, false, &ctx);
	igt_assert_eq(ret, 0);
	igt_assert_eq(get_ctx_protected_param(i915, ctx), 1);
	ibb = intel_bb_create_with_context(i915, ctx, 4096);
	igt_assert(ibb);
	intel_bb_set_pxp(ibb, true, DISPLAY_APPTYPE, I915_PROTECTED_CONTENT_DEFAULT_SESSION);

	dstbo = alloc_and_fill_dest_buff(i915, true, TSTSURF_SIZE, TSTSURF_INITCOLOR2);
	dstbuf = intel_buf_create_using_handle(bops, dstbo, TSTSURF_WIDTH, TSTSURF_HEIGHT,
						TSTSURF_BYTESPP*8, 0, I915_TILING_NONE, 0);
	intel_buf_set_pxp(dstbuf, true);

	srcbo = alloc_and_fill_dest_buff(i915, false, TSTSURF_SIZE, TSTSURF_FILLCOLOR2);
	srcbuf = intel_buf_create_using_handle(bops, srcbo, TSTSURF_WIDTH, TSTSURF_HEIGHT,
						TSTSURF_BYTESPP*8, 0, I915_TILING_NONE, 0);

	gen12_render_copyfunc(ibb, srcbuf, 0, 0, TSTSURF_WIDTH, TSTSURF_HEIGHT, dstbuf, 0, 0);
	gem_sync(i915, dstbo);

	assert_bo_content_check(i915, dstbo, COMPARE_COLOR_UNREADIBLE,
				TSTSURF_SIZE, TSTSURF_FILLCOLOR2, NULL, 0);

	if (outpixels)
		assert_bo_content_check(i915, dstbo, COPY_BUFFER,
					TSTSURF_SIZE, 0, outpixels, outsize);

	intel_bb_destroy(ibb);
	intel_buf_destroy(srcbuf);
	gem_close(i915, srcbo);
	intel_buf_destroy(dstbuf);
	gem_close(i915, dstbo);
	gem_context_destroy(i915, ctx);
	buf_ops_destroy(bops);
}

static void test_render_pxp_src_to_protdest(int i915)
{
	__test_render_pxp_src_to_protdest(i915, NULL, 0);
}

static void test_render_pxp_protsrc_to_protdest(int i915)
{
	uint32_t ctx, srcbo, dstbo, dstbo2;
	struct intel_buf *srcbuf, *dstbuf, *dstbuf2;
	struct buf_ops *bops;
	struct intel_bb *ibb;
	uint32_t devid;
	int ret;
	uint32_t encrypted[TSTSURF_SIZE/TSTSURF_BYTESPP];

	devid = intel_get_drm_devid(i915);
	igt_assert(devid);

	bops = buf_ops_create(i915);
	igt_assert(bops);

	/*
	 * Perform a protected render operation but only label
	 * the dest as protected. After rendering, the content
	 * should be encrypted
	 */
	ret = create_ctx_with_params(i915, true, true, true, false, &ctx);
	igt_assert_eq(ret, 0);
	igt_assert_eq(get_ctx_protected_param(i915, ctx), 1);
	ibb = intel_bb_create_with_context(i915, ctx, 4096);
	igt_assert(ibb);
	intel_bb_set_pxp(ibb, true, DISPLAY_APPTYPE, I915_PROTECTED_CONTENT_DEFAULT_SESSION);

	dstbo = alloc_and_fill_dest_buff(i915, true, TSTSURF_SIZE, TSTSURF_INITCOLOR2);
	dstbuf = intel_buf_create_using_handle(bops, dstbo, TSTSURF_WIDTH, TSTSURF_HEIGHT,
						TSTSURF_BYTESPP*8, 0, I915_TILING_NONE, 0);
	intel_buf_set_pxp(dstbuf, true);

	srcbo = alloc_and_fill_dest_buff(i915, false, TSTSURF_SIZE, TSTSURF_FILLCOLOR2);
	srcbuf = intel_buf_create_using_handle(bops, srcbo, TSTSURF_WIDTH, TSTSURF_HEIGHT,
						TSTSURF_BYTESPP*8, 0, I915_TILING_NONE, 0);

	gen12_render_copyfunc(ibb, srcbuf, 0, 0, TSTSURF_WIDTH, TSTSURF_HEIGHT, dstbuf, 0, 0);
	gem_sync(i915, dstbo);

	assert_bo_content_check(i915, dstbo, COMPARE_COLOR_UNREADIBLE,
				TSTSURF_SIZE, TSTSURF_FILLCOLOR2, NULL, 0);

	/*
	 * Reuse prior dst as the new-src and create dst2 as the new-dest.
	 * Take a copy of encrypted content from new-src for comparison after render
	 * operation. After the rendering, we should find no difference in content
	 * since both new-src and new-dest are labelled as encrypted. HW should read
	 * and decrypt new-src, perform the render and re-encrypt when going into
	 * new-dest
	 */
	assert_bo_content_check(i915, dstbo, COPY_BUFFER,
				TSTSURF_SIZE, 0, encrypted, TSTSURF_SIZE);

	dstbo2 = alloc_and_fill_dest_buff(i915, true, TSTSURF_SIZE, TSTSURF_INITCOLOR3);
	dstbuf2 = intel_buf_create_using_handle(bops, dstbo2, TSTSURF_WIDTH, TSTSURF_HEIGHT,
						TSTSURF_BYTESPP*8, 0, I915_TILING_NONE, 0);
	intel_buf_set_pxp(dstbuf2, true);
	intel_buf_set_pxp(dstbuf, true);/*this time, src is protected*/

	intel_bb_set_pxp(ibb, true, DISPLAY_APPTYPE, I915_PROTECTED_CONTENT_DEFAULT_SESSION);

	gen12_render_copyfunc(ibb, dstbuf, 0, 0, TSTSURF_WIDTH, TSTSURF_HEIGHT, dstbuf2, 0, 0);
	gem_sync(i915, dstbo2);

	assert_bo_content_check(i915, dstbo2, COMPARE_BUFFER_READIBLE,
				TSTSURF_SIZE, 0, encrypted, TSTSURF_SIZE);

	intel_bb_destroy(ibb);
	intel_buf_destroy(srcbuf);
	gem_close(i915, srcbo);
	intel_buf_destroy(dstbuf);
	gem_close(i915, dstbo);
	intel_buf_destroy(dstbuf2);
	gem_close(i915, dstbo2);
	gem_context_destroy(i915, ctx);
	buf_ops_destroy(bops);
}

static void init_powermgt_resources(int i915, struct powermgt_data *pm)
{
	pm->debugfsdir = igt_debugfs_dir(i915);
	igt_require(pm->debugfsdir != -1);
	pm->has_runtime_pm = igt_setup_runtime_pm(i915);
	igt_require(pm->has_runtime_pm);
}

static void trigger_powermgt_suspend_cycle(int i915,
	struct powermgt_data *pm)
{
	igt_pm_enable_sata_link_power_management();
	igt_system_suspend_autoresume(SUSPEND_STATE_MEM, SUSPEND_TEST_DEVICES);
}

static void test_pxp_pwrcycle_teardown_keychange(int i915, struct powermgt_data *pm)
{
	uint32_t encrypted_pixels_b4[TSTSURF_SIZE/TSTSURF_BYTESPP];
	uint32_t encrypted_pixels_aft[TSTSURF_SIZE/TSTSURF_BYTESPP];
	int matched_after_keychange = 0, loop = 0;

	__test_render_pxp_src_to_protdest(i915, encrypted_pixels_b4, TSTSURF_SIZE);

	trigger_powermgt_suspend_cycle(i915, pm);

	__test_render_pxp_src_to_protdest(i915, encrypted_pixels_aft, TSTSURF_SIZE);

	while (loop < (TSTSURF_SIZE/TSTSURF_BYTESPP)) {
		if (encrypted_pixels_b4[loop] == encrypted_pixels_aft[loop])
			++matched_after_keychange;
		++loop;
	}
	igt_assert_eq(matched_after_keychange, 0);
}

#define GFX_OP_PIPE_CONTROL    ((3 << 29) | (3 << 27) | (2 << 24))
#define PIPE_CONTROL_CS_STALL	            (1 << 20)
#define PIPE_CONTROL_RENDER_TARGET_FLUSH    (1 << 12)
#define PIPE_CONTROL_FLUSH_ENABLE           (1 << 7)
#define PIPE_CONTROL_DATA_CACHE_INVALIDATE  (1 << 5)
#define PIPE_CONTROL_PROTECTEDPATH_DISABLE  (1 << 27)
#define PIPE_CONTROL_PROTECTEDPATH_ENABLE   (1 << 22)
#define PIPE_CONTROL_POST_SYNC_OP           (1 << 14)
#define PIPE_CONTROL_POST_SYNC_OP_STORE_DW_IDX (1 << 21)
#define PS_OP_TAG_BEFORE                    0x1234fed0
#define PS_OP_TAG_AFTER                     0x5678cbaf

static void emit_pipectrl(struct intel_bb *ibb, struct intel_buf *fenceb, bool before)
{
	uint32_t pipe_ctl_flags = 0;
	uint32_t ps_op_id;

	intel_bb_out(ibb, GFX_OP_PIPE_CONTROL);
	intel_bb_out(ibb, pipe_ctl_flags);

	if (before)
		ps_op_id = PS_OP_TAG_BEFORE;
	else
		ps_op_id = PS_OP_TAG_AFTER;

	pipe_ctl_flags = (PIPE_CONTROL_FLUSH_ENABLE |
			  PIPE_CONTROL_CS_STALL |
			  PIPE_CONTROL_POST_SYNC_OP);
	intel_bb_out(ibb, GFX_OP_PIPE_CONTROL | 4);
	intel_bb_out(ibb, pipe_ctl_flags);
	intel_bb_emit_reloc(ibb, fenceb->handle, 0, I915_GEM_DOMAIN_COMMAND, (before?0:8),
			    fenceb->addr.offset);
	intel_bb_out(ibb, ps_op_id);
	intel_bb_out(ibb, ps_op_id);
	intel_bb_out(ibb, MI_NOOP);
	intel_bb_out(ibb, MI_NOOP);
}

static void assert_pipectl_storedw_done(int i915, uint32_t bo)
{
	uint32_t *ptr;
	uint32_t success_mask = 0x0;

	ptr = gem_mmap__device_coherent(i915, bo, 0, 4096, PROT_READ);

	if (ptr[0] == PS_OP_TAG_BEFORE && ptr[1] == PS_OP_TAG_BEFORE)
		success_mask |= 0x1;

	igt_assert_eq(success_mask, 0x1);
	igt_assert(gem_munmap(ptr, 4096) == 0);
}

static int gem_execbuf_flush_store_dw(int i915, struct intel_bb *ibb, uint32_t ctx,
				      struct intel_buf *fence)
{
	int ret;

	intel_bb_ptr_set(ibb, 0);
	intel_bb_add_intel_buf(ibb, fence, true);
	emit_pipectrl(ibb, fence, true);
	intel_bb_emit_bbe(ibb);
	ret = __intel_bb_exec(ibb, intel_bb_offset(ibb),
				  I915_EXEC_RENDER | I915_EXEC_NO_RELOC, false);
	if (ret == 0) {
		gem_sync(ibb->i915, fence->handle);
		assert_pipectl_storedw_done(i915, fence->handle);
	}
	return ret;
}

static void prepare_exec_assets(int i915, struct simple_exec_assets *data, bool ctx_pxp,
				bool buf_pxp)
{
	int ret;

	if (ctx_pxp)
		ret = create_ctx_with_params(i915, true, true, true, false, &(data->ctx));
	else
		ret = create_ctx_with_params(i915, false, false, false, false, &(data->ctx));
	igt_assert_eq(ret, 0);
	igt_assert_eq(get_ctx_protected_param(i915, data->ctx), ctx_pxp);
	data->ibb = intel_bb_create_with_context(i915, data->ctx, 4096);
	igt_assert(data->ibb);

	data->fencebo = alloc_and_fill_dest_buff(i915, buf_pxp, 4096, 0);

	data->bops = buf_ops_create(i915);
	igt_assert(data->bops);

	data->fencebuf = intel_buf_create_using_handle(data->bops, data->fencebo, 256, 4,
						       32, 0, I915_TILING_NONE, 0);
	intel_bb_add_intel_buf(data->ibb, data->fencebuf, true);
}

static void free_exec_assets(int i915, struct simple_exec_assets *data)
{
	intel_bb_destroy(data->ibb);
	gem_close(i915, data->fencebo);
	intel_buf_destroy(data->fencebuf);
	gem_context_destroy(i915, data->ctx);
	buf_ops_destroy(data->bops);
}

static void trigger_pxp_debugfs_forced_teardown(int i915)
{
	int fd, ret;
	char str[32];

	fd = igt_debugfs_open(i915, "gt/pxp/terminate_state", O_RDWR);
	igt_assert_f(fd >= 0, "Can't open pxp termination debugfs\n");
	ret = snprintf(str, sizeof(str), "0x1");
	igt_assert(ret > 2 && ret < sizeof(str));
	ret = write(fd, str, ret);
	igt_assert_f(ret > 0, "Can't write pxp termination debugfs\n");

	close(fd);
}

static void test_pxp_stale_ctx_execution(int i915)
{
	int ret;
	struct simple_exec_assets data = {0};

	/*
	 * Use normal buffers for testing for invalidation
	 * of protected contexts to ensure kernel is catching
	 * the invalidated context (not buffer)
	 */
	prepare_exec_assets(i915, &data, true, false);
	ret = gem_execbuf_flush_store_dw(i915, data.ibb, data.ctx, data.fencebuf);
	igt_assert(ret == 0);

	trigger_pxp_debugfs_forced_teardown(i915);

	ret = gem_execbuf_flush_store_dw(i915, data.ibb, data.ctx, data.fencebuf);
	igt_assert_f((ret == -EIO), "Executing stale pxp context didn't fail with -EIO\n");

	free_exec_assets(i915, &data);
}

static void test_pxp_stale_buf_execution(int i915)
{
	int ret;
	struct simple_exec_assets data = {0};
	uint32_t ctx2;
	struct intel_bb *ibb2;

	/* Use pxp buffers with pxp context for testing for invalidation of protected buffers. */
	prepare_exec_assets(i915, &data, true, true);
	ret = gem_execbuf_flush_store_dw(i915, data.ibb, data.ctx, data.fencebuf);
	igt_assert(ret == 0);

	trigger_pxp_debugfs_forced_teardown(i915);

	/*
	 * After teardown, use a new pxp context but reuse the stale bo to ensure
	 * the kernel is catching the invalidated bo (not context)
	 */
	ret = create_ctx_with_params(i915, true, true, true, false, &ctx2);
	igt_assert_eq(ret, 0);
	igt_assert_eq(get_ctx_protected_param(i915, ctx2), 1);
	ibb2 = intel_bb_create_with_context(i915, ctx2, 4096);
	igt_assert(ibb2);
	intel_bb_remove_intel_buf(data.ibb, data.fencebuf);
	intel_bb_add_intel_buf(ibb2, data.fencebuf, true);
	ret = gem_execbuf_flush_store_dw(i915, ibb2, ctx2, data.fencebuf);
	igt_assert_f((ret == -ENOEXEC), "Executing stale pxp buffer didn't fail with -ENOEXEC\n");

	intel_bb_destroy(ibb2);
	gem_context_destroy(i915, ctx2);
	free_exec_assets(i915, &data);
}

static void test_pxp_stale_buf_optout_execution(int i915)
{
	int ret;
	struct simple_exec_assets data = {0};

	/*
	 * Use a normal context for testing opt-out behavior
	 * when executing with a pxp buffer across a teardown event.
	 */
	prepare_exec_assets(i915, &data, false, true);
	ret = gem_execbuf_flush_store_dw(i915, data.ibb, data.ctx, data.fencebuf);
	igt_assert(ret == 0);

	trigger_pxp_debugfs_forced_teardown(i915);

	ret = gem_execbuf_flush_store_dw(i915, data.ibb, data.ctx, data.fencebuf);
	igt_assert_f((ret == 0), "Opt-out-execution with stale pxp buffer didn't succeed\n");

	free_exec_assets(i915, &data);
}

igt_main
{
	int i915 = -1;
	bool pxp_supported = false;
	struct powermgt_data pm = {0};
	igt_render_copyfunc_t rendercopy = NULL;
	uint32_t devid = 0;

	igt_fixture
	{
		i915 = drm_open_driver(DRIVER_INTEL);
		igt_require(i915);
		igt_require_gem(i915);
		pxp_supported = is_pxp_hw_supported(i915);
	}

	igt_subtest_group {
		igt_fixture {
			igt_require((pxp_supported == 0));
		}

		igt_describe("Verify protected buffer on unsupported hw:");
		igt_subtest("hw-rejects-pxp-buffer")
			test_bo_alloc_pxp_nohw(i915);
		igt_describe("Verify protected context on unsupported hw:");
		igt_subtest("hw-rejects-pxp-context")
			test_ctx_alloc_pxp_nohw(i915);
	}

	igt_subtest_group {
		igt_fixture {
			igt_require(pxp_supported);
		}

		igt_describe("Verify protected buffer on supported hw:");
		igt_subtest("create-regular-buffer")
			test_bo_alloc_pxp_off(i915);
		igt_subtest("create-protected-buffer")
			test_bo_alloc_pxp_on(i915);

		igt_describe("Verify protected context on supported hw:");
		igt_subtest("create-regular-context-1")
			test_ctx_alloc_recover_off_protect_off(i915);
		igt_subtest("create-regular-context-2")
			test_ctx_alloc_recover_on_protect_off(i915);
		igt_subtest("fail-invalid-protected-context")
			test_ctx_alloc_recover_on_protect_on(i915);
		igt_subtest("create-valid-protected-context")
			test_ctx_alloc_recover_off_protect_on(i915);

		igt_describe("Verify protected context integrity:");
		igt_subtest("reject-modify-context-protection-on")
			test_ctx_mod_regular_to_all_valid(i915);
		igt_subtest("reject-modify-context-protection-off-1")
			test_ctx_mod_recover_off_to_on(i915);
		igt_subtest("reject-modify-context-protection-off-2")
			test_ctx_mod_protected_on_to_off(i915);
		igt_subtest("reject-modify-context-protection-off-3")
			test_ctx_mod_protected_to_all_invalid(i915);
	}
	igt_subtest_group {
		igt_fixture {
			igt_require(pxp_supported);
			devid = intel_get_drm_devid(i915);
			igt_assert(devid);
			rendercopy = igt_get_render_copyfunc(devid);
			igt_require(rendercopy);
		}

		igt_describe("Verify protected render operations:");
		igt_subtest("regular-baseline-src-copy-readible")
			test_render_baseline(i915);
		igt_subtest("protected-raw-src-copy-not-readible")
			test_render_pxp_src_to_protdest(i915);
		igt_subtest("protected-encrypted-src-copy-not-readible")
			test_render_pxp_protsrc_to_protdest(i915);
	}
	igt_subtest_group {
		igt_fixture {
			igt_require(pxp_supported);
			devid = intel_get_drm_devid(i915);
			igt_assert(devid);
			rendercopy = igt_get_render_copyfunc(devid);
			igt_require(rendercopy);
			init_powermgt_resources(i915, &pm);
		}
		igt_describe("Verify suspend-resume teardown management:");
		igt_subtest("verify-pxp-key-change-after-suspend-resume")
			test_pxp_pwrcycle_teardown_keychange(i915, &pm);
		igt_subtest("verify-pxp-stale-ctx-execution")
			test_pxp_stale_ctx_execution(i915);
		igt_subtest("verify-pxp-stale-buf-execution")
			test_pxp_stale_buf_execution(i915);
		igt_subtest("verify-pxp-stale-buf-optout-execution")
			test_pxp_stale_buf_optout_execution(i915);
	}

	igt_fixture {
		close(i915);
	}
}
