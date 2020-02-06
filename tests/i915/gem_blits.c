/*
 * Copyright © 2018 Intel Corporation
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

#include "igt.h"
#include "igt_x86.h"

#define MI_FLUSH_DW (0x26 << 23)

#define BCS_SWCTRL 0x22200
#define BCS_SRC_Y (1 << 0)
#define BCS_DST_Y (1 << 1)

struct device {
	int fd;
	int gen;
	int pciid;
	int llc;
};

struct buffer {
	uint32_t handle;
	uint16_t width;
	uint16_t height;
	uint16_t stride;
	uint32_t size;
	unsigned int caching : 3;
	unsigned int tiling : 3;
	unsigned int fenced : 1;
	uint64_t gtt_offset;
	uint32_t model[] __attribute__((aligned(16)));
};

enum mode {
	CPU,
	PRW,
	GTT,
	WC,
};

static unsigned int
get_tiling_stride(const struct device *device,
		  unsigned int width, unsigned int tiling)
{
	unsigned int stride = 4u * width;

	if (tiling) {
		if (device->gen < 3)
			stride = ALIGN(stride, 128);
		else if (device->gen < 4 || tiling == I915_TILING_X)
			stride = ALIGN(stride, 512);
		else
			stride = ALIGN(stride, 128);
		if (device->gen < 4)
			stride = 1 << igt_fls(stride - 1);
	} else {
		if (device->gen >= 8)
			stride = ALIGN(stride, 64);
	}

	igt_assert(stride < UINT16_MAX && stride >= 4*width);
	return stride;
}

static unsigned int
get_tiling_height(const struct device *device,
		  unsigned int height, unsigned int tiling)
{
	if (!tiling)
		return height;

	if (device->gen < 3)
		return ALIGN(height, 16);
	else if (device->gen < 4 || tiling == I915_TILING_X)
		return ALIGN(height, 8);
	else
		return ALIGN(height, 32);
}

static struct buffer *buffer_create(const struct device *device,
				    unsigned int width,
				    unsigned int height)
{
	struct buffer *buffer;

	igt_assert(width && height);

	buffer = malloc(sizeof(*buffer) + 4u * width * height);
	if (!buffer)
		return NULL;

	buffer->width = width;
	buffer->height = height;

	buffer->tiling = I915_TILING_NONE;
	buffer->stride = get_tiling_stride(device, width, I915_TILING_NONE);
	buffer->size = ALIGN(buffer->stride * height, 4096);
	buffer->handle = gem_create(device->fd, buffer->size);
	buffer->caching = device->llc;

	buffer->gtt_offset = buffer->handle * buffer->size;

	for (int y = 0; y < height; y++) {
		uint32_t *row = buffer->model + y * width;

		for (int x = 0; x < width; x++)
			row[x] = (y << 16 | x) ^ buffer->handle;

		gem_write(device->fd,
			  buffer->handle, 4u * y * width,
			  row, 4u * width);
	}

	return buffer;
}

static void buffer_set_tiling(const struct device *device,
			      struct buffer *buffer,
			      unsigned int tiling)
{
	struct drm_i915_gem_exec_object2 obj[3];
	struct drm_i915_gem_relocation_entry reloc[2];
	struct drm_i915_gem_execbuffer2 execbuf;
	const bool has_64b_reloc = device->gen >= 8;
	uint32_t stride, size, pitch;
	uint32_t *batch;
	int i;

	if (buffer->tiling == tiling)
		return;

	stride = get_tiling_stride(device, buffer->width, tiling);
	size = stride * get_tiling_height(device, buffer->height, tiling);
	size = ALIGN(size, 4096);

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(obj);
	execbuf.buffer_count = ARRAY_SIZE(obj);
	if (device->gen >= 6)
		execbuf.flags = I915_EXEC_BLT;

	memset(obj, 0, sizeof(obj));
	obj[0].handle = gem_create(device->fd, size);
	if (__gem_set_tiling(device->fd, obj[0].handle, tiling, stride) == 0)
		obj[0].flags = EXEC_OBJECT_NEEDS_FENCE;

	obj[1].handle = buffer->handle;
	obj[1].offset = buffer->gtt_offset;
	if (buffer->fenced)
		obj[1].flags = EXEC_OBJECT_NEEDS_FENCE;

	obj[2].handle = gem_create(device->fd, 4096);
	obj[2].relocs_ptr = to_user_pointer(memset(reloc, 0, sizeof(reloc)));
	obj[2].relocation_count = 2;
	batch = gem_mmap__cpu(device->fd, obj[2].handle, 0, 4096, PROT_WRITE);

	i = 0;

	if ((tiling | buffer->tiling) >= I915_TILING_Y) {
		unsigned int mask;

		batch[i++] = MI_LOAD_REGISTER_IMM;
		batch[i++] = BCS_SWCTRL;

		mask = (BCS_SRC_Y | BCS_DST_Y) << 16;
		if (buffer->tiling == I915_TILING_Y)
			mask |= BCS_SRC_Y;
		if (tiling == I915_TILING_Y)
			mask |= BCS_DST_Y;
		batch[i++] = mask;
	}

	batch[i] = (XY_SRC_COPY_BLT_CMD |
		    XY_SRC_COPY_BLT_WRITE_ALPHA |
		    XY_SRC_COPY_BLT_WRITE_RGB);
	if (device->gen >= 4 && buffer->tiling)
		batch[i] |= XY_SRC_COPY_BLT_SRC_TILED;
	if (device->gen >= 4 && tiling)
		batch[i] |= XY_SRC_COPY_BLT_DST_TILED;
	batch[i++] |= 6 + 2 * has_64b_reloc;

	pitch = stride;
	if (device->gen >= 4 && tiling)
		pitch /= 4;
	batch[i++] = 3 << 24 | 0xcc << 16 | pitch;
	batch[i++] = 0;
	batch[i++] = buffer->height << 16 | buffer->width;
	reloc[0].target_handle = obj[0].handle;
	reloc[0].presumed_offset = obj[0].offset;
	reloc[0].offset = sizeof(*batch) * i;
	reloc[0].read_domains = I915_GEM_DOMAIN_RENDER;
	reloc[0].write_domain = I915_GEM_DOMAIN_RENDER;
	batch[i++] = obj[0].offset;
	if (has_64b_reloc)
		batch[i++] = obj[0].offset >> 32;

	batch[i++] = 0;
	pitch = buffer->stride;
	if (device->gen >= 4 && buffer->tiling)
		pitch /= 4;
	batch[i++] = pitch;
	reloc[1].target_handle = obj[1].handle;
	reloc[1].presumed_offset = obj[1].offset;
	reloc[1].offset = sizeof(*batch) * i;
	reloc[1].read_domains = I915_GEM_DOMAIN_RENDER;
	batch[i++] = obj[1].offset;
	if (has_64b_reloc)
		batch[i++] = obj[1].offset >> 32;

	if ((tiling | buffer->tiling) >= I915_TILING_Y) {
		igt_assert(device->gen >= 6);
		batch[i++] = MI_FLUSH_DW | 2;
		batch[i++] = 0;
		batch[i++] = 0;
		batch[i++] = 0;

		batch[i++] = MI_LOAD_REGISTER_IMM;
		batch[i++] = BCS_SWCTRL;
		batch[i++] = (BCS_SRC_Y | BCS_DST_Y) << 16;
	}

	batch[i++] = MI_BATCH_BUFFER_END;
	munmap(batch, 4096);

	gem_execbuf(device->fd, &execbuf);

	gem_close(device->fd, obj[2].handle);
	gem_close(device->fd, obj[1].handle);

	buffer->gtt_offset = obj[0].offset;
	buffer->handle = obj[0].handle;

	buffer->fenced = !!(obj[0].flags & EXEC_OBJECT_NEEDS_FENCE);
	buffer->tiling = tiling;
	buffer->stride = stride;
	buffer->size = size;
}

static bool can_blit_to_linear(const struct device *device,
			       const struct buffer *buffer)
{
	if (buffer->caching && !device->llc)
		return false;

	if (device->gen < 3)
		return false;

	return true;
}

static bool blit_to_linear(const struct device *device,
			   const struct buffer *buffer,
			   void *linear)
{
	struct drm_i915_gem_exec_object2 obj[3];
	struct drm_i915_gem_relocation_entry reloc[2];
	struct drm_i915_gem_execbuffer2 execbuf;
	const bool has_64b_reloc = device->gen >= 8;
	uint32_t *batch;
	uint32_t pitch;
	int i = 0;

	igt_assert(buffer->tiling);

	if (!can_blit_to_linear(device, buffer))
		return false;

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(obj);
	execbuf.buffer_count = ARRAY_SIZE(obj);
	if (device->gen >= 6)
		execbuf.flags = I915_EXEC_BLT;

	memset(obj, 0, sizeof(obj));
	if (__gem_userptr(device->fd, linear, buffer->size, 0, 0, &obj[0].handle))
		return false;

	obj[1].handle = buffer->handle;
	obj[1].offset = buffer->gtt_offset;
	obj[1].flags = EXEC_OBJECT_NEEDS_FENCE;

	memset(reloc, 0, sizeof(reloc));
	obj[2].handle = gem_create(device->fd, 4096);
	obj[2].relocs_ptr = to_user_pointer(reloc);
	obj[2].relocation_count = ARRAY_SIZE(reloc);
	batch = gem_mmap__cpu(device->fd, obj[2].handle, 0, 4096, PROT_WRITE);

	if (buffer->tiling >= I915_TILING_Y) {
		unsigned int mask;

		batch[i++] = MI_LOAD_REGISTER_IMM;
		batch[i++] = BCS_SWCTRL;

		mask = (BCS_SRC_Y | BCS_DST_Y) << 16;
		if (buffer->tiling == I915_TILING_Y)
			mask |= BCS_SRC_Y;
		batch[i++] = mask;
	}

	batch[i] = (XY_SRC_COPY_BLT_CMD |
		    XY_SRC_COPY_BLT_WRITE_ALPHA |
		    XY_SRC_COPY_BLT_WRITE_RGB);
	if (device->gen >= 4 && buffer->tiling)
		batch[i] |= XY_SRC_COPY_BLT_SRC_TILED;
	batch[i++] |= 6 + 2 * has_64b_reloc;

	batch[i++] = 3 << 24 | 0xcc << 16 | buffer->stride;
	batch[i++] = 0;
	batch[i++] = buffer->height << 16 | buffer->width;
	reloc[0].target_handle = obj[0].handle;
	reloc[0].presumed_offset = obj[0].offset;
	reloc[0].offset = sizeof(*batch) * i;
	reloc[0].read_domains = I915_GEM_DOMAIN_RENDER;
	reloc[0].write_domain = I915_GEM_DOMAIN_RENDER;
	batch[i++] = obj[0].offset;
	if (has_64b_reloc)
		batch[i++] = obj[0].offset >> 32;

	batch[i++] = 0;
	pitch = buffer->stride;
	if (device->gen >= 4 && buffer->tiling)
		pitch /= 4;
	batch[i++] = pitch;
	reloc[1].target_handle = obj[1].handle;
	reloc[1].presumed_offset = obj[1].offset;
	reloc[1].offset = sizeof(*batch) * i;
	reloc[1].read_domains = I915_GEM_DOMAIN_RENDER;
	batch[i++] = obj[1].offset;
	if (has_64b_reloc)
		batch[i++] = obj[1].offset >> 32;

	if (buffer->tiling >= I915_TILING_Y) {
		igt_assert(device->gen >= 6);
		batch[i++] = MI_FLUSH_DW | 2;
		batch[i++] = 0;
		batch[i++] = 0;
		batch[i++] = 0;

		batch[i++] = MI_LOAD_REGISTER_IMM;
		batch[i++] = BCS_SWCTRL;
		batch[i++] = (BCS_SRC_Y | BCS_DST_Y) << 16;
	}

	batch[i++] = MI_BATCH_BUFFER_END;
	munmap(batch, 4096);

	gem_execbuf(device->fd, &execbuf);
	gem_close(device->fd, obj[2].handle);

	gem_sync(device->fd, obj[0].handle);
	gem_close(device->fd, obj[0].handle);

	return true;
}

static void *download(const struct device *device,
		      const struct buffer *buffer,
		      enum mode mode)
{
	void *linear, *src;

	igt_assert(posix_memalign(&linear, 4096, buffer->size) == 0);

	if (buffer->tiling && !buffer->fenced) {
		igt_assert(blit_to_linear(device, buffer, linear));
		return linear;
	}

	switch (mode) {
	case CPU:
		if (buffer->tiling) {
			if (blit_to_linear(device, buffer, linear))
				return linear;

			mode = GTT;
		}
		break;

	case WC:
		if (!gem_mmap__has_wc(device->fd) || buffer->tiling)
			mode = GTT;
		break;

	case PRW:
		if (buffer->tiling)
			mode = GTT;
		break;

	case GTT:
		break;
	}

	switch (mode) {
	case CPU:
		src = gem_mmap__cpu(device->fd, buffer->handle,
				    0, buffer->size,
				    PROT_READ);

		gem_set_domain(device->fd, buffer->handle,
			       I915_GEM_DOMAIN_CPU, 0);
		igt_memcpy_from_wc(linear, src, buffer->size);
		munmap(src, buffer->size);
		break;

	case WC:
		src = gem_mmap__wc(device->fd, buffer->handle,
				   0, buffer->size,
				   PROT_READ);

		gem_set_domain(device->fd, buffer->handle,
			       I915_GEM_DOMAIN_WC, 0);
		igt_memcpy_from_wc(linear, src, buffer->size);
		munmap(src, buffer->size);
		break;

	case GTT:
		src = gem_mmap__gtt(device->fd, buffer->handle,
				   buffer->size,
				   PROT_READ);

		gem_set_domain(device->fd, buffer->handle,
			       I915_GEM_DOMAIN_GTT, 0);
		igt_memcpy_from_wc(linear, src, buffer->size);
		munmap(src, buffer->size);
		break;

	case PRW:
		gem_read(device->fd, buffer->handle, 0, linear, buffer->size);
		break;
	}

	return linear;
}

static bool buffer_check(const struct device *device,
			 const struct buffer *buffer,
			 enum mode mode)
{
	unsigned int num_errors = 0;
	uint32_t *linear;

	linear = download(device, buffer, mode);
	igt_assert(linear);

	for (int y = 0; y < buffer->height; y++) {
		const uint32_t *model = buffer->model + y * buffer->width;
		const uint32_t *row =
			linear + y * buffer->stride / sizeof(uint32_t);

		if (!memcmp(model, row, buffer->width * sizeof(uint32_t)))
			continue;

		for (int x = 0; x < buffer->width; x++) {
			if (row[x] != model[x] && num_errors++ < 5) {
				igt_warn("buffer handle=%d mismatch at (%d, %d): expected %08x, found %08x\n",
					 buffer->handle,
					 x, y, model[x], row[x]);
			}
		}
	}

	free(linear);

	return num_errors == 0;
}

static void buffer_free(const struct device *device, struct buffer *buffer)
{
	igt_assert(buffer_check(device, buffer, GTT));
	gem_close(device->fd, buffer->handle);
	free(buffer);
}

static void memcpy_blt(const void *src, void *dst,
		       uint32_t src_stride, uint32_t dst_stride,
		       uint16_t src_x, uint16_t src_y,
		       uint16_t dst_x, uint16_t dst_y,
		       uint16_t width, uint16_t height)
{
	const uint8_t *src_bytes;
	uint8_t *dst_bytes;
	int byte_width;

	src_bytes = (const uint8_t *)src + src_stride * src_y + src_x * 4;
	dst_bytes = (uint8_t *)dst + dst_stride * dst_y + dst_x * 4;

	byte_width = width * 4;
	if (byte_width == src_stride && byte_width == dst_stride) {
		byte_width *= height;
		height = 1;
	}

	switch (byte_width) {
	case 4:
		do {
			*(uint32_t *)dst_bytes = *(const uint32_t *)src_bytes;
			src_bytes += src_stride;
			dst_bytes += dst_stride;
		} while (--height);
		break;

	case 8:
		do {
			*(uint64_t *)dst_bytes = *(const uint64_t *)src_bytes;
			src_bytes += src_stride;
			dst_bytes += dst_stride;
		} while (--height);
		break;
	case 16:
		do {
			((uint64_t *)dst_bytes)[0] = ((const uint64_t *)src_bytes)[0];
			((uint64_t *)dst_bytes)[1] = ((const uint64_t *)src_bytes)[1];
			src_bytes += src_stride;
			dst_bytes += dst_stride;
		} while (--height);
		break;

	default:
		do {
			memcpy(dst_bytes, src_bytes, byte_width);
			src_bytes += src_stride;
			dst_bytes += dst_stride;
		} while (--height);
		break;
	}
}

static void
blit(const struct device *device,
     struct buffer *src, uint16_t src_x, uint16_t src_y,
     struct buffer *dst, uint16_t dst_x, uint16_t dst_y,
     uint16_t width, uint16_t height)

{
	struct drm_i915_gem_exec_object2 obj[3];
	struct drm_i915_gem_relocation_entry reloc[2];
	struct drm_i915_gem_execbuffer2 execbuf;
	const bool has_64b_reloc = device->gen >= 8;
	uint32_t *batch;
	uint32_t pitch;
	int i = 0;

	if (src_x < 0) {
		width += src_x;
		dst_x -= src_x;
		src_x  = 0;
	}
	if (src_y < 0) {
		height += src_y;
		dst_y  -= src_y;
		src_y   = 0;
	}

	if (dst_x < 0) {
		width += dst_x;
		src_x -= dst_x;
		dst_x  = 0;
	}
	if (dst_y < 0) {
		height += dst_y;
		src_y  -= dst_y;
		dst_y   = 0;
	}

	if (src_x + width > src->width)
		width = src->width - src_x;
	if (dst_x + width > dst->width)
		width = dst->width - dst_x;

	if (src_y + height > src->height)
		height = src->height - src_y;
	if (dst_y + height > dst->height)
		height = dst->height - dst_y;

	if (dst->caching) {
		igt_assert(device->gen >= 3);
		igt_assert(device->llc || !src->caching);
	}

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(obj);
	execbuf.buffer_count = ARRAY_SIZE(obj);
	if (device->gen >= 6)
		execbuf.flags = I915_EXEC_BLT;

	memset(obj, 0, sizeof(obj));
	obj[0].handle = dst->handle;
	obj[0].offset = dst->gtt_offset;
	if (dst->tiling)
		obj[0].flags = EXEC_OBJECT_NEEDS_FENCE;

	obj[1].handle = src->handle;
	obj[1].offset = src->gtt_offset;
	if (src->tiling)
		obj[1].flags = EXEC_OBJECT_NEEDS_FENCE;

	memset(reloc, 0, sizeof(reloc));
	obj[2].handle = gem_create(device->fd, 4096);
	obj[2].relocs_ptr = to_user_pointer(reloc);
	obj[2].relocation_count = ARRAY_SIZE(reloc);
	batch = gem_mmap__cpu(device->fd, obj[2].handle, 0, 4096, PROT_WRITE);

	if ((src->tiling | dst->tiling) >= I915_TILING_Y) {
		unsigned int mask;

		batch[i++] = MI_LOAD_REGISTER_IMM;
		batch[i++] = BCS_SWCTRL;

		mask = (BCS_SRC_Y | BCS_DST_Y) << 16;
		if (src->tiling == I915_TILING_Y)
			mask |= BCS_SRC_Y;
		if (dst->tiling == I915_TILING_Y)
			mask |= BCS_DST_Y;
		batch[i++] = mask;
	}

	batch[i] = (XY_SRC_COPY_BLT_CMD |
		    XY_SRC_COPY_BLT_WRITE_ALPHA |
		    XY_SRC_COPY_BLT_WRITE_RGB);
	if (device->gen >= 4 && src->tiling)
		batch[i] |= XY_SRC_COPY_BLT_SRC_TILED;
	if (device->gen >= 4 && dst->tiling)
		batch[i] |= XY_SRC_COPY_BLT_DST_TILED;
	batch[i++] |= 6 + 2 * has_64b_reloc;

	pitch = dst->stride;
	if (device->gen >= 4 && dst->tiling)
		pitch /= 4;
	batch[i++] = 3 << 24 | 0xcc << 16 | pitch;

	batch[i++] = dst_y << 16 | dst_x;
	batch[i++] = (height + dst_y) << 16 | (width + dst_x);
	reloc[0].target_handle = obj[0].handle;
	reloc[0].presumed_offset = obj[0].offset;
	reloc[0].offset = sizeof(*batch) * i;
	reloc[0].read_domains = I915_GEM_DOMAIN_RENDER;
	reloc[0].write_domain = I915_GEM_DOMAIN_RENDER;
	batch[i++] = obj[0].offset;
	if (has_64b_reloc)
		batch[i++] = obj[0].offset >> 32;

	batch[i++] = src_y << 16 | src_x;
	pitch = src->stride;
	if (device->gen >= 4 && src->tiling)
		pitch /= 4;
	batch[i++] = pitch;
	reloc[1].target_handle = obj[1].handle;
	reloc[1].presumed_offset = obj[1].offset;
	reloc[1].offset = sizeof(*batch) * i;
	reloc[1].read_domains = I915_GEM_DOMAIN_RENDER;
	batch[i++] = obj[1].offset;
	if (has_64b_reloc)
		batch[i++] = obj[1].offset >> 32;

	if ((src->tiling | dst->tiling) >= I915_TILING_Y) {
		igt_assert(device->gen >= 6);
		batch[i++] = MI_FLUSH_DW | 2;
		batch[i++] = 0;
		batch[i++] = 0;
		batch[i++] = 0;

		batch[i++] = MI_LOAD_REGISTER_IMM;
		batch[i++] = BCS_SWCTRL;
		batch[i++] = (BCS_SRC_Y | BCS_DST_Y) << 16;
	}

	batch[i++] = MI_BATCH_BUFFER_END;
	munmap(batch, 4096);

	gem_execbuf(device->fd, &execbuf);
	gem_close(device->fd, obj[2].handle);

	dst->gtt_offset = obj[0].offset;
	src->gtt_offset = obj[1].offset;

	memcpy_blt(src->model, dst->model,
		   4u * src->width, 4u * dst->width,
		   src_x, src_y,
		   dst_x, dst_y,
		   width, height);
}

enum start {
	ZERO,
	ABOVE,
	BELOW
};

static int start_at(int x, enum start s)
{
	switch (s) {
	default:
	case ZERO:
		return 0;
	case ABOVE:
		return 1;
	case BELOW:
		return x - 1;
	}
}

igt_main
{
	struct device device;

	igt_fixture {
		device.fd = drm_open_driver_render(DRIVER_INTEL);
		igt_require_gem(device.fd);
		gem_require_blitter(device.fd);

		device.pciid = intel_get_drm_devid(device.fd);
		device.gen = intel_gen(device.pciid);
		device.llc = gem_has_llc(device.fd);
	}

	igt_subtest("basic") {
		struct buffer *src, *dst;
		unsigned int x, y;

		for (unsigned int height = 1; height <= 16; height <<= 1) {
			for (unsigned int y0 = ZERO; y0 <= (height > 2 ? BELOW : ZERO); y0++) {
				for (unsigned int width = 1; width <= 64; width <<= 1) {
					for (unsigned int x0 = ZERO; x0 <= (width > 2 ? BELOW : ZERO); x0++) {

						src = buffer_create(&device,
								    width * 16, height * 4);
						dst = buffer_create(&device,
								    width * 16, height * 4);

						y = start_at(height, y0);
						for (unsigned int src_tiling = I915_TILING_NONE;
						     src_tiling <= (device.gen >= 6 ? I915_TILING_Y : I915_TILING_X);
						     src_tiling++) {
							buffer_set_tiling(&device, src, src_tiling);

							x = start_at(width, x0);
							for (unsigned int dst_tiling = I915_TILING_NONE;
							     dst_tiling <= (device.gen >= 6 ? I915_TILING_Y : I915_TILING_X);
							     dst_tiling++) {
								buffer_set_tiling(&device, dst, dst_tiling);

								for (enum mode down = CPU; down <= WC; down++) {
									if (down == GTT && !gem_has_mappable_ggtt(device.fd))
										continue;

									igt_debug("Testing src_tiling=%d, dst_tiling=%d, down=%d at (%d, %d) x (%d, %d)\n",
										  src_tiling,
										  dst_tiling,
										  down, x, y,
										  width, height);

									igt_assert(x + width <= dst->width);
									igt_assert(y + height <= dst->height);

									blit(&device,
									     src, x, y,
									     dst, x, y,
									     width, height);
									igt_assert(buffer_check(&device, dst, down));

									x += width;
								}
							}

							y += height;
						}

						buffer_free(&device, dst);
						buffer_free(&device, src);
					}
				}
			}
		}
	}
}
