/*
 * Copyright © 2007, 2011, 2013, 2014, 2019 Intel Corporation
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
 *
 */

#include <stdbool.h>
#include <sys/ioctl.h>
#include <errno.h>

#include "igt_core.h"
#include "igt_device.h"
#include "ioctl_wrappers.h"
#include "intel_chipset.h"

#include "gem_create.h"
#include "gem_mman.h"

#ifdef HAVE_VALGRIND
#include <valgrind/valgrind.h>
#include <valgrind/memcheck.h>

#define VG(x) x
#else
#define VG(x) do {} while (0)
#endif

static int gem_mmap_gtt_version(int fd)
{
	struct drm_i915_getparam gp;
	int gtt_version = -1;

	memset(&gp, 0, sizeof(gp));
	gp.param = I915_PARAM_MMAP_GTT_VERSION;
	gp.value = &gtt_version;
	ioctl(fd, DRM_IOCTL_I915_GETPARAM, &gp);

	return gtt_version;
}

bool gem_has_mmap_offset(int fd)
{
	int gtt_version = gem_mmap_gtt_version(fd);

	return gtt_version >= 4;
}

bool gem_has_mmap_offset_type(int fd, const struct mmap_offset *t)
{
	return gem_has_mmap_offset(fd) || t->type == I915_MMAP_OFFSET_GTT;
}

/**
 * __gem_mmap__gtt:
 * @fd: open i915 drm file descriptor
 * @handle: gem buffer object handle
 * @size: size of the gem buffer
 * @prot: memory protection bits as used by mmap()
 *
 * This functions wraps up procedure to establish a memory mapping through the
 * GTT.
 *
 * Returns: A pointer to the created memory mapping, NULL on failure.
 */
void *__gem_mmap__gtt(int fd, uint32_t handle, uint64_t size, unsigned prot)
{
	struct drm_i915_gem_mmap_gtt mmap_arg;
	void *ptr;

	memset(&mmap_arg, 0, sizeof(mmap_arg));
	mmap_arg.handle = handle;
	if (igt_ioctl(fd, DRM_IOCTL_I915_GEM_MMAP_GTT, &mmap_arg))
		return NULL;

	ptr = mmap64(0, size, prot, MAP_SHARED, fd, mmap_arg.offset);
	if (ptr == MAP_FAILED)
		ptr = NULL;
	else
		errno = 0;

	VG(VALGRIND_MAKE_MEM_DEFINED(ptr, size));

	return ptr;
}

/**
 * gem_mmap__gtt:
 * @fd: open i915 drm file descriptor
 * @handle: gem buffer object handle
 * @size: size of the gem buffer
 * @prot: memory protection bits as used by mmap()
 *
 * Like __gem_mmap__gtt() except we assert on failure.
 *
 * Returns: A pointer to the created memory mapping
 */
void *gem_mmap__gtt(int fd, uint32_t handle, uint64_t size, unsigned prot)
{
	void *ptr = __gem_mmap__gtt(fd, handle, size, prot);
	igt_assert(ptr);
	return ptr;
}

int gem_munmap(void *ptr, uint64_t size)
{
	int ret = munmap(ptr, size);

	if (ret == 0)
		VG(VALGRIND_MAKE_MEM_NOACCESS(ptr, size));

	return ret;
}

bool gem_mmap__has_wc(int fd)
{
	int has_wc = 0;

	struct drm_i915_getparam gp;
	int mmap_version = -1;

	memset(&gp, 0, sizeof(gp));
	gp.param = I915_PARAM_MMAP_VERSION;
	gp.value = &mmap_version;
	ioctl(fd, DRM_IOCTL_I915_GETPARAM, &gp);

	/* Do we have the mmap_ioctl with DOMAIN_WC? */
	if (mmap_version >= 1 && gem_mmap_gtt_version(fd) >= 2) {
		struct drm_i915_gem_mmap arg;

		/* Does this device support wc-mmaps ? */
		memset(&arg, 0, sizeof(arg));
		arg.handle = gem_create(fd, 4096);
		arg.offset = 0;
		arg.size = 4096;
		arg.flags = I915_MMAP_WC;
		has_wc = igt_ioctl(fd, DRM_IOCTL_I915_GEM_MMAP, &arg) == 0;
		gem_close(fd, arg.handle);

		if (has_wc && from_user_pointer(arg.addr_ptr))
			munmap(from_user_pointer(arg.addr_ptr), arg.size);
	}
	errno = 0;

	return has_wc > 0;
}

