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
 */

#ifndef __IGT_PARAMS_H__
#define __IGT_PARAMS_H__

#include <stdbool.h>

int igt_params_open(int device);

char *__igt_params_get(int device, const char *parameter);

__attribute__((format(printf, 3, 4)))
bool igt_params_set(int device, const char *parameter, const char *fmt, ...);

__attribute__((format(printf, 3, 4)))
bool igt_params_save_and_set(int device, const char *parameter, const char *fmt, ...);

void igt_set_module_param(int device, const char *name, const char *val);
void igt_set_module_param_int(int device, const char *name, int val);

#endif /* __IGT_PARAMS_H__ */
