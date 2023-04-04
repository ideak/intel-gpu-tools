/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2023 Intel Corporation
 *
 * Authors:
 *    Francois Dugast <francois.dugast@intel.com>
 */

#ifndef XE_COMPUTE_H
#define XE_COMPUTE_H

/*
 * OpenCL Kernels are generated using:
 *
 * GPU=tgllp &&                                                         \
 *      ocloc -file opencl/compute_square_kernel.cl -device $GPU &&     \
 *      xxd -i compute_square_kernel_Gen12LPlp.bin
 *
 * For each GPU model desired. A list of supported models can be obtained with: ocloc compile --help
 */

struct xe_compute_kernels {
	int ip_ver;
	unsigned int size;
	const unsigned char *kernel;
};

extern const struct xe_compute_kernels xe_compute_square_kernels[];

bool run_xe_compute_kernel(int fd);

#endif	/* XE_COMPUTE_H */
