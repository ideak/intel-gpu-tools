/*
 * Copyright © 2017 Intel Corporation
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
 */

#ifndef GEM_CONTEXT_H
#define GEM_CONTEXT_H

uint32_t gem_context_create(int fd);
void gem_context_destroy(int fd, uint32_t ctx_id);
int __gem_context_destroy(int fd, uint32_t ctx_id);
struct local_i915_gem_context_param {
	uint32_t context;
	uint32_t size;
	uint64_t param;
#define LOCAL_CONTEXT_PARAM_BAN_PERIOD	0x1
#define LOCAL_CONTEXT_PARAM_NO_ZEROMAP	0x2
#define LOCAL_CONTEXT_PARAM_GTT_SIZE	0x3
#define LOCAL_CONTEXT_PARAM_NO_ERROR_CAPTURE	0x4
#define LOCAL_CONTEXT_PARAM_BANNABLE	0x5
	uint64_t value;
};
void gem_context_require_bannable(int fd);
void gem_context_require_param(int fd, uint64_t param);
void gem_context_get_param(int fd, struct local_i915_gem_context_param *p);
void gem_context_set_param(int fd, struct local_i915_gem_context_param *p);
int __gem_context_set_param(int fd, struct local_i915_gem_context_param *p);
int __gem_context_get_param(int fd, struct local_i915_gem_context_param *p);

#endif /* GEM_CONTEXT_H */
