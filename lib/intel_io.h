/*
 * Copyright © 2009 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    Eric Anholt <eric@anholt.net>
 *
 */

#ifndef INTEL_GPU_TOOLS_H
#define INTEL_GPU_TOOLS_H

#include <stdint.h>
#include <pciaccess.h>
#include <stdbool.h>

extern void *igt_global_mmio;

/* register access helpers from intel_mmio.c */
struct intel_register_range {
	uint32_t base;
	uint32_t size;
	uint32_t flags;
};

struct intel_register_map {
	struct intel_register_range *map;
	uint32_t top;
	uint32_t alignment_mask;
};

struct intel_mmio_data {
	void *igt_mmio;
	struct intel_register_map map;
	uint32_t pci_device_id;
	int key;
	bool safe;
};

void intel_mmio_use_pci_bar(struct intel_mmio_data *mmio_data,
			    struct pci_device *pci_dev);
void intel_mmio_use_dump_file(struct intel_mmio_data *mmio_data, char *file);

int intel_register_access_init(struct intel_mmio_data *mmio_data,
			       struct pci_device *pci_dev, int safe, int fd);
void intel_register_access_fini(struct intel_mmio_data *mmio_data);
uint32_t intel_register_read(struct intel_mmio_data *mmio_data, uint32_t reg);
void intel_register_write(struct intel_mmio_data *mmio_data, uint32_t reg,
			  uint32_t val);
int intel_register_access_needs_fakewake(struct intel_mmio_data *mmio_data);

/* mmio register access functions that use igt_global_mmio */

#define __ioread(x__) \
static inline uint##x__##_t ioread##x__(void *mmio, uint32_t reg) \
{\
	return *(volatile uint##x__##_t *)(mmio + reg);\
}
__ioread(32)
__ioread(16)
__ioread(8)
#undef __ioread

#define __iowrite(x__) \
static inline void iowrite##x__(void *mmio, uint32_t reg, uint##x__##_t val) \
{\
	*(volatile uint##x__##_t *)(mmio + reg) = val; \
}
__iowrite(32)
__iowrite(16)
__iowrite(8)
#undef __iowrite

#define __INREG(x__,s__) \
static inline uint##x__##_t INREG##s__(uint32_t reg) \
{\
	return ioread##x__(igt_global_mmio, reg); \
}

#define __OUTREG(x__,s__) \
static inline void OUTREG##s__(uint32_t reg, uint##x__##_t val) \
{\
	iowrite##x__(igt_global_mmio, reg, val); \
}
__INREG(32,)
__INREG(16,16)
__INREG(8,8)

__OUTREG(32,)
__OUTREG(16,16)
__OUTREG(8,8)
#undef __INREG
#undef __OUTREG

/* sideband access functions from intel_iosf.c */
uint32_t intel_dpio_reg_read(struct intel_mmio_data *mmio_data, uint32_t reg,
			     int phy);
void intel_dpio_reg_write(struct intel_mmio_data *mmio_data, uint32_t reg,
			  uint32_t val, int phy);
uint32_t intel_flisdsi_reg_read(struct intel_mmio_data *mmio_data, uint32_t reg);
void intel_flisdsi_reg_write(struct intel_mmio_data *mmio_data, uint32_t reg,
			     uint32_t val);
uint32_t intel_iosf_sb_read(struct intel_mmio_data *mmio_data, uint32_t port,
			    uint32_t reg);
void intel_iosf_sb_write(struct intel_mmio_data *mmio_data, uint32_t port,
			 uint32_t reg, uint32_t val);

int intel_punit_read(struct intel_mmio_data *mmio_data, uint32_t addr,
		     uint32_t *val);
int intel_punit_write(struct intel_mmio_data *mmio_data, uint32_t addr,
		      uint32_t val);
int intel_nc_read(struct intel_mmio_data *mmio_data, uint32_t addr, uint32_t *val);
int intel_nc_write(struct intel_mmio_data *mmio_data, uint32_t addr, uint32_t val);

/* register maps from intel_reg_map.c */
#ifndef __GTK_DOC_IGNORE__

#define INTEL_RANGE_RSVD	(0<<0) /*  Shouldn't be read or written */
#define INTEL_RANGE_READ	(1<<0)
#define INTEL_RANGE_WRITE	(1<<1)
#define INTEL_RANGE_RW		(INTEL_RANGE_READ | INTEL_RANGE_WRITE)
#define INTEL_RANGE_END		(1<<31)

struct intel_register_map intel_get_register_map(uint32_t devid);
struct intel_register_range *intel_get_register_range(struct intel_register_map map, uint32_t offset, uint32_t mode);
#endif /* __GTK_DOC_IGNORE__ */

#endif /* INTEL_GPU_TOOLS_H */
