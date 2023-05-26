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

#define DRM_I915_QUERY_GEOMETRY_SUBSLICES      6

#define DRM_I915_PERF_PROP_OA_ENGINE_CLASS	9
#define DRM_I915_PERF_PROP_OA_ENGINE_INSTANCE	10

/*
 * Top 4 bits of every non-engine counter are GT id.
 */
#define __I915_PMU_GT_SHIFT (60)

#define ___I915_PMU_OTHER(gt, x) \
	(((__u64)__I915_PMU_ENGINE(0xff, 0xff, 0xf) + 1 + (x)) | \
	((__u64)(gt) << __I915_PMU_GT_SHIFT))

#define __I915_PMU_ACTUAL_FREQUENCY(gt)		___I915_PMU_OTHER(gt, 0)
#define __I915_PMU_REQUESTED_FREQUENCY(gt)	___I915_PMU_OTHER(gt, 1)
#define __I915_PMU_INTERRUPTS(gt)		___I915_PMU_OTHER(gt, 2)
#define __I915_PMU_RC6_RESIDENCY(gt)		___I915_PMU_OTHER(gt, 3)
#define __I915_PMU_SOFTWARE_GT_AWAKE_TIME(gt)	___I915_PMU_OTHER(gt, 4)

#define I915_GEM_CREATE_EXT_SET_PAT 2

/**
 * struct drm_i915_gem_create_ext_set_pat - The
 * I915_GEM_CREATE_EXT_SET_PAT extension.
 *
 * If this extension is provided, the specified caching policy (PAT index) is
 * applied to the buffer object.
 *
 * Below is an example on how to create an object with specific caching policy:
 *
 * .. code-block:: C
 *
 *	struct drm_i915_gem_create_ext_set_pat set_pat_ext = {
 *		.base = { .name = I915_GEM_CREATE_EXT_SET_PAT },
 *		.pat_index = 0,
 *	};
 *	struct drm_i915_gem_create_ext create_ext = {
 *		.size = PAGE_SIZE,
 *		.extensions = (uintptr_t)&set_pat_ext,
 *	};
 *
 *	int err = ioctl(fd, DRM_IOCTL_I915_GEM_CREATE_EXT, &create_ext);
 *	if (err) ...
 */
struct drm_i915_gem_create_ext_set_pat {
	/** @base: Extension link. See struct i915_user_extension. */
	struct i915_user_extension base;
	/** @pat_index: PAT index to be set */
	__u32 pat_index;
	/** @rsvd: reserved for future use */
	__u32 rsvd;
};

#if defined(__cplusplus)
}
#endif

#endif /* _I915_DRM_LOCAL_H_ */
