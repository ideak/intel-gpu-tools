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

#ifndef GEM_ENGINE_TOPOLOGY_H
#define GEM_ENGINE_TOPOLOGY_H

#include "igt_gt.h"
#include "i915_drm.h"
#include "intel_ctx.h"

int __gem_query_engines(int fd,
			struct drm_i915_query_engine_info *query_engines,
			int length);

/**
 * intel_engine_data:
 * @nengines: Number of engines
 * @n: Current engine index
 * @current_engine: Current engine
 * @engines: List of all engines
 *
 * This struct acts as an interator for walking over a set of engines.
 */
struct intel_engine_data {
	uint32_t nengines;
	uint32_t n;
	struct intel_execution_engine2 *current_engine;
	struct intel_execution_engine2 engines[GEM_MAX_ENGINES];
};

bool gem_has_engine_topology(int fd);
struct intel_engine_data intel_engine_list_of_physical(int fd);
struct intel_engine_data intel_engine_list_for_ctx_cfg(int fd, const intel_ctx_cfg_t *cfg);

/* iteration functions */
struct intel_execution_engine2 *
intel_get_current_engine(struct intel_engine_data *ed);

struct intel_execution_engine2 *
intel_get_current_physical_engine(struct intel_engine_data *ed);

void intel_next_engine(struct intel_engine_data *ed);

bool gem_engine_is_equal(const struct intel_execution_engine2 *e1,
			 const struct intel_execution_engine2 *e2);

struct intel_execution_engine2 gem_eb_flags_to_engine(unsigned int flags);

/**
 * __for_each_static_engine:
 * @e__: struct intel_execution_engine2 iterator
 *
 * Iterates over each of the statically defined (legacy) engines.
 */
#define __for_each_static_engine(e__) \
	for ((e__) = intel_execution_engines2; (e__)->name[0]; (e__)++)

/**
 * for_each_ctx_cfg_engine
 * @fd__: open i915 drm file descriptor
 * @ctx_cfg__: Intel context config
 * @e__: struct intel_execution_engine2 iterator
 *
 * Iterates over each physical engine in the context config
 */
#define for_each_ctx_cfg_engine(fd__, ctx_cfg__, e__) \
	for (struct intel_engine_data i__##e__ = \
			intel_engine_list_for_ctx_cfg(fd__, ctx_cfg__); \
	     ((e__) = intel_get_current_engine(&i__##e__)); \
	     intel_next_engine(&i__##e__))

/**
 * for_each_ctx_engine
 * @fd__: open i915 drm file descriptor
 * @ctx__: Intel context wrapper
 * @e__: struct intel_execution_engine2 iterator
 *
 * Iterates over each physical engine in the context
 */
#define for_each_ctx_engine(fd__, ctx__, e__) \
	for_each_ctx_cfg_engine(fd__, &(ctx__)->cfg, e__)

/**
 * for_each_physical_engine
 * @fd__: open i915 drm file descriptor
 * @e__: struct intel_execution_engine2 iterator
 *
 * Iterates over each physical engine in device.  Be careful when using
 * this iterator as your context may not have all of these engines and the
 * intel_execution_engine2::flags field in the iterator may not match your
 * context configuration.
 */
#define for_each_physical_engine(fd__, e__) \
	for (struct intel_engine_data i__##e__ = intel_engine_list_of_physical(fd__); \
	     ((e__) = intel_get_current_physical_engine(&i__##e__)); \
	     intel_next_engine(&i__##e__))

__attribute__((format(scanf, 4, 5)))
int gem_engine_property_scanf(int i915, const char *engine, const char *attr,
			      const char *fmt, ...);
__attribute__((format(printf, 4, 5)))
int gem_engine_property_printf(int i915, const char *engine, const char *attr,
			       const char *fmt, ...);

uint32_t gem_engine_mmio_base(int i915, const char *engine);

void dyn_sysfs_engines(int i915, int engines, const char *file,
		       void (*test)(int i915, int engine));

#endif /* GEM_ENGINE_TOPOLOGY_H */
