/*
 * Copyright © 2009-2011 Intel Corporation
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

#ifndef INTEL_DECODE_H
#define INTEL_DECODE_H

#include <stdio.h>
#include <stdint.h>

struct intel_decode;

struct intel_decode *intel_decode_context_alloc(uint32_t devid);
void intel_decode_context_free(struct intel_decode *ctx);
void intel_decode_set_dump_past_end(struct intel_decode *ctx, int dump_past_end);
void intel_decode_set_batch_pointer(struct intel_decode *ctx,
				    void *data, uint32_t hw_offset, int count);
void intel_decode_set_head_tail(struct intel_decode *ctx,
				uint32_t head, uint32_t tail);
void intel_decode_set_output_file(struct intel_decode *ctx, FILE *output);
void intel_decode(struct intel_decode *ctx);

#endif /* INTEL_DECODE_H */
