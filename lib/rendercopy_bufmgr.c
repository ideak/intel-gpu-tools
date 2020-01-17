/*
 * Copyright © 2020 Intel Corporation
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

#include <sys/ioctl.h>
#include "igt.h"
#include "igt_x86.h"
#include "rendercopy_bufmgr.h"

/**
 * SECTION:rendercopy_bufmgr
 * @short_description: Render copy buffer manager
 * @title: Render copy bufmgr
 * @include: igt.h
 *
 * # Rendercopy buffer manager
 *
 * Rendercopy depends on libdrm and igt_buf, so some middle layer to intel_buf
 * and buf_ops is required.
 *
 * |[<!-- language="c" -->
 * struct rendercopy_bufmgr *bmgr;
 * ...
 * bmgr = rendercopy_bufmgr_create(fd, bufmgr);
 * ...
 * igt_buf_init(bmgr, &buf, 512, 512, 32, I915_TILING_X, false);
 * ...
 * linear_to_igt_buf(bmgr, &buf, linear);
 * ...
 * igt_buf_to_linear(bmgr, &buf, linear);
 * ...
 * rendercopy_bufmgr_destroy(bmgr);
 * ]|
 */

struct rendercopy_bufmgr {
	int fd;
	drm_intel_bufmgr *bufmgr;
	struct buf_ops *bops;
};

static void __igt_buf_to_intel_buf(struct igt_buf *buf, struct intel_buf *ibuf)
{
	ibuf->handle = buf->bo->handle;
	ibuf->stride = buf->surface[0].stride;
	ibuf->tiling = buf->tiling;
	ibuf->bpp = buf->bpp;
	ibuf->size = buf->surface[0].size;
	ibuf->compression = buf->compression;
	ibuf->aux.offset = buf->ccs[0].offset;
	ibuf->aux.stride = buf->ccs[0].stride;
}

void igt_buf_to_linear(struct rendercopy_bufmgr *bmgr, struct igt_buf *buf,
		       uint32_t *linear)
{
	struct intel_buf ibuf;

	__igt_buf_to_intel_buf(buf, &ibuf);

	intel_buf_to_linear(bmgr->bops, &ibuf, linear);
}

void linear_to_igt_buf(struct rendercopy_bufmgr *bmgr, struct igt_buf *buf,
		       uint32_t *linear)
{
	struct intel_buf ibuf;

	__igt_buf_to_intel_buf(buf, &ibuf);

	linear_to_intel_buf(bmgr->bops, &ibuf, linear);
}

struct rendercopy_bufmgr *
rendercopy_bufmgr_create(int fd, drm_intel_bufmgr *bufmgr)
{
	struct buf_ops *bops;
	struct rendercopy_bufmgr *bmgr;

	igt_assert(bufmgr);

	bops = buf_ops_create(fd);
	igt_assert(bops);

	bmgr = calloc(1, sizeof(*bmgr));
	igt_assert(bmgr);

	bmgr->fd = fd;
	bmgr->bufmgr = bufmgr;
	bmgr->bops = bops;

	return bmgr;
}

void rendercopy_bufmgr_destroy(struct rendercopy_bufmgr *bmgr)
{
	igt_assert(bmgr);
	igt_assert(bmgr->bops);

	buf_ops_destroy(bmgr->bops);
	free(bmgr);
}

bool rendercopy_bufmgr_set_software_tiling(struct rendercopy_bufmgr *bmgr,
					   uint32_t tiling,
					   bool use_software_tiling)
{
	return buf_ops_set_software_tiling(bmgr->bops, tiling,
					   use_software_tiling);
}

void igt_buf_init(struct rendercopy_bufmgr *bmgr, struct igt_buf *buf,
		  int width, int height, int bpp,
		  uint32_t tiling, uint32_t compression)
{
	uint32_t devid = intel_get_drm_devid(bmgr->fd);
	int generation= intel_gen(devid);
	struct intel_buf ibuf;
	int size;

	memset(buf, 0, sizeof(*buf));

	buf->surface[0].stride = ALIGN(width * (bpp / 8), 128);
	buf->surface[0].size = buf->surface[0].stride * height;
	buf->tiling = tiling;
	buf->bpp = bpp;
	buf->compression = compression;

	size = buf->surface[0].stride * ALIGN(height, 32);

	if (compression) {
		int ccs_width = igt_buf_intel_ccs_width(generation, buf);
		int ccs_height = igt_buf_intel_ccs_height(generation, buf);

		buf->ccs[0].offset = buf->surface[0].stride * ALIGN(height, 32);
		buf->ccs[0].stride = ccs_width;

		size = buf->ccs[0].offset + ccs_width * ccs_height;
	}

	buf->bo = drm_intel_bo_alloc(bmgr->bufmgr, "", size, 4096);

	intel_buf_init_using_handle(bmgr->bops,
				    buf->bo->handle,
				    &ibuf,
				    width, height, bpp, tiling,
				    compression);

	buf->ccs[0].offset = ibuf.aux.offset;
	buf->ccs[0].stride = ibuf.aux.stride;
}
