/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2021 Intel Corporation
 */
#ifndef _I915_DRM_LOCAL_H_
#define _I915_DRM_LOCAL_H_

#if defined(__cplusplus)
extern "C" {
#endif

/*
 * It is necessary on occasion to add uapi declarations to IGT before they
 * appear in imported kernel uapi headers. This header is provided for this
 * purpose.

 * Early uapi declarations should be added here exactly as they are
 * expected to appear in the kernel uapi headers, i.e. without the LOCAL_
 * or local_ prefix and without any #ifndef's. Attempt should be made to
 * clean these up when kernel uapi headers are sync'd.
 */
#define I915_ENGINE_CLASS_COMPUTE 4

/* Needed for PXP */
#define I915_GEM_CREATE_EXT_PROTECTED_CONTENT  1
#define I915_CONTEXT_PARAM_PROTECTED_CONTENT   0xd
#define I915_PROTECTED_CONTENT_DEFAULT_SESSION 0xf

/* Needed for PXP */
struct drm_i915_gem_create_ext_protected_content {
	/** @base: Extension link. See struct i915_user_extension. */
	struct i915_user_extension base;
	/** @flags: reserved for future usage, currently MBZ */
	__u32 flags;
};

#if defined(__cplusplus)
}
#endif

#endif /* _I915_DRM_LOCAL_H_ */
