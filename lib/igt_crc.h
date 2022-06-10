// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef __IGT_CRC_H__
#define __IGT_CRC_H__

#include <stddef.h>
#include <stdint.h>

/**
 * SECTION:igt_crc
 * @short_description: igt crc tables and calculation functions
 * @title: CRC
 * @include: igt_crc.h
 *
 * # Introduction
 *
 * Providing vendor agnostic crc calculation is useful to avoid code
 * duplication. Especially if vendor will decide to do on-gpu crc calculation
 * it will need to inject crc table to gpu.
 *
 * All crc tables are globals to allow direct in-code use.
 */

const uint32_t igt_crc32_tab[256];

uint32_t igt_cpu_crc32(const void *buf, size_t size);

#endif
