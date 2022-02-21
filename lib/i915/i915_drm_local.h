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

/*
 * Signal to the kernel that the object will need to be accessed via
 * the CPU.
 *
 * Only valid when placing objects in I915_MEMORY_CLASS_DEVICE, and only
 * strictly required on platforms where only some of the device memory
 * is directly visible or mappable through the CPU, like on DG2+.
 *
 * One of the placements MUST also be I915_MEMORY_CLASS_SYSTEM, to
 * ensure we can always spill the allocation to system memory, if we
 * can't place the object in the mappable part of
 * I915_MEMORY_CLASS_DEVICE.
 *
 * Without this hint, the kernel will assume that non-mappable
 * I915_MEMORY_CLASS_DEVICE is preferred for this object. Note that the
 * kernel can still migrate the object to the mappable part, as a last
 * resort, if userspace ever CPU faults this object, but this might be
 * expensive, and so ideally should be avoided.
 */
#define I915_GEM_CREATE_EXT_FLAG_NEEDS_CPU_ACCESS (1 << 0)

#if defined(__cplusplus)
}
#endif

#endif /* _I915_DRM_LOCAL_H_ */
