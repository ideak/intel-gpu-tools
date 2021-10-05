// SPDX-License-Identifier: MIT
/*
 * Copyright © 2021 Intel Corporation
 */

#include "igt.h"
#include "i915/gem.h"
#include "i915/gem_create.h"

IGT_TEST_DESCRIPTION("Test PXP that manages protected content through arbitrated HW-PXP-session");
/* Note: PXP = "Protected Xe Path" */

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
#define COMPARE_N_PIXELS_VERBOSELY 0

static void assert_bo_content_check(int i915, uint32_t bo, int compare_op,
				    uint32_t size, uint32_t color)
{
	uint32_t *ptr, *ptrtmp;
	int loop = 0, num_matches = 0;
	uint32_t value;
	bool op_readible = (compare_op == COMPARE_COLOR_READIBLE);

	ptr = gem_mmap__device_coherent(i915, bo, 0, size, PROT_READ);
	ptrtmp = ptr;

	if (COMPARE_N_PIXELS_VERBOSELY) {
		igt_info("--------->>>\n");
		while (loop < COMPARE_N_PIXELS_VERBOSELY && loop < (size/4)) {
			value = *ptrtmp;
			igt_info("Color read = 0x%08x ", value);
			igt_info("expected %c= 0x%08x)\n", op_readible?'=':'!', color);
			++ptrtmp;
			++loop;
		}
		igt_info("<<<---------\n");
		ptrtmp = ptr;
		loop = 0;
	}

	/* count all pixels for matches */
	while (loop++ < (size/4)) {
		value = *ptrtmp;
		if (value == color)
			++num_matches;
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
	assert_bo_content_check(i915, bo, COMPARE_COLOR_READIBLE, size, init_color);

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
#define TSTSURF_INITCOLOR1  0x12341234

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
				TSTSURF_SIZE, TSTSURF_FILLCOLOR1);

	intel_bb_destroy(ibb);
	intel_buf_destroy(srcbuf);
	gem_close(i915, srcbo);
	intel_buf_destroy(dstbuf);
	gem_close(i915, dstbo);
	gem_context_destroy(i915, ctx);
	buf_ops_destroy(bops);
}

igt_main
{
	int i915 = -1;
	bool pxp_supported = false;
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
	}

	igt_fixture {
		close(i915);
	}
}
