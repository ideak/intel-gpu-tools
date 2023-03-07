/* SPDX-License-Identifier: MIT */

#ifndef _LINUX_BITOPS_H_
#define _LINUX_BITOPS_H_

/*
 * Origin of this file requires some comment.
 *
 * In the i915 we use nicely collected gpu command macros in
 * intel_gpu_commands.h file and we want to reuse it here. Moreover we want
 * to copy this file verbatimly and not touch it at all. Unfortunatly this file
 * includes kernel header which doesn't have its userspace counterpart.
 *
 * We need to solve this include substituting kernel file with this one and
 * provide some scaffold macros which will solve the rest.
 */

#include "linux_scaffold.h"

#define REG_BIT(x) (1ul << (x))

#endif /* _LINUX_BITOPS_H_ */
