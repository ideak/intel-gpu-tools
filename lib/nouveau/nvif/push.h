/*
 * Copyright 2021 Red Hat Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef _IGT_NOUVEAU_PUSHBUF_H_
#define _IGT_NOUVEAU_PUSHBUF_H_

#include <string.h>
#include <inttypes.h>

#include <nouveau/nouveau.h>

#include "igt_core.h"
#include "igt_aux.h"
#include "drmtest.h"

#include "nouveau/nvhw/drf.h"

/* Pushbuffer management functions copied from mesa */
static inline uint32_t
PUSH_AVAIL(struct nouveau_pushbuf *push)
{
	return push->end - push->cur;
}

static inline void
PUSH_SPACE(struct nouveau_pushbuf *push, uint32_t size)
{
	/* Provide a buffer so that fences always have room to be emitted */
	size += 2;

	size *= 4;
	if (PUSH_AVAIL(push) < size) {
		do_or_die(nouveau_pushbuf_space(push, size, 0, 0));

		/* Stash the beginning of the pushbuffer as the user_priv data, so we can keep track
		 * of it to properly print the current offset of the pushbuffer
		 */
		push->user_priv = push->cur;
	}
}

static inline void
PUSH_KICK(struct nouveau_pushbuf *push)
{
	do_or_die(nouveau_pushbuf_kick(push, push->channel));
}

static inline void
PUSH_REFN(struct nouveau_pushbuf *push, struct nouveau_bo *bo, uint32_t flags)
{
	struct nouveau_pushbuf_refn ref = { bo, flags };

	do_or_die(nouveau_pushbuf_refn(push, &ref, 1));
}

#define PUSH_PRINTF(push, fmt, args...) do {       \
	igt_debug("%08tx: "fmt"\n", (ptrdiff_t)((void*)push->cur - push->user_priv), ##args); \
} while (0)

#define PUSH_ASSERT_ON(a, b) igt_assert_f((a), "Pushbuffer assertion failed: %s (%s)\n", #a, b)
#define PUSH_ASSERT(a, b) do {                                            \
	static_assert(                                                    \
		__builtin_choose_expr(__builtin_constant_p(a), (a), 1), b \
	);                                                                \
	PUSH_ASSERT_ON((a), b);                                           \
} while (0)

#define PUSH_DATA__(push, data, fmt, args...) do {                      \
	struct nouveau_pushbuf *_push = (push);                         \
	uint32_t _data = (data);                                        \
                                                                        \
	PUSH_ASSERT((_push->cur + 1) <= _push->end, "pushbuf overrun"); \
	PUSH_PRINTF(_push, "%08x"fmt, _data, ##args);                   \
	*_push->cur++ = _data;                                          \
} while(0)

#define PUSH_DATA_(X,p,m,i0,i1,d,s,f,a...) PUSH_DATA__((p), (d), "-> "#m f, ##a)
#define PUSH_DATA(p,d) PUSH_DATA__((p), (d), " data - %s", __func__)

