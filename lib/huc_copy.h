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

#ifndef HUC_COPY_H
#define HUC_COPY_H

#include <stdint.h>
#include <string.h>
#include "ioctl_wrappers.h"
#include "intel_reg.h"

#define PARALLEL_VIDEO_PIPE		(0x3<<29)
#define MFX_WAIT			(PARALLEL_VIDEO_PIPE|(0x1<<27)|(0x1<<8))

#define HUC_IMEM_STATE			(PARALLEL_VIDEO_PIPE|(0x2<<27)|(0xb<<23)|(0x1<<16)|0x3)
#define HUC_PIPE_MODE_SELECT		(PARALLEL_VIDEO_PIPE|(0x2<<27)|(0xb<<23)|0x1)
#define HUC_START			(PARALLEL_VIDEO_PIPE|(0x2<<27)|(0xb<<23)|(0x21<<16))
#define HUC_VIRTUAL_ADDR_STATE		(PARALLEL_VIDEO_PIPE|(0x2<<27)|(0xb<<23)|(0x4<<16)|0x2f)

#define HUC_VIRTUAL_ADDR_REGION_NUM	16
#define HUC_VIRTUAL_ADDR_REGION_SRC	0
#define HUC_VIRTUAL_ADDR_REGION_DST	14

void
gen9_huc_copyfunc(int fd,
		struct drm_i915_gem_exec_object2 *obj);

#endif /* HUC_COPY_H */
