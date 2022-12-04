/* SPDX-License-Identifier: MIT */
/*
 * Copyright (C) 2016 Intel Corporation
 */

#ifndef MESA_V3D_PACKET_HELPERS_H
#define MESA_V3D_PACKET_HELPERS_H

#include <assert.h>
#include <stdint.h>

#include "bitpack_helpers.h"

/*
 * Copied from Mesa's u_math.h
 */
union fi {
	float f;
	int32_t i;
	uint32_t ui;
};

static inline float uif(uint32_t ui)
{
	union fi fi;

	fi.ui = ui;
	return fi.f;
}

static inline unsigned int fui(float f)
{
	union fi fi;

	fi.f = f;
	return fi.ui;
}

static inline uint64_t
__gen_unpack_uint(const uint8_t *restrict cl, uint32_t start, uint32_t end)
{
	uint64_t val = 0;
	const int width = end - start + 1;
	const uint32_t mask = (width == 32 ? ~0 : (1 << width) - 1);

	for (uint32_t byte = start / 8; byte <= end / 8; byte++)
		val |= cl[byte] << ((byte - start / 8) * 8);

	return (val >> (start % 8)) & mask;
}

static inline uint64_t
__gen_unpack_sint(const uint8_t *restrict cl, uint32_t start, uint32_t end)
{
	int size = end - start + 1;
	int64_t val = __gen_unpack_uint(cl, start, end);

	/* Get the sign bit extended. */
	return (val << (64 - size)) >> (64 - size);
}

static inline float
__gen_unpack_sfixed(const uint8_t *restrict cl, uint32_t start, uint32_t end,
		    uint32_t fractional_size)
{
	int32_t bits = __gen_unpack_sint(cl, start, end);

	return (float)bits / (1 << fractional_size);
}

static inline float
__gen_unpack_ufixed(const uint8_t *restrict cl, uint32_t start, uint32_t end,
		    uint32_t fractional_size)
{
	int32_t bits = __gen_unpack_uint(cl, start, end);

	return (float)bits / (1 << fractional_size);
}

static inline float
__gen_unpack_float(const uint8_t *restrict cl, uint32_t start, uint32_t end)
{
	struct PACKED { float f; } *f;

	assert(start % 8 == 0);
	assert(end - start == 31);

	f = (void *)(cl + (start / 8));

	return f->f;
}

static inline float
__gen_unpack_f187(const uint8_t *restrict cl, uint32_t start, uint32_t end)
{
	uint32_t bits;

	assert(end - start == 15);

	bits = __gen_unpack_uint(cl, start, end);
	return uif(bits << 16);
}

#endif