bool gem_mmap_offset__has_wc(int fd)
{
	int has_wc = 0;
	struct drm_i915_gem_mmap_offset arg;

	if (!gem_has_mmap_offset(fd))
		return false;

	/* Does this device support wc-mmaps ? */
	memset(&arg, 0, sizeof(arg));
	arg.handle = gem_create(fd, 4096);
	arg.offset = 0;
	arg.flags = I915_MMAP_OFFSET_WC;
	has_wc = igt_ioctl(fd, DRM_IOCTL_I915_GEM_MMAP_OFFSET,
			   &arg) == 0;
	gem_close(fd, arg.handle);

	errno = 0;

	return has_wc > 0;
}

/**
 * __gem_mmap:
 * @fd: open i915 drm file descriptor
 * @handle: gem buffer object handle
 * @offset: offset in the gem buffer of the mmap arena
 * @size: size of the mmap arena
 * @prot: memory protection bits as used by mmap()
 * @flags: flags used to determine caching
 *
 * This functions wraps up procedure to establish a memory mapping through
 * direct cpu access, bypassing the gpu (valid for wc == false). For wc == true
 * it also bypass cpu caches completely and GTT system agent (i.e. there is no
 * automatic tiling of the mmapping through the fence registers).
 *
 * Returns: A pointer to the created memory mapping, NULL on failure.
 */
static void *__gem_mmap(int fd, uint32_t handle, uint64_t offset, uint64_t size,
			unsigned int prot, uint64_t flags)
{
	struct drm_i915_gem_mmap arg;

	memset(&arg, 0, sizeof(arg));
	arg.handle = handle;
	arg.offset = offset;
	arg.size = size;
	arg.flags = flags;

	if (igt_ioctl(fd, DRM_IOCTL_I915_GEM_MMAP, &arg))
		return NULL;

	VG(VALGRIND_MAKE_MEM_DEFINED(from_user_pointer(arg.addr_ptr), arg.size));

	errno = 0;
	return from_user_pointer(arg.addr_ptr);
}

/**
 * __gem_mmap_offset:
 * @fd: open i915 drm file descriptor
 * @handle: gem buffer object handle
 * @offset: offset in the gem buffer of the mmap arena
 * @size: size of the mmap arena
 * @prot: memory protection bits as used by mmap()
 * @flags: flags used to determine caching
 *
 * Mmap the gem buffer memory on offset returned in GEM_MMAP_OFFSET ioctl.
 * Offset argument passed in function call must be 0. In the future
 * when driver will allow slice mapping of buffer object this restriction
 * will be removed.
 *
 * Returns: A pointer to the created memory mapping, NULL on failure.
 */
void *__gem_mmap_offset(int fd, uint32_t handle, uint64_t offset, uint64_t size,
			unsigned int prot, uint64_t flags)
{
	struct drm_i915_gem_mmap_offset arg;
	void *ptr;

	if (!gem_has_mmap_offset(fd))
		return NULL;

	igt_assert(offset == 0);

	memset(&arg, 0, sizeof(arg));
	arg.handle = handle;
	arg.flags = flags;

	if (igt_ioctl(fd, DRM_IOCTL_I915_GEM_MMAP_OFFSET, &arg))
		return NULL;

	ptr = mmap64(0, size, prot, MAP_SHARED, fd, arg.offset + offset);

	if (ptr == MAP_FAILED)
		ptr = NULL;
	else
		errno = 0;

	return ptr;
}

/**
 * __gem_mmap__wc:
 * @fd: open i915 drm file descriptor
 * @handle: gem buffer object handle
 * @offset: offset in the gem buffer of the mmap arena
 * @size: size of the mmap arena
 * @prot: memory protection bits as used by mmap()
 *
 * This function wraps up procedure to establish a memory mapping through
 * direct cpu access, bypassing the gpu and cpu caches completely and also
 * bypassing the GTT system agent (i.e. there is no automatic tiling of
 * the mmapping through the fence registers).
 *
 * Returns: A pointer to the created memory mapping, NULL on failure.
 */
