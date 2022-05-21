/*
 * Copyright © 2008 Intel Corporation
 * Copyright (c) 2012, Oracle and/or its affiliates. All rights reserved.
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

#ifndef IGT_OS_H
#define IGT_OS_H


/* These are separate to allow easier testing when porting, see the comment at
 * the bottom of intel_os.c. */
uint64_t igt_get_total_ram_mb(void);
uint64_t igt_get_avail_ram_mb(void);
uint64_t igt_get_total_swap_mb(void);
void *igt_get_total_pinnable_mem(size_t *pinned);

int __igt_check_memory(uint64_t count, uint64_t size, unsigned mode,
		       uint64_t *out_required, uint64_t *out_total);
void igt_require_memory(uint64_t count, uint64_t size, unsigned mode);
void igt_require_files(uint64_t count);
#define CHECK_RAM 0x1
#define CHECK_SWAP 0x2

void igt_purge_vm_caches(int drm_fd);

#endif /* IGT_OS_H */
