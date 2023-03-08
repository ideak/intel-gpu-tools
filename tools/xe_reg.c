// SPDX-License-Identifier: MIT
/*
 * Copyright © 2021 Intel Corporation
 */

#include "igt.h"
#include "igt_device_scan.h"

#include "xe_drm.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define DECL_XE_MMIO_READ_FN(bits) \
static inline uint##bits##_t \
xe_mmio_read##bits(int fd, uint32_t reg) \
{ \
	struct drm_xe_mmio mmio = { \
		.addr = reg, \
		.flags = DRM_XE_MMIO_READ | DRM_XE_MMIO_##bits##BIT, \
	}; \
\
	igt_assert_eq(igt_ioctl(fd, DRM_IOCTL_XE_MMIO, &mmio), 0); \
\
	return mmio.value;\
}\
static inline void \
xe_mmio_write##bits(int fd, uint32_t reg, uint##bits##_t value) \
{ \
	struct drm_xe_mmio mmio = { \
		.addr = reg, \
		.flags = DRM_XE_MMIO_WRITE | DRM_XE_MMIO_##bits##BIT, \
		.value = value, \
	}; \
\
	igt_assert_eq(igt_ioctl(fd, DRM_IOCTL_XE_MMIO, &mmio), 0); \
}

DECL_XE_MMIO_READ_FN(8)
DECL_XE_MMIO_READ_FN(16)
DECL_XE_MMIO_READ_FN(32)
DECL_XE_MMIO_READ_FN(64)

static void print_help(FILE *fp)
{
	fprintf(fp, "usage: xe_reg read REG1 [REG2]...\n");
	fprintf(fp, "       xe_reg write REG VALUE\n");
}

enum ring {
	RING_UNKNOWN = -1,
	RING_RCS0,
	RING_BCS0,
};

static const struct ring_info {
	enum ring ring;
	const char *name;
	uint32_t mmio_base;
} ring_info[] = {
	{RING_RCS0, "rcs0", 0x02000, },
	{RING_BCS0, "bcs0", 0x22000, },
};

static const struct ring_info *ring_info_for_name(const char *name)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(ring_info); i++)
		if (strcmp(name, ring_info[i].name) == 0)
			return &ring_info[i];

	return NULL;
}

struct reg_info {
	const char *name;
	bool is_ring;
	uint32_t addr_low;
	uint32_t addr_high;
} reg_info[] = {
#define REG32(name, addr) { #name, false, addr }
#define REG64(name, low, high) { #name, false, low, high }
#define RING_REG32(name, addr) { #name, true, addr }
#define RING_REG64(name, low, high) { #name, true, low, high }

	RING_REG64(ACTHD, 0x74, 0x5c),
	RING_REG32(BB_ADDR_DIFF, 0x154),
	RING_REG64(BB_ADDR, 0x140, 0x168),
	RING_REG32(BB_PER_CTX_PTR, 0x2c0),
	RING_REG64(EXECLIST_STATUS, 0x234, 0x238),
	RING_REG64(EXECLIST_SQ0, 0x510, 0x514),
	RING_REG64(EXECLIST_SQ1, 0x518, 0x51c),
	RING_REG32(HWS_PGA, 0x80),
	RING_REG32(INDIRECT_CTX, 0x1C4),
	RING_REG32(INDIRECT_CTX_OFFSET, 0x1C8),
	RING_REG32(NOPID, 0x94),
	RING_REG64(PML4E, 0x270, 0x274),
	RING_REG32(RING_BUFFER_CTL, 0x3c),
	RING_REG32(RING_BUFFER_HEAD, 0x34),
	RING_REG32(RING_BUFFER_START, 0x38),
	RING_REG32(RING_BUFFER_TAIL, 0x30),
	RING_REG64(SBB_ADDR, 0x114, 0x11c),
	RING_REG32(SBB_STATE, 0x118),

#undef REG32
#undef REG64
#undef RING_REG32
#undef RING_REG64
};

static const struct reg_info *reg_info_for_name(const char *name)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(reg_info); i++)
		if (strcmp(name, reg_info[i].name) == 0)
			return &reg_info[i];

	return NULL;
}

static int print_reg_for_info(int xe, FILE *fp, const struct reg_info *reg,
			      const struct ring_info *ring)
{
	if (reg->is_ring) {
		if (!ring) {
			fprintf(stderr, "%s is a ring register but --ring "
					"not set\n", reg->name);
			return EXIT_FAILURE;
		}

		if (reg->addr_high) {
			uint32_t low = xe_mmio_read32(xe, reg->addr_low +
							  ring->mmio_base);
			uint32_t high = xe_mmio_read32(xe, reg->addr_high +
							   ring->mmio_base);

			fprintf(fp, "%s[%s] = 0x%08x %08x\n", reg->name,
				ring->name, high, low);
		} else {
			uint32_t value = xe_mmio_read32(xe, reg->addr_low +
							    ring->mmio_base);

			fprintf(fp, "%s[%s] = 0x%08x\n", reg->name,
				ring->name, value);
		}
	} else {
		if (reg->addr_high) {
			uint32_t low = xe_mmio_read32(xe, reg->addr_low);
			uint32_t high = xe_mmio_read32(xe, reg->addr_high);

			fprintf(fp, "%s = 0x%08x %08x\n", reg->name, high, low);
		} else {
			uint32_t value = xe_mmio_read32(xe, reg->addr_low);

			fprintf(fp, "%s = 0x%08x\n", reg->name, value);
		}
	}

	return 0;
}

