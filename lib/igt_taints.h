/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2021 Intel Corporation
 */

#ifndef __IGT_TAINTS_H__
#define __IGT_TAINTS_H__

unsigned long igt_kernel_tainted(unsigned long *taints);
const char *igt_explain_taints(unsigned long *taints);

unsigned long igt_bad_taints(void);

static inline unsigned long is_tainted(unsigned long taints)
{
	return taints & igt_bad_taints();
}

#endif /* __IGT_TAINTS_H__ */
