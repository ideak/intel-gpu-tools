/* SPDX-License-Identifier: MIT */
/*
 * Copyright (C) 2016 Intel Corporation
 */

#ifndef UTIL_BITPACK_HELPERS_H
#define UTIL_BITPACK_HELPERS_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#ifndef util_bitpack_validate_value
#define util_bitpack_validate_value(x)
#endif

/** Set a single bit */
#define BITFIELD64_BIT(b)      (1ull << (b))
/** Set all bits up to excluding bit b */
#define BITFIELD64_MASK(b)      \
	((b) == 64 ? (~0ull) : BITFIELD64_BIT(b) - 1)

static inline uint64_t
util_bitpack_uint(uint64_t v, uint32_t start, __attribute__((unused)) uint32_t end)
{
	util_bitpack_validate_value(v);
	return v << start;
}

static inline uint64_t
util_bitpack_sint(int64_t v, uint32_t start, uint32_t end)
{
	const int bits = end - start + 1;
	const uint64_t mask = BITFIELD64_MASK(bits);

	return (v & mask) << start;
}

static inline uint64_t
util_bitpack_sfixed(float v, uint32_t start, uint32_t end,
		    uint32_t fract_bits)
{
	const float factor = (1 << fract_bits);
	const int64_t int_val = llroundf(v * factor);
	const uint64_t mask = ~0ull >> (64 - (end - start + 1));

	return (int_val & mask) << start;
}

static inline uint64_t
util_bitpack_ufixed(float v, uint32_t start, __attribute__((unused)) uint32_t end,
		    uint32_t fract_bits)
{
	const float factor = (1 << fract_bits);
	const uint64_t uint_val = llroundf(v * factor);

	return uint_val << start;
}

#endif /* UTIL_BITPACK_HELPERS_H */
