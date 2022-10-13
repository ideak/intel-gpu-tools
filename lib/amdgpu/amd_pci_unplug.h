/* SPDX-License-Identifier: MIT
 * Copyright 2022 Advanced Micro Devices, Inc.
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
 *
 *
 */
#ifndef AMD_PCI_UNPLUG_H
#define AMD_PCI_UNPLUG_H

#include <amdgpu.h>
#include <amdgpu_drm.h>

#define MAX_CARDS_SUPPORTED 4

struct amd_pci_unplug_setup {
	uint32_t  major_version_req;
	uint32_t  minor_version_req;
	bool open_device;
	bool open_device2;
};

struct amd_pci_unplug {
	char *sysfs_remove ;
	int drm_amdgpu_fds[MAX_CARDS_SUPPORTED];
	int num_devices;
	amdgpu_device_handle device_handle;
	amdgpu_device_handle device_handle2;
	volatile bool do_cs;
};

void
amdgpu_hotunplug_simple(struct amd_pci_unplug_setup *setup,
						struct amd_pci_unplug *unplug);

void
amdgpu_hotunplug_with_cs(struct amd_pci_unplug_setup *setup,
						 struct amd_pci_unplug *unplug);

void
amdgpu_hotunplug_with_exported_bo(struct amd_pci_unplug_setup *setup,
								  struct amd_pci_unplug *unplug);

void
amdgpu_hotunplug_with_exported_fence(struct amd_pci_unplug_setup *setup,
									 struct amd_pci_unplug *unplug);


#endif
