// SPDX-License-Identifier: MIT
/*
 * Copyright © 2021 Intel Corporation
 */

#include <stdio.h>

#include "igt_taints.h"

/* see Linux's include/linux/kernel.h */
static const struct {
	int bit;
	int bad;
	const char *explanation;
} abort_taints[] = {
  { 5, 1, "TAINT_BAD_PAGE: Bad page reference or an unexpected page flags." },
  { 7, 1, "TAINT_DIE: Kernel has died - BUG/OOPS." },
  { 9, 1, "TAINT_WARN: WARN_ON has happened." },
  { -1 }
};

/**
 * igt_explain_taints:
 * @taints: mask of taints requiring an explanation [inout]
 *
 * Inspects the mask and looks up the first reason corresponding to a set
 * bit in the mask. It returns the reason as a string constant, and removes
 * the bit from the mask. If the mask is empty, or we have no known reason
 * matching the mask, NULL is returned.
 *
 * This may be used in a loop to extract all known reasons for why the
 * kernel is tainted:
 *
 * while (reason = igt_explain_taints(&taints))
 * 	igt_info("%s", reason);
 *
 * Returns the first reason corresponding to a taint bit.
 */
const char *igt_explain_taints(unsigned long *taints)
{
	for (typeof(*abort_taints) *taint = abort_taints;
	     taint->bit >= 0;
	     taint++) {
		if (*taints & (1ul << taint->bit)) {
			*taints &= ~(1ul << taint->bit);
			return taint->explanation;
		}
	}

	return NULL;
}

/**
 * igt_bad_taints:
 *
 * Returns the mask of kernel taints that IGT considers fatal.
 * Such as TAINT_WARN set when the kernel oopses.
 */
unsigned long igt_bad_taints(void)
{
	static unsigned long bad_taints;

	if (!bad_taints) {
		for (typeof(*abort_taints) *taint = abort_taints;
		     taint->bit >= 0;
		     taint++) {
			if (taint->bad)
				bad_taints |= 1ul << taint->bit;
		}
	}

	return bad_taints;
}

/**
 * igt_kernel_tainted:
 * @taints: bitmask of kernel taints [out]
 *
 * Reads the bitmask of kernel taints from "/proc/sys/kernel/tainted",
 * see linux/kernel.h for the full set of flags. These are set whenever
 * the kernel encounters an exceptional condition that may impair functionality.
 * The kernel only sets the taint once, and so once a "fatal" condition has
 * been encountered, it is generally not advisable to continue testing, as at
 * least all future taint reporting will be lost.
 *
 * igt_kernel_tainted() returns the set of _all_ taints reported via @taints,
 * and also the set of _fatal_ taints as its return value.
 *
 * Returns a mask of fatal taints; 0 if untainted.
 */
unsigned long igt_kernel_tainted(unsigned long *taints)
{
	FILE *f;

	*taints = 0;

	f = fopen("/proc/sys/kernel/tainted", "r");
	if (f) {
		fscanf(f, "%lu", taints);
		fclose(f);
	}

	return is_tainted(*taints);
}
