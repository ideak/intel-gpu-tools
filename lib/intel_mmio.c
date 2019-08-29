/*
 * Copyright © 2008 Intel Corporation
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
 *    Ben Widawsky <ben@bwidawsk.net>
 *
 */

#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <err.h>
#include <assert.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include "intel_io.h"
#include "igt_core.h"
#include "igt_gt.h"
#include "intel_chipset.h"

/**
 * SECTION:intel_io
 * @short_description: Register access and sideband I/O library
 * @title: I/O
 * @include: igt.h
 * @section_id: igt-gpu-tools-IO
 *
 * This library provides register I/O helpers in both a basic version and a more
 * fancy version which also handles forcewake and can optionally check registers
 * against a white-list. All register function are compatible. Hence the same
 * code can be used to decode registers with either of them, or also from a dump
 * file using intel_mmio_use_dump_file().
 *
 * Furthermore this library also provides helper functions for accessing the
 * various sideband interfaces found on Valleyview/Baytrail based platforms.
 */

#define FAKEKEY 0x2468ace0

/**
 * igt_global_mmio:
 *
 * Pointer to the register range, initialized using intel_register_access_init()
 * or intel_mmio_use_dump_file(). It is not recommended to use this directly.
 * This pointer is valid only for one drm device.
 */
void *igt_global_mmio;

/**
 * intel_mmio_use_dump_file:
 * @mmio_data:  mmio structure for IO operations
 * @file: name of the register dump file to open
 *
 * Sets also up mmio_data->igt_mmio to point at the data contained
 * in @file. This allows the same code to get reused for dumping and decoding
 * from running hardware as from register dumps.
 */
void
intel_mmio_use_dump_file(struct intel_mmio_data *mmio_data, char *file)
{
	int fd;
	struct stat st;

	memset(mmio_data, 0, sizeof(struct intel_mmio_data));
	fd = open(file, O_RDWR);
	igt_fail_on_f(fd == -1,
		      "Couldn't open %s\n", file);

	fstat(fd, &st);
	mmio_data->igt_mmio = mmap(NULL, st.st_size, PROT_READ|PROT_WRITE, MAP_PRIVATE, fd, 0);
	igt_fail_on_f(mmio_data->igt_mmio == MAP_FAILED,
		      "Couldn't mmap %s\n", file);

	igt_global_mmio = mmio_data->igt_mmio;

	close(fd);
}

/**
 * intel_mmio_use_pci_bar:
 * @mmio_data:  mmio structure for IO operations
 * @pci_dev: intel gracphis pci device
 *
 * Fill a mmio_data stucture with igt_mmio to point at the mmio bar.
 *
 * @pci_dev can be obtained from intel_get_pci_device().
 */
void
intel_mmio_use_pci_bar(struct intel_mmio_data *mmio_data, struct pci_device *pci_dev)
{
	uint32_t devid, gen;
	int mmio_bar, mmio_size;
	int error;

	memset(mmio_data, 0, sizeof(struct intel_mmio_data));
	devid = pci_dev->device_id;
	if (IS_GEN2(devid))
		mmio_bar = 1;
	else
		mmio_bar = 0;

	gen = intel_gen(devid);
	if (gen < 3)
		mmio_size = 512*1024;
	else if (gen < 5)
		mmio_size = 512*1024;
	else
		mmio_size = 2*1024*1024;

	error = pci_device_map_range (pci_dev,
				      pci_dev->regions[mmio_bar].base_addr,
				      mmio_size,
				      PCI_DEV_MAP_FLAG_WRITABLE,
				      &mmio_data->igt_mmio);

	igt_global_mmio = mmio_data->igt_mmio;

	igt_fail_on_f(error != 0,
		      "Couldn't map MMIO region\n");
}

static void
release_forcewake_lock(int fd)
{
	close(fd);
}

/**
 * intel_register_access_init:
 * @mmio_data:  mmio structure for IO operations
 * @pci_dev: intel graphics pci device
 * @safe: use safe register access tables
 *
 * This initializes the new register access library, which supports forcewake
 * handling and also allows register access to be checked with an explicit
 * whitelist.
 *
 * It also initializes mmio_data->igt_mmio like intel_mmio_use_pci_bar().
 *
 * @pci_dev can be obtained from intel_get_pci_device().
 */
