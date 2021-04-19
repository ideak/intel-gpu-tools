/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2021 Intel Corporation
 */

#ifndef INTEL_CTX_H
#define INTEL_CTX_H

#include "igt_core.h"

#include "i915_drm.h"

#define GEM_MAX_ENGINES		I915_EXEC_RING_MASK + 1

/**
 * intel_ctx_cfg_t:
 * @flags: Context create flags
 * @vm: VM to inherit or 0 for using a per-context VM
 * @nopersist: set I915_CONTEXT_PARAM_PERSISTENCE to 0
 * @load_balance: True if the first engine should be a load balancing engine
 * @num_engines: Number of client-specified engines or 0 for legacy mode
 * @engines: Client-specified engines
 *
 * Represents the full configuration of an intel_ctx.
 *
 * @num_engines not only specifies the number of engines in the context but
 * also how engine information should be communicated to execbuf.  With the
 * engines API, every context has two modes:
 *
 *   - In legacy mode (indicated by @num_engines == 0), the context has a
 *     fixed set of engines.  The engine to use is specified to execbuf via
 *     an I915_EXEC_* flag such as I915_EXEC_RENDER or I915_EXEC_BLT.  This
 *     is the default behavior of a GEM context if CONTEXT_PARAM_ENGINES is
 *     never set.
 *
 *   - In modern mode (indicated by @num_engines > 0), the set of engines
 *     is provided by userspace via CONTEXT_PARAM_ENGINES.  Userspace
 *     provides an array of i915_engine_class_instance which are class +
 *     instance pairs.  When calling execbuf in this mode, the engine to
 *     use is specified by passing an integer engine index into that array
 *     of engines as part of the flags parameter.  (Because of the layout
 *     of the flags, the maximum possible index value is 63.)
 */
typedef struct intel_ctx_cfg {
	uint32_t flags;
	uint32_t vm;
	bool nopersist;
	bool load_balance;
	unsigned int num_engines;
	struct i915_engine_class_instance engines[GEM_MAX_ENGINES];
} intel_ctx_cfg_t;

intel_ctx_cfg_t intel_ctx_cfg_for_engine(unsigned int class, unsigned int inst);
intel_ctx_cfg_t intel_ctx_cfg_all_physical(int fd);

/**
 * intel_ctx_t:
 * @id: the context id/handle
 * @cfg: the config used to create this context
 *
 * Represents the full configuration of an intel_ctx.
 */
typedef struct intel_ctx {
	uint32_t id;
	intel_ctx_cfg_t cfg;
} intel_ctx_t;

int __intel_ctx_create(int fd, const intel_ctx_cfg_t *cfg,
		       const intel_ctx_t **out_ctx);
const intel_ctx_t *intel_ctx_create(int i915, const intel_ctx_cfg_t *cfg);
const intel_ctx_t *intel_ctx_0(int fd);
const intel_ctx_t *intel_ctx_create_for_engine(int fd, unsigned int class,
					       unsigned int inst);
const intel_ctx_t *intel_ctx_create_all_physical(int fd);
void intel_ctx_destroy(int fd, const intel_ctx_t *ctx);

unsigned int intel_ctx_engine_class(const intel_ctx_t *ctx, unsigned int engine);

#endif
