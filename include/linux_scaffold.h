/* SPDX-License-Identifier: MIT */

#ifndef _INTEL_GPU_COMMANDS_SCAFFOLD_H_
#define _INTEL_GPU_COMMANDS_SCAFFOLD_H_

#include <stdint.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef int8_t  s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

static inline s64 sign_extend64(u64 value, int index)
{
	int shift = 63 - index;
	return (s64)(value << shift) >> shift;
}

#ifndef _AC
#  define _AC(X, Y)      X##Y
#else
#  error "_AC macro already defined"
#endif

/* Make IGT build with Kernels < 4.17 */
#ifndef _AC
#  define _AC(X, Y)	__AC(X, Y)
#endif
#ifndef _UL
#  define  _UL(x)		(_AC(x, UL))
#endif
#ifndef _ULL
#  define _ULL(x)		(_AC(x, ULL))
#endif

#define GENMASK(h, l) \
	(((~_UL(0)) - (_UL(1) << (l)) + 1) & \
	(~_UL(0) >> (BITS_PER_LONG - 1 - (h))))

#define GENMASK_ULL(h, l) \
	(((~_ULL(0)) - (_ULL(1) << (l)) + 1) & \
	(~_ULL(0) >> (BITS_PER_LONG_LONG - 1 - (h))))

#define BITS_PER_BYTE 8
#define BITS_PER_TYPE(t) (sizeof(t) * BITS_PER_BYTE)
#define BITS_PER_LONG BITS_PER_TYPE(long)
#define BITS_PER_LONG_LONG BITS_PER_TYPE(long long)

#endif /* _INTEL_GPU_COMMANDS_SCAFFOLD_H_ */
