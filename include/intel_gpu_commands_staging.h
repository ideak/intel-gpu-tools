/* SPDX-License-Identifier: MIT*/
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef _INTEL_GPU_COMMANDS_STAGING_H_
#define _INTEL_GPU_COMMANDS_STAGING_H_

#include "linux_scaffold.h"

/* Length-free commands */
#define MI_SEMAPHORE_WAIT_CMD		(0x1c << 23)
#define MI_STORE_DWORD_IMM_CMD		(0x20 << 23)
#define MI_STORE_REGISTER_MEM_CMD	(0x24 << 23)
#define MI_FLUSH_DW_CMD			(0x26 << 23)
#define MI_LOAD_REGISTER_MEM_CMD	(0x29 << 23)

#endif /* _INTEL_GPU_COMMANDS_STAGING_H_ */