int
intel_register_access_init(struct intel_mmio_data *mmio_data, struct pci_device *pci_dev, int safe, int fd)
{
	int ret;

	intel_mmio_use_pci_bar(mmio_data, pci_dev);

	igt_assert(mmio_data->igt_mmio != NULL);

	mmio_data->safe = (safe != 0 &&
			intel_gen(pci_dev->device_id) >= 4) ? true : false;
	mmio_data->pci_device_id = pci_dev->device_id;
	if (mmio_data->safe)
		mmio_data->map = intel_get_register_map(mmio_data->pci_device_id);

	/* Find where the forcewake lock is. Forcewake doesn't exist
	 * gen < 6, but the debugfs should do the right things for us.
	 */
	ret = igt_open_forcewake_handle(fd);
	if (ret == -1)
		mmio_data->key = FAKEKEY;
	else
		mmio_data->key = ret;

	return 0;
}

static int
intel_register_access_needs_wake(struct intel_mmio_data *mmio_data)
{
	return mmio_data->key != FAKEKEY;
}

/**
 * intel_register_access_needs_fakewake:
 * @mmio_data:  mmio structure for IO operations
 *
 * Returns:
 * Non-zero when forcewake initialization failed.
 */
int intel_register_access_needs_fakewake(struct intel_mmio_data *mmio_data)
{
	return mmio_data->key == FAKEKEY;
}

/**
 * intel_register_access_fini:
 * @mmio_data:  mmio structure for IO operations
 *
 * Clean up the register access helper initialized with
 * intel_register_access_init().
 */
void
intel_register_access_fini(struct intel_mmio_data *mmio_data)
{
	if (mmio_data->key && intel_register_access_needs_wake(mmio_data))
		release_forcewake_lock(mmio_data->key);
}

/**
 * intel_register_read:
 * @mmio_data:  mmio structure for IO operations
 * @reg: register offset
 *
 * 32-bit read of the register at @offset. This function only works when the new
 * register access helper is initialized with intel_register_access_init().
 *
 * Compared to INREG() it can do optional checking with the register access
 * white lists.
 *
 * Returns:
 * The value read from the register.
 */
uint32_t
intel_register_read(struct intel_mmio_data *mmio_data, uint32_t reg)
{
	struct intel_register_range *range;
	uint32_t ret;

	if (intel_gen(mmio_data->pci_device_id) >= 6)
		igt_assert(mmio_data->key != -1);

	if (!mmio_data->safe)
		goto read_out;

	range = intel_get_register_range(mmio_data->map,
					 reg,
					 INTEL_RANGE_READ);

	if(!range) {
		igt_warn("Register read blocked for safety ""(*0x%08x)\n", reg);
		ret = 0xffffffff;
		goto out;
	}

read_out:
	ret = ioread32(mmio_data->igt_mmio, reg);
out:
	return ret;
}

/**
 * intel_register_write:
 * @mmio_data:  mmio structure for IO operations
 * @reg: register offset
 * @val: value to write
 *
 * 32-bit write to the register at @offset. This function only works when the new
 * register access helper is initialized with intel_register_access_init().
 *
 * Compared to OUTREG() it can do optional checking with the register access
 * white lists.
 */
void
intel_register_write(struct intel_mmio_data *mmio_data, uint32_t reg, uint32_t val)
{
	struct intel_register_range *range;

	if (intel_gen(mmio_data->pci_device_id) >= 6)
		igt_assert(mmio_data->key != -1);

	if (!mmio_data->safe)
		goto write_out;

	range = intel_get_register_range(mmio_data->map,
					 reg,
					 INTEL_RANGE_WRITE);

	igt_warn_on_f(!range,
		      "Register write blocked for safety ""(*0x%08x = 0x%x)\n", reg, val);

write_out:
	iowrite32(mmio_data->igt_mmio, reg, val);
}