void *__gem_mmap__wc(int fd, uint32_t handle, uint64_t offset, uint64_t size, unsigned prot)
{
	return __gem_mmap(fd, handle, offset, size, prot, I915_MMAP_WC);
}

/**
 * gem_mmap__wc:
 * @fd: open i915 drm file descriptor
 * @handle: gem buffer object handle
 * @offset: offset in the gem buffer of the mmap arena
 * @size: size of the mmap arena
 * @prot: memory protection bits as used by mmap()
 *
 * Try to __gem_mmap__wc(). Assert on failure.
 *
 * Returns: A pointer to the created memory mapping
 */
void *gem_mmap__wc(int fd, uint32_t handle, uint64_t offset, uint64_t size, unsigned prot)
{
	void *ptr = __gem_mmap__wc(fd, handle, offset, size, prot);
	igt_assert(ptr);
	return ptr;
}

/**
 * __gem_mmap_offset__wc:
 * @fd: open i915 drm file descriptor
 * @handle: gem buffer object handle
 * @offset: offset in the gem buffer of the mmap arena
 * @size: size of the mmap arena
 * @prot: memory protection bits as used by mmap()
 *
 * This function wraps up procedure to establish a memory mapping through
 * direct cpu access, bypassing the gpu and cpu caches completely and also
 * bypassing the GTT system agent (i.e. there is no automatic tiling of
 * the mmapping through the fence registers).
 *
 * Returns: A pointer to the created memory mapping, NULL on failure.
 */
void *__gem_mmap_offset__wc(int fd, uint32_t handle, uint64_t offset,
			    uint64_t size, unsigned prot)
{
	return __gem_mmap_offset(fd, handle, offset, size, prot,
				 I915_MMAP_OFFSET_WC);
}

/**
 * gem_mmap_offset__wc:
 * @fd: open i915 drm file descriptor
 * @handle: gem buffer object handle
 * @offset: offset in the gem buffer of the mmap arena
 * @size: size of the mmap arena
 * @prot: memory protection bits as used by mmap()
 *
 * Try to __gem_mmap_offset__wc(). Assert on failure.
 *
 * Returns: A pointer to the created memory mapping
 */
void *gem_mmap_offset__wc(int fd, uint32_t handle, uint64_t offset,
			  uint64_t size, unsigned prot)
{
	void *ptr = __gem_mmap_offset__wc(fd, handle, offset, size, prot);

	igt_assert(ptr);
	return ptr;
}

/**
 * __gem_mmap__device_coherent:
 * @fd: open i915 drm file descriptor
 * @handle: gem buffer object handle
 * @offset: offset in the gem buffer of the mmap arena
 * @size: size of the mmap arena
 * @prot: memory protection bits as used by mmap()
 *
 * Returns: A pointer to a block of linear device memory mapped into the
 * process with WC semantics. When no WC is available try to mmap using GGTT.
 */
void *__gem_mmap__device_coherent(int fd, uint32_t handle, uint64_t offset,
				  uint64_t size, unsigned prot)
{
	void *ptr = __gem_mmap_offset(fd, handle, offset, size, prot,
				      I915_MMAP_OFFSET_WC);
	if (!ptr)
		ptr = __gem_mmap__wc(fd, handle, offset, size, prot);

	if (!ptr)
		ptr = __gem_mmap__gtt(fd, handle, size, prot);

	return ptr;
}

/**
 * gem_mmap__device_coherent:
 * @fd: open i915 drm file descriptor
 * @handle: gem buffer object handle
 * @offset: offset in the gem buffer of the mmap arena
 * @size: size of the mmap arena
 * @prot: memory protection bits as used by mmap()
 *
 * Call __gem_mmap__device__coherent(), asserts on fail.
 * Offset argument passed in function call must be 0. In the future
 * when driver will allow slice mapping of buffer object this restriction
 * will be removed.
 *
 * Returns: A pointer to the created memory mapping.
 */
void *gem_mmap__device_coherent(int fd, uint32_t handle, uint64_t offset,
				uint64_t size, unsigned prot)
{
	void *ptr;

	igt_assert(offset == 0);

	ptr = __gem_mmap__device_coherent(fd, handle, offset, size, prot);
	igt_assert(ptr);

	return ptr;
}

