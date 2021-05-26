// SPDX-License-Identifier: MIT
/*
 * Copyright © 2021 Intel Corporation
 */

#ifndef GEM_CREATE_H
#define GEM_CREATE_H

#include <stdint.h>

#include "i915_drm.h"

int __gem_create(int fd, uint64_t *size, uint32_t *handle);
uint32_t gem_create(int fd, uint64_t size);
int __gem_create_ext(int fd, uint64_t *size, uint32_t *handle,
                     struct i915_user_extension *ext);
uint32_t gem_create_ext(int fd, uint64_t size, struct i915_user_extension *ext);

#endif /* GEM_CREATE_H */
