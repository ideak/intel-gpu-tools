// SPDX-License-Identifier: MIT
/*
* Copyright © 2022 Intel Corporation
*/

#include <unistd.h>

#include "igt_types.h"

void igt_cleanup_fd(volatile int *fd)
{
	if (!fd || *fd < 0)
		return;

	close(*fd);
	*fd = -1;
}