/**
 * __gem_mmap__cpu:
 * @fd: open i915 drm file descriptor
 * @handle: gem buffer object handle
 * @offset: offset in the gem buffer of the mmap arena
 * @size: size of the mmap arena
 * @prot: memory protection bits as used by mmap()
 *
 * This functions wraps up procedure to establish a memory mapping through
 * direct cpu access, bypassing the gpu completely.
 *
 * Returns: A pointer to the created memory mapping, NULL on failure.
 */
void *__gem_mmap__cpu(int fd, uint32_t handle, uint64_t offset, uint64_t size, unsigned prot)
{
	return __gem_mmap(fd, handle, offset, size, prot, 0);
}

/**
 * gem_mmap__cpu:
 * @fd: open i915 drm file descriptor
 * @handle: gem buffer object handle
 * @offset: offset in the gem buffer of the mmap arena
 * @size: size of the mmap arena
 * @prot: memory protection bits as used by mmap()
 *
 * Like __gem_mmap__cpu() except we assert on failure.
 *
 * Returns: A pointer to the created memory mapping
 */
void *gem_mmap__cpu(int fd, uint32_t handle, uint64_t offset, uint64_t size, unsigned prot)
{
	void *ptr = __gem_mmap__cpu(fd, handle, offset, size, prot);
	igt_assert(ptr);
	return ptr;
}

/**
 * __gem_mmap_offset__cpu:
 * @fd: open i915 drm file descriptor
 * @handle: gem buffer object handle
 * @offset: offset in the gem buffer of the mmap arena
 * @size: size of the mmap arena
 * @prot: memory protection bits as used by mmap()
 *
 * This function wraps up procedure to establish a memory mapping through
 * direct cpu access.
 *
 * Returns: A pointer to the created memory mapping, NULL on failure.
 */
void *__gem_mmap_offset__cpu(int fd, uint32_t handle, uint64_t offset,
			     uint64_t size, unsigned prot)
{
	return __gem_mmap_offset(fd, handle, offset, size, prot,
				 I915_MMAP_OFFSET_WB);
}

/**
 * gem_mmap_offset__cpu:
 * @fd: open i915 drm file descriptor
 * @handle: gem buffer object handle
 * @offset: offset in the gem buffer of the mmap arena
 * @size: size of the mmap arena
 * @prot: memory protection bits as used by mmap()
 *
 * Like __gem_mmap__cpu() except we assert on failure.
 *
 * Returns: A pointer to the created memory mapping
 */
void *gem_mmap_offset__cpu(int fd, uint32_t handle, uint64_t offset,
			   uint64_t size, unsigned prot)
{
	void *ptr = __gem_mmap_offset(fd, handle, offset, size, prot,
				      I915_MMAP_OFFSET_WB);

	igt_assert(ptr);
	return ptr;
}

/**
 * __gem_mmap__cpu_coherent:
 * @fd: open i915 drm file descriptor
 * @handle: gem buffer object handle
 * @offset: offset in the gem buffer of the mmap arena
 * @size: size of the mmap arena
 * @prot: memory protection bits as used by mmap()
 *
 * This function wraps up procedure to establish a memory mapping through
 * direct cpu access.
 */
void *__gem_mmap__cpu_coherent(int fd, uint32_t handle, uint64_t offset,
			       uint64_t size, unsigned prot)
{
	void *ptr = __gem_mmap_offset__cpu(fd, handle, offset, size, prot);

	if (!ptr)
		ptr = __gem_mmap__cpu(fd, handle, offset, size, prot);

	return ptr;
}

/**
 * gem_mmap__cpu_coherent:
 * @fd: open i915 drm file descriptor
 * @handle: gem buffer object handle
 * @offset: offset in the gem buffer of the mmap arena
 * @size: size of the mmap arena
 * @prot: memory protection bits as used by mmap()
 *
 * Call __gem_mmap__cpu__coherent(), asserts on fail.
 * Offset argument passed in function call must be 0. In the future
 * when driver will allow slice mapping of buffer object this restriction
 * will be removed.
 *
 * Returns: A pointer to the created memory mapping.
 */
