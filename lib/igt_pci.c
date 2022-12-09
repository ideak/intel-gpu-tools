// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include <pciaccess.h>
#include "igt_core.h"
#include "igt_pci.h"

static int find_pci_cap_offset_at(struct pci_device *dev, enum pci_cap_id cap_id,
				  int start_offset)
{
	uint8_t offset = 0xff;
	uint16_t cap_header = 0xffff;
	int loop = (PCI_CFG_SPACE_SIZE - PCI_TYPE0_1_HEADER_SIZE)
			/ sizeof(cap_header);

	if (pci_device_cfg_read_u8(dev, &offset, start_offset))
		return -1;

	while (loop--) {
		igt_assert_f(offset != 0xff, "pci config space inaccessible\n");

		if (offset < PCI_TYPE0_1_HEADER_SIZE)
			break;

		if (pci_device_cfg_read_u16(dev, &cap_header, (offset & 0xFC)))
			return -1;

		if (!cap_id || cap_id == (cap_header & 0xFF))
			return offset;

		offset = cap_header >> 8;
	}

	igt_fail_on_f(loop <= 0 && offset, "pci capability offset doesn't terminate\n");

	return 0;
}

/**
 * find_pci_cap_offset:
 * @dev: pci device
 * @cap_id: searched capability id, 0 means any capability
 *
 * return:
 * -1 on config read error, 0 if capability is not found,
 * otherwise offset at which capability with cap_id is found
 */
int find_pci_cap_offset(struct pci_device *dev, enum pci_cap_id cap_id)
{
	return find_pci_cap_offset_at(dev, cap_id, PCI_CAPS_START);
}
