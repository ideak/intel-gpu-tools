/*
 * Copyright © 2014 Intel Corporation
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
 */

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <err.h>
#include <string.h>
#include "intel_io.h"
#include "intel_chipset.h"

static void usage(const char *name)
{
	printf("Warning : This program will work only on Valleyview\n"
	       "Usage: %s <port> <reg> <val>\n"
	       "\t port/reg/val : in 0xXXXX format\n",
	       name);
}

int main(int argc, char** argv)
{
	uint32_t port, reg, val, tmp;
	struct pci_device *dev = intel_get_pci_device();

	if (argc != 4 || !(IS_VALLEYVIEW(dev->device_id) || IS_CHERRYVIEW(dev->device_id))) {
		usage(argv[0]);
		return 1;
	}

	if (!strcasecmp(argv[1], "bunit"))
		port = 0x03;
	else if (!strcasecmp(argv[1], "punit"))
		port = 0x04;
	else if (!strcasecmp(argv[1], "nc"))
		port = 0x11;
	else if (!strcasecmp(argv[1], "dpio"))
		port = 0x13;
	else if (!strcasecmp(argv[1], "gpio_nc"))
		port = 0x13;
	else if (!strcasecmp(argv[1], "cck"))
		port = 0x14;
	else if (!strcasecmp(argv[1], "ccu"))
		port = 0xa9;
	else if (!strcasecmp(argv[1], "dpio2"))
		port = 0x1a;
	else if (!strcasecmp(argv[1], "flisdsi"))
		port = 0x1b;
	else
		port = strtoul(argv[1], NULL, 16);

	reg = strtoul(argv[2], NULL, 16);
	val = strtoul(argv[3], NULL, 16);

	intel_register_access_init(dev, 0);

	tmp = intel_iosf_sb_read(port, reg);
	printf("0x%02x(%s)/0x%04x before : 0x%08x\n", port, argv[1], reg, tmp);

	intel_iosf_sb_write(port, reg, val);

	tmp = intel_iosf_sb_read(port, reg);
	printf("0x%02x(%s)/0x%04x after  : 0x%08x\n", port, argv[1], reg, tmp);

	intel_register_access_fini();

	return 0;
}
