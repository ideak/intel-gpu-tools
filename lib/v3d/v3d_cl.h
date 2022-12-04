/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2019 Raspberry Pi Ltd
 */

#ifndef V3D_CL_H
#define V3D_CL_H

#include "v3d_packet_helpers.h"
#include "igt_v3d.h"

/**
 * Undefined structure, used for typechecking that you're passing the pointers
 * to these functions correctly.
 */
struct v3d_cl_out;

/** A reference to a BO used in the CL packing functions */
struct v3d_cl_reloc {
	struct v3d_bo *bo;
	uint32_t offset;
};

#define __gen_user_data struct v3d_cl
#define __gen_address_type struct v3d_cl_reloc
#define __gen_address_offset(reloc) (((reloc)->bo ? (reloc)->bo->offset : 0) + \
				     (reloc)->offset)
#define __gen_emit_reloc cl_pack_emit_reloc
#define __gen_unpack_address(cl, s, e) __unpack_address(cl, s, e)

struct v3d_cl {
	void *base;
	struct v3d_cl_out *next;
	struct v3d_bo *bo;
	uint32_t size;
};

static inline struct v3d_cl_reloc
__unpack_address(const uint8_t *cl, uint32_t s, uint32_t e)
{
	struct v3d_cl_reloc reloc = {
		NULL, __gen_unpack_uint(cl, s, e) << (31 - (e - s)) };
	return reloc;
}

static inline uint32_t
v3d_cl_offset(struct v3d_cl *cl)
{
	return (char *)cl->next - (char *)cl->base;
}

static inline struct v3d_cl_reloc
v3d_cl_address(struct v3d_bo *bo, uint32_t offset)
{
	struct v3d_cl_reloc reloc = {
		.bo = bo,
		.offset = offset,
	};
	return reloc;
}

static inline struct v3d_cl_reloc
v3d_cl_get_address(struct v3d_cl *cl)
{
	return (struct v3d_cl_reloc){ .bo = cl->bo, .offset = v3d_cl_offset(cl) };
}

static inline struct v3d_cl_out *
cl_start(struct v3d_cl *cl)
{
	return cl->next;
}

static inline void
cl_end(struct v3d_cl *cl, struct v3d_cl_out *next)
{
	cl->next = next;
	assert(v3d_cl_offset(cl) <= cl->size);
}

static inline void
cl_advance(struct v3d_cl_out **cl, uint32_t n)
{
	(*cl) = (struct v3d_cl_out *)((char *)(*cl) + n);
}

#define V3DX(x) V3D42_##x
#define v3dX(x) V3D42_##x

#define cl_packet_header(packet) V3DX(packet ## _header)
#define cl_packet_length(packet) V3DX(packet ## _length)
#define cl_packet_pack(packet)   V3DX(packet ## _pack)
#define cl_packet_struct(packet) V3DX(packet)

/* Macro for setting up an emit of a CL struct.  A temporary unpacked struct
 * is created, which you get to set fields in of the form:
 *
 * cl_emit(bcl, FLAT_SHADE_FLAGS, flags) {
 *     .flags.flat_shade_flags = 1 << 2,
 * }
 *
 * or default values only can be emitted with just:
 *
 * cl_emit(bcl, FLAT_SHADE_FLAGS, flags);
 *
 * The trick here is that we make a for loop that will execute the body
 * (either the block or the ';' after the macro invocation) exactly once.
 */
#define cl_emit(cl, packet, name)					\
	for (struct cl_packet_struct(packet) name = {			\
		cl_packet_header(packet)				\
	},								\
	*_loop_terminate = &name;					\
	__builtin_expect(_loop_terminate != NULL, 1);			\
	({								\
		struct v3d_cl_out *cl_out = cl_start(cl);		\
		cl_packet_pack(packet)(cl, (uint8_t *)cl_out, &name);	\
		cl_advance(&cl_out, cl_packet_length(packet));		\
		cl_end(cl, cl_out);					\
		_loop_terminate = NULL;					\
	}))								\

/*
 * Helper function called by the XML-generated pack functions for filling in
 * an address field in shader records.
 *
 * Since we have a private address space as of V3D, our BOs can have lifelong
 * offsets, and all the kernel needs to know is which BOs need to be paged in
 * for this exec.
 */
static inline void
cl_pack_emit_reloc(struct v3d_cl *cl, const struct v3d_cl_reloc *reloc)
{
	/*
	 * Mock emit reloc, as it is not needed for IGT tests.
	 */
}

#endif /* V3D_CL_H */