static void print_reg_for_addr(int xe, FILE *fp, uint32_t addr)
{
	uint32_t value = xe_mmio_read32(xe, addr);

	fprintf(fp, "MMIO[0x%05x] = 0x%08x\n", addr, value);
}

enum opt {
	OPT_UNKNOWN = '?',
	OPT_END = -1,
	OPT_DEVICE,
	OPT_RING,
	OPT_ALL,
};

static int read_reg(int argc, char *argv[])
{
	int xe, i, err, index;
	unsigned long reg_addr;
	char *endp = NULL;
	const struct ring_info *ring = NULL;
	enum opt opt;
	bool dump_all = false;

	static struct option options[] = {
		{ "device",	required_argument,	NULL,	OPT_DEVICE },
		{ "ring",	required_argument,	NULL,	OPT_RING },
		{ "all",	no_argument,		NULL,	OPT_ALL },
	};

	for (opt = 0; opt != OPT_END; ) {
		opt = getopt_long(argc, argv, "", options, &index);

		switch (opt) {
		case OPT_DEVICE:
			igt_device_filter_add(optarg);
			break;
		case OPT_RING:
			ring = ring_info_for_name(optarg);
			if (!ring) {
				fprintf(stderr, "invalid ring: %s\n", optarg);
				return EXIT_FAILURE;
			}
			break;
		case OPT_ALL:
			dump_all = true;
			break;
		case OPT_END:
			break;
		case OPT_UNKNOWN:
			return EXIT_FAILURE;
		}
	}

	argc -= optind;
	argv += optind;

	xe = drm_open_driver(DRIVER_XE);
	if (dump_all) {
		for (i = 0; i < ARRAY_SIZE(reg_info); i++) {
			if (reg_info[i].is_ring != !!ring)
				continue;

			print_reg_for_info(xe, stdout, &reg_info[i], ring);
		}
	} else {
		for (i = 0; i < argc; i++) {
			const struct reg_info *reg = reg_info_for_name(argv[i]);
			if (reg) {
				err = print_reg_for_info(xe, stdout, reg, ring);
				if (err)
					return err;
				continue;
			}
			reg_addr = strtoul(argv[i], &endp, 16);
			if (!reg_addr || reg_addr >= (4 << 20) || *endp) {
				fprintf(stderr, "invalid reg address '%s'\n",
					argv[i]);
				return EXIT_FAILURE;
			}
			print_reg_for_addr(xe, stdout, reg_addr);
		}
	}

	return 0;
}

static int write_reg_for_info(int xe, const struct reg_info *reg,
			      const struct ring_info *ring,
			      uint64_t value)
{
	if (reg->is_ring) {
		if (!ring) {
			fprintf(stderr, "%s is a ring register but --ring "
					"not set\n", reg->name);
			return EXIT_FAILURE;
		}

		xe_mmio_write32(xe, reg->addr_low + ring->mmio_base, value);
		if (reg->addr_high) {
			xe_mmio_write32(xe, reg->addr_high + ring->mmio_base,
					value >> 32);
		}
	} else {
		xe_mmio_write32(xe, reg->addr_low, value);
		if (reg->addr_high)
			xe_mmio_write32(xe, reg->addr_high, value >> 32);
	}

	return 0;
}

static void write_reg_for_addr(int xe, uint32_t addr, uint32_t value)
{
	xe_mmio_write32(xe, addr, value);
}

static int write_reg(int argc, char *argv[])
{
	int xe, index;
	unsigned long reg_addr;
	char *endp = NULL;
	const struct ring_info *ring = NULL;
	enum opt opt;
	const char *reg_name;
	const struct reg_info *reg;
	uint64_t value;

	static struct option options[] = {
		{ "device",	required_argument,	NULL,	OPT_DEVICE },
		{ "ring",	required_argument,	NULL,	OPT_RING },
	};

	for (opt = 0; opt != OPT_END; ) {
		opt = getopt_long(argc, argv, "", options, &index);

		switch (opt) {
		case OPT_DEVICE:
			igt_device_filter_add(optarg);
			break;
		case OPT_RING:
			ring = ring_info_for_name(optarg);
			if (!ring) {
				fprintf(stderr, "invalid ring: %s\n", optarg);
				return EXIT_FAILURE;
			}
			break;
		case OPT_END:
			break;
		case OPT_UNKNOWN:
			return EXIT_FAILURE;
		default:
			break;
		}
	}

	argc -= optind;
	argv += optind;

	if (argc != 2) {
		print_help(stderr);
		return EXIT_FAILURE;
	}

	reg_name = argv[0];
	value = strtoull(argv[1], &endp, 0);
	if (*endp) {
		fprintf(stderr, "Invalid register value: %s\n", argv[1]);
		return EXIT_FAILURE;
	}

	xe = drm_open_driver(DRIVER_XE);

	reg = reg_info_for_name(reg_name);
	if (reg)
		return write_reg_for_info(xe, reg, ring, value);

	reg_addr = strtoul(reg_name, &endp, 16);
	if (!reg_addr || reg_addr >= (4 << 20) || *endp) {
		fprintf(stderr, "invalid reg address '%s'\n", reg_name);
		return EXIT_FAILURE;
	}
	write_reg_for_addr(xe, reg_addr, value);

	return 0;
}

int main(int argc, char *argv[])
{
	if (argc < 2) {
		print_help(stderr);
		return EXIT_FAILURE;
	}

	if (strcmp(argv[1], "read") == 0)
		return read_reg(argc - 1, argv + 1);
	else if (strcmp(argv[1], "write") == 0)
		return write_reg(argc - 1, argv + 1);

	fprintf(stderr, "invalid sub-command: %s", argv[1]);
	return EXIT_FAILURE;
}
