/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2023 Intel Corporation
 *
 * Authors:
 *    Matthew Brost <matthew.brost@intel.com>
 */

#ifndef XE_SPIN_H
#define XE_SPIN_H

#include <stdint.h>
#include <stdbool.h>

#include "xe_query.h"

/* Mapped GPU object */
struct xe_spin {
	uint32_t batch[16];
	uint64_t pad;
	uint32_t start;
	uint32_t end;
};

void xe_spin_init(struct xe_spin *spin, uint64_t addr, bool preempt);
bool xe_spin_started(struct xe_spin *spin);
void xe_spin_wait_started(struct xe_spin *spin);
void xe_spin_end(struct xe_spin *spin);

struct xe_cork {
	struct xe_spin *spin;
	int fd;
	uint32_t vm;
	uint32_t bo;
	uint32_t engine;
	uint32_t syncobj;
};

void xe_cork_init(int fd, struct drm_xe_engine_class_instance *hwe,
		  struct xe_cork *cork);
bool xe_cork_started(struct xe_cork *cork);
void xe_cork_wait_started(struct xe_cork *cork);
void xe_cork_end(struct xe_cork *cork);
void xe_cork_wait_done(struct xe_cork *cork);
void xe_cork_fini(struct xe_cork *cork);
uint32_t xe_cork_sync_handle(struct xe_cork *cork);

#endif	/* XE_SPIN_H */
