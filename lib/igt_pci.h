// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef __IGT_PCI_H__
#define __IGT_PCI_H__

#include <stdint.h>
#include <endian.h>

/* forward declaration */
struct pci_device;

#define PCI_TYPE0_1_HEADER_SIZE 0x40
#define PCI_CAPS_START 0x34
#define PCI_CFG_SPACE_SIZE 0x100

enum pci_cap_id {
	PCI_EXPRESS_CAP_ID = 0x10
};

#define PCI_SLOT_CAP_OFFSET 0x14
#define  PCI_SLOT_PWR_CTRL_PRESENT (1 << 1)

int find_pci_cap_offset(struct pci_device *dev, enum pci_cap_id cap_id);

#endif