void *gem_mmap__cpu_coherent(int fd, uint32_t handle, uint64_t offset,
			     uint64_t size, unsigned prot)
{
	void *ptr;

	igt_assert(offset == 0);

	ptr = __gem_mmap__cpu_coherent(fd, handle, offset, size, prot);
	igt_assert(ptr);

	return ptr;
}

bool gem_has_mappable_ggtt(int i915)
{
	struct drm_i915_gem_mmap_gtt arg = {};
	int err;

	err = 0;
	if (ioctl(i915, DRM_IOCTL_I915_GEM_MMAP_GTT, &arg))
		err = errno;
	errno = 0;

	return err != ENODEV;
}

void gem_require_mappable_ggtt(int i915)
{
	igt_require_f(gem_has_mappable_ggtt(i915),
		      "HW & kernel support for indirect detiling aperture\n");
}

const struct mmap_offset mmap_offset_types[] = {
	{ "gtt", I915_MMAP_OFFSET_GTT, I915_GEM_DOMAIN_GTT },
	{ "wb", I915_MMAP_OFFSET_WB, I915_GEM_DOMAIN_CPU },
	{ "wc", I915_MMAP_OFFSET_WC, I915_GEM_DOMAIN_WC },
	{ "uc", I915_MMAP_OFFSET_UC, I915_GEM_DOMAIN_WC },
	{},
};

/**
 * gem_available_aperture_size:
 * @fd: open i915 drm file descriptor
 *
 * Feature test macro to query the kernel for the available gpu aperture size
 * usable in a batchbuffer.
 *
 * Returns: The available gtt address space size.
 */
uint64_t gem_available_aperture_size(int fd)
{
	struct drm_i915_gem_get_aperture aperture = {
		aperture.aper_available_size = 256*1024*1024,
	};

	igt_ioctl(fd, DRM_IOCTL_I915_GEM_GET_APERTURE, &aperture);
	errno = 0;

	return aperture.aper_available_size;
}

/**
 * gem_aperture_size:
 * @fd: open i915 drm file descriptor
 *
 * Feature test macro to query the kernel for the total gpu aperture size.
 *
 * Returns: The total gtt address space size.
 */
uint64_t gem_aperture_size(int fd)
{
	struct drm_i915_gem_context_param p = {
		.param = I915_CONTEXT_PARAM_GTT_SIZE
	};

	if (__gem_context_get_param(fd, &p))
		p.value = gem_global_aperture_size(fd);

	return p.value;
}

/**
 * gem_mappable_aperture_size:
 *
 * Feature test macro to query the kernel for the mappable gpu aperture size.
 * This is the area available for GTT memory mappings.
 *
 * Returns: The mappable gtt address space size.
 */
uint64_t gem_mappable_aperture_size(int fd)
{
	struct pci_device *pci_dev = igt_device_get_pci_device(fd);
	int bar;

	if (intel_gen(pci_dev->device_id) < 3)
		bar = 0;
	else
		bar = 2;

	return pci_dev->regions[bar].size;
}

/**
 * gem_global_aperture_size:
 * @fd: open i915 drm file descriptor
 *
 * Feature test macro to query the kernel for the global gpu aperture size.
 * This is the area available for the kernel to perform address translations.
 *
 * Returns: The gtt address space size.
 */
uint64_t gem_global_aperture_size(int fd)
{
	struct drm_i915_gem_get_aperture aperture = {
		aperture.aper_size = 256*1024*1024
	};

	igt_ioctl(fd, DRM_IOCTL_I915_GEM_GET_APERTURE, &aperture);
	errno = 0;

	return aperture.aper_size;
}

/**
 * gem_available_fences:
 * @fd: open i915 drm file descriptor
 *
 * Feature test macro to query the kernel for the number of available fences
 * usable in a batchbuffer. Only relevant for pre-gen4.
 *
 * Returns: The number of available fences.
 */
int gem_available_fences(int fd)
{
	int num_fences = 0;
	struct drm_i915_getparam gp = {
		gp.param = I915_PARAM_NUM_FENCES_AVAIL,
		gp.value = &num_fences,
	};

	ioctl(fd, DRM_IOCTL_I915_GETPARAM, &gp, sizeof(gp));
	errno = 0;

	return num_fences;
}
