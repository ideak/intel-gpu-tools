/*
 * Copyright © 2019 Intel Corporation
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

#include <i915_drm.h>
#include "huc_copy.h"

static void
gen9_emit_huc_virtual_addr_state(struct drm_i915_gem_exec_object2 *src,
		struct drm_i915_gem_exec_object2 *dst,
		struct drm_i915_gem_relocation_entry *reloc_src,
		struct drm_i915_gem_relocation_entry *reloc_dst,
		uint32_t *buf,
		int *i)
{
	buf[(*i)++] = HUC_VIRTUAL_ADDR_STATE;

	for (int j = 0; j < HUC_VIRTUAL_ADDR_REGION_NUM; j++) {
		if (j == HUC_VIRTUAL_ADDR_REGION_SRC) {
			buf[(*i)++] = src->offset;

			reloc_src->target_handle = src->handle;
			reloc_src->delta = 0;
			reloc_src->offset = (*i - 1) * sizeof(buf[0]);
			reloc_src->read_domains = 0;
			reloc_src->write_domain = 0;
		} else if (j == HUC_VIRTUAL_ADDR_REGION_DST) {
			buf[(*i)++] = dst->offset;

			reloc_dst->target_handle = dst->handle;
			reloc_dst->delta = 0;
			reloc_dst->offset = (*i - 1) * sizeof(buf[0]);
			reloc_dst->read_domains = 0;
			reloc_dst->write_domain = I915_GEM_DOMAIN_RENDER;
		} else {
			buf[(*i)++] = 0;
		}
		buf[(*i)++] = 0;
		buf[(*i)++] = 0;
	}
}

void
gen9_huc_copyfunc(int fd,
		struct drm_i915_gem_exec_object2 *obj)
{
	struct drm_i915_gem_relocation_entry reloc[2];
	struct drm_i915_gem_execbuffer2 execbuf;
	int i = 0;
	uint32_t buf[63];

	/* load huc kernel */
	buf[i++] = HUC_IMEM_STATE;
	buf[i++] = 0;
	buf[i++] = 0;
	buf[i++] = 0;
	buf[i++] = 0x3;

	buf[i++] = MFX_WAIT;
	buf[i++] = MFX_WAIT;

	buf[i++] = HUC_PIPE_MODE_SELECT;
	buf[i++] = 0;
	buf[i++] = 0;

	buf[i++] = MFX_WAIT;

	memset(reloc, 0, sizeof(reloc));
	gen9_emit_huc_virtual_addr_state(&obj[0], &obj[1], &reloc[0], &reloc[1], buf, &i);

	buf[i++] = HUC_START;
	buf[i++] = 1;

	buf[i++] = MI_BATCH_BUFFER_END;

	gem_write(fd, obj[2].handle, 0, buf, sizeof(buf));
	obj[2].relocation_count = 2;
	obj[2].relocs_ptr = to_user_pointer(reloc);

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(obj);
	execbuf.buffer_count = 3;
	execbuf.flags = I915_EXEC_BSD;

	gem_execbuf(fd, &execbuf);
}