#define PUSH_DATAp(X,p,m,i,o,d,s,f,a...) do {                                     \
	struct nouveau_pushbuf *_pp = (p);                                        \
	const uint32_t *_dd = (d);                                                \
	uint32_t _s = (s), _i = (i?PUSH_##o##_INC);                               \
	if (_s--) {                                                               \
		PUSH_DATA_(X, _pp, X##m, i0, i1, *_dd++, 1, "+0x%x", 0);          \
		while (_s--) {                                                    \
			PUSH_DATA_(X, _pp, X##m, i0, i1, *_dd++, 1, "+0x%x", _i); \
			_i += (0?PUSH_##o##_INC);                                 \
		}                                                                 \
	}                                                                         \
} while(0)

#define PUSH_1(X,f,ds,n,c,o,p,s,mA,dA) do {                            \
	PUSH_##o##_HDR((p), s, mA, (c)+(n));                           \
	PUSH_##f(X, (p), X##mA, 1, o, (dA), ds, "");                   \
} while(0)
#define PUSH_2(X,f,ds,n,c,o,p,s,mB,dB,mA,dA,a...) do {                 \
	PUSH_ASSERT((mB) - (mA) == (1?PUSH_##o##_INC), "mthd1");       \
	PUSH_1(X, DATA_, 1, ds, (c)+(n), o, (p), s, X##mA, (dA), ##a); \
	PUSH_##f(X, (p), X##mB, 0, o, (dB), ds, "");                   \
} while(0)
#define PUSH_3(X,f,ds,n,c,o,p,s,mB,dB,mA,dA,a...) do {                 \
	PUSH_ASSERT((mB) - (mA) == (0?PUSH_##o##_INC), "mthd2");       \
	PUSH_2(X, DATA_, 1, ds, (c)+(n), o, (p), s, X##mA, (dA), ##a); \
	PUSH_##f(X, (p), X##mB, 0, o, (dB), ds, "");                   \
} while(0)
#define PUSH_4(X,f,ds,n,c,o,p,s,mB,dB,mA,dA,a...) do {                 \
	PUSH_ASSERT((mB) - (mA) == (0?PUSH_##o##_INC), "mthd3");       \
	PUSH_3(X, DATA_, 1, ds, (c)+(n), o, (p), s, X##mA, (dA), ##a); \
	PUSH_##f(X, (p), X##mB, 0, o, (dB), ds, "");                   \
} while(0)
#define PUSH_5(X,f,ds,n,c,o,p,s,mB,dB,mA,dA,a...) do {                 \
	PUSH_ASSERT((mB) - (mA) == (0?PUSH_##o##_INC), "mthd4");       \
	PUSH_4(X, DATA_, 1, ds, (c)+(n), o, (p), s, X##mA, (dA), ##a); \
	PUSH_##f(X, (p), X##mB, 0, o, (dB), ds, "");                   \
} while(0)
#define PUSH_6(X,f,ds,n,c,o,p,s,mB,dB,mA,dA,a...) do {                 \
	PUSH_ASSERT((mB) - (mA) == (0?PUSH_##o##_INC), "mthd5");       \
	PUSH_5(X, DATA_, 1, ds, (c)+(n), o, (p), s, X##mA, (dA), ##a); \
	PUSH_##f(X, (p), X##mB, 0, o, (dB), ds, "");                   \
} while(0)
#define PUSH_7(X,f,ds,n,c,o,p,s,mB,dB,mA,dA,a...) do {                 \
	PUSH_ASSERT((mB) - (mA) == (0?PUSH_##o##_INC), "mthd6");       \
	PUSH_6(X, DATA_, 1, ds, (c)+(n), o, (p), s, X##mA, (dA), ##a); \
	PUSH_##f(X, (p), X##mB, 0, o, (dB), ds, "");                   \
} while(0)
#define PUSH_8(X,f,ds,n,c,o,p,s,mB,dB,mA,dA,a...) do {                 \
	PUSH_ASSERT((mB) - (mA) == (0?PUSH_##o##_INC), "mthd7");       \
	PUSH_7(X, DATA_, 1, ds, (c)+(n), o, (p), s, X##mA, (dA), ##a); \
	PUSH_##f(X, (p), X##mB, 0, o, (dB), ds, "");                   \
} while(0)
#define PUSH_9(X,f,ds,n,c,o,p,s,mB,dB,mA,dA,a...) do {                 \
	PUSH_ASSERT((mB) - (mA) == (0?PUSH_##o##_INC), "mthd8");       \
	PUSH_8(X, DATA_, 1, ds, (c)+(n), o, (p), s, X##mA, (dA), ##a); \
	PUSH_##f(X, (p), X##mB, 0, o, (dB), ds, "");                   \
} while(0)
#define PUSH_10(X,f,ds,n,c,o,p,s,mB,dB,mA,dA,a...) do {                \
	PUSH_ASSERT((mB) - (mA) == (0?PUSH_##o##_INC), "mthd9");       \
	PUSH_9(X, DATA_, 1, ds, (c)+(n), o, (p), s, X##mA, (dA), ##a); \
	PUSH_##f(X, (p), X##mB, 0, o, (dB), ds, "");                   \
} while(0)

#define PUSH_1D(X,o,p,s,mA,dA)                            \
	PUSH_1(X, DATA_, 1, 1, 0, o, (p), s, X##mA, (dA))
#define PUSH_2D(X,o,p,s,mA,dA,mB,dB)                      \
	PUSH_2(X, DATA_, 1, 1, 0, o, (p), s, X##mB, (dB), \
					     X##mA, (dA))
#define PUSH_3D(X,o,p,s,mA,dA,mB,dB,mC,dC)                \
	PUSH_3(X, DATA_, 1, 1, 0, o, (p), s, X##mC, (dC), \
					     X##mB, (dB), \
					     X##mA, (dA))
#define PUSH_4D(X,o,p,s,mA,dA,mB,dB,mC,dC,mD,dD)          \
	PUSH_4(X, DATA_, 1, 1, 0, o, (p), s, X##mD, (dD), \
					     X##mC, (dC), \
					     X##mB, (dB), \
					     X##mA, (dA))
#define PUSH_5D(X,o,p,s,mA,dA,mB,dB,mC,dC,mD,dD,mE,dE)    \
	PUSH_5(X, DATA_, 1, 1, 0, o, (p), s, X##mE, (dE), \
					     X##mD, (dD), \
					     X##mC, (dC), \
					     X##mB, (dB), \
					     X##mA, (dA))
#define PUSH_6D(X,o,p,s,mA,dA,mB,dB,mC,dC,mD,dD,mE,dE,mF,dF) \
	PUSH_6(X, DATA_, 1, 1, 0, o, (p), s, X##mF, (dF),    \
					     X##mE, (dE),    \
					     X##mD, (dD),    \
					     X##mC, (dC),    \
					     X##mB, (dB),    \
					     X##mA, (dA))
#define PUSH_7D(X,o,p,s,mA,dA,mB,dB,mC,dC,mD,dD,mE,dE,mF,dF,mG,dG) \
	PUSH_7(X, DATA_, 1, 1, 0, o, (p), s, X##mG, (dG),          \
					     X##mF, (dF),          \
					     X##mE, (dE),          \
					     X##mD, (dD),          \
					     X##mC, (dC),          \
					     X##mB, (dB),          \
					     X##mA, (dA))
#define PUSH_8D(X,o,p,s,mA,dA,mB,dB,mC,dC,mD,dD,mE,dE,mF,dF,mG,dG,mH,dH) \
	PUSH_8(X, DATA_, 1, 1, 0, o, (p), s, X##mH, (dH),                \
					     X##mG, (dG),                \
					     X##mF, (dF),                \
					     X##mE, (dE),                \
					     X##mD, (dD),                \
					     X##mC, (dC),                \
					     X##mB, (dB),                \
					     X##mA, (dA))
#define PUSH_9D(X,o,p,s,mA,dA,mB,dB,mC,dC,mD,dD,mE,dE,mF,dF,mG,dG,mH,dH,mI,dI) \
	PUSH_9(X, DATA_, 1, 1, 0, o, (p), s, X##mI, (dI),                      \
					     X##mH, (dH),                      \
					     X##mG, (dG),                      \
					     X##mF, (dF),                      \
					     X##mE, (dE),                      \
					     X##mD, (dD),                      \
					     X##mC, (dC),                      \
					     X##mB, (dB),                      \
					     X##mA, (dA))
#define PUSH_10D(X,o,p,s,mA,dA,mB,dB,mC,dC,mD,dD,mE,dE,mF,dF,mG,dG,mH,dH,mI,dI,mJ,dJ) \
	PUSH_10(X, DATA_, 1, 1, 0, o, (p), s, X##mJ, (dJ),                            \
					      X##mI, (dI),                            \
					      X##mH, (dH),                            \
					      X##mG, (dG),                            \
					      X##mF, (dF),                            \
					      X##mE, (dE),                            \
					      X##mD, (dD),                            \
					      X##mC, (dC),                            \
					      X##mB, (dB),                            \
					      X##mA, (dA))

#define PUSH_1P(X,o,p,s,mA,dp,ds)                           \
	PUSH_1(X, DATAp, ds, ds, 0, o, (p), s, X##mA, (dp))
#define PUSH_2P(X,o,p,s,mA,dA,mB,dp,ds)                     \
	PUSH_2(X, DATAp, ds, ds, 0, o, (p), s, X##mB, (dp), \
					       X##mA, (dA))
#define PUSH_3P(X,o,p,s,mA,dA,mB,dB,mC,dp,ds)               \
	PUSH_3(X, DATAp, ds, ds, 0, o, (p), s, X##mC, (dp), \
					       X##mB, (dB), \
					       X##mA, (dA))

#define PUSH_(A,B,C,D,E,F,G,H,I,J,K,L,M,N,O,P,Q,R,S,T,U,V,W,X,IMPL,...) IMPL
#define PUSH(A...) PUSH_(A, PUSH_10P, PUSH_10D,          \
			    PUSH_9P , PUSH_9D,           \
			    PUSH_8P , PUSH_8D,           \
			    PUSH_7P , PUSH_7D,           \
			    PUSH_6P , PUSH_6D,           \
			    PUSH_5P , PUSH_5D,           \
			    PUSH_4P , PUSH_4D,           \
			    PUSH_3P , PUSH_3D,           \
			    PUSH_2P , PUSH_2D,           \
			    PUSH_1P , PUSH_1D)(, ##A)

#define PUSH_NVIM(p,c,m,d) do {             \
	struct nouveau_pushbuf *__p = (p);        \
	uint32_t __d = (d);                      \
	PUSH_IMMD_HDR(__p, c, m, __d);      \
	__p->cur--;                         \
	PUSH_PRINTF(__p, "%08x-> "#m, __d); \
	__p->cur++;                         \
} while(0)
#define PUSH_NVSQ(A...) PUSH(MTHD, ##A)
#define PUSH_NV1I(A...) PUSH(1INC, ##A)
#define PUSH_NVNI(A...) PUSH(NINC, ##A)

#define PUSH_NV_1(X,o,p,c,mA,d...) \
       PUSH_##o(p,c,c##_##mA,d)
#define PUSH_NV_2(X,o,p,c,mA,dA,mB,d...) \
       PUSH_##o(p,c,c##_##mA,dA,         \
		    c##_##mB,d)
#define PUSH_NV_3(X,o,p,c,mA,dA,mB,dB,mC,d...) \
       PUSH_##o(p,c,c##_##mA,dA,               \
		    c##_##mB,dB,               \
		    c##_##mC,d)
#define PUSH_NV_4(X,o,p,c,mA,dA,mB,dB,mC,dC,mD,d...) \
       PUSH_##o(p,c,c##_##mA,dA,                     \
		    c##_##mB,dB,                     \
		    c##_##mC,dC,                     \
		    c##_##mD,d)
#define PUSH_NV_5(X,o,p,c,mA,dA,mB,dB,mC,dC,mD,dD,mE,d...) \
       PUSH_##o(p,c,c##_##mA,dA,                           \
		    c##_##mB,dB,                           \
		    c##_##mC,dC,                           \
		    c##_##mD,dD,                           \
		    c##_##mE,d)
#define PUSH_NV_6(X,o,p,c,mA,dA,mB,dB,mC,dC,mD,dD,mE,dE,mF,d...) \
       PUSH_##o(p,c,c##_##mA,dA,                                 \
		    c##_##mB,dB,                                 \
		    c##_##mC,dC,                                 \
		    c##_##mD,dD,                                 \
		    c##_##mE,dE,                                 \
		    c##_##mF,d)
#define PUSH_NV_7(X,o,p,c,mA,dA,mB,dB,mC,dC,mD,dD,mE,dE,mF,dF,mG,d...) \
       PUSH_##o(p,c,c##_##mA,dA,                                       \
		    c##_##mB,dB,                                       \
		    c##_##mC,dC,                                       \
		    c##_##mD,dD,                                       \
		    c##_##mE,dE,                                       \
		    c##_##mF,dF,                                       \
		    c##_##mG,d)
#define PUSH_NV_8(X,o,p,c,mA,dA,mB,dB,mC,dC,mD,dD,mE,dE,mF,dF,mG,dG,mH,d...) \
       PUSH_##o(p,c,c##_##mA,dA,                                             \
		    c##_##mB,dB,                                             \
		    c##_##mC,dC,                                             \
		    c##_##mD,dD,                                             \
		    c##_##mE,dE,                                             \
		    c##_##mF,dF,                                             \
		    c##_##mG,dG,                                             \
		    c##_##mH,d)
#define PUSH_NV_9(X,o,p,c,mA,dA,mB,dB,mC,dC,mD,dD,mE,dE,mF,dF,mG,dG,mH,dH,mI,d...) \
       PUSH_##o(p,c,c##_##mA,dA,                                                   \
		    c##_##mB,dB,                                                   \
		    c##_##mC,dC,                                                   \
		    c##_##mD,dD,                                                   \
		    c##_##mE,dE,                                                   \
		    c##_##mF,dF,                                                   \
		    c##_##mG,dG,                                                   \
		    c##_##mH,dH,                                                   \
		    c##_##mI,d)
#define PUSH_NV_10(X,o,p,c,mA,dA,mB,dB,mC,dC,mD,dD,mE,dE,mF,dF,mG,dG,mH,dH,mI,dI,mJ,d...) \
       PUSH_##o(p,c,c##_##mA,dA,                                                          \
		    c##_##mB,dB,                                                          \
		    c##_##mC,dC,                                                          \
		    c##_##mD,dD,                                                          \
		    c##_##mE,dE,                                                          \
		    c##_##mF,dF,                                                          \
		    c##_##mG,dG,                                                          \
		    c##_##mH,dH,                                                          \
		    c##_##mI,dI,                                                          \
		    c##_##mJ,d)

#define PUSH_NV_(A,B,C,D,E,F,G,H,I,J,K,L,M,N,O,P,Q,R,S,T,U,V,W,X,IMPL,...) IMPL
#define PUSH_NV(A...) PUSH_NV_(A, PUSH_NV_10, PUSH_NV_10,       \
				  PUSH_NV_9 , PUSH_NV_9,        \
				  PUSH_NV_8 , PUSH_NV_8,        \
				  PUSH_NV_7 , PUSH_NV_7,        \
				  PUSH_NV_6 , PUSH_NV_6,        \
				  PUSH_NV_5 , PUSH_NV_5,        \
				  PUSH_NV_4 , PUSH_NV_4,        \
				  PUSH_NV_3 , PUSH_NV_3,        \
				  PUSH_NV_2 , PUSH_NV_2,        \
				  PUSH_NV_1 , PUSH_NV_1)(, ##A)

#define PUSH_IMMD(A...) PUSH_NV(NVIM, ##A)
#define PUSH_MTHD(A...) PUSH_NV(NVSQ, ##A)
#define PUSH_1INC(A...) PUSH_NV(NV1I, ##A)
#define PUSH_NINC(A...) PUSH_NV(NVNI, ##A)

#endif /* _IGT_NOUVEAU_PUSHBUF_H_ */
