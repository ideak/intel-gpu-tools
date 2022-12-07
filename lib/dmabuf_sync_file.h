/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef DMABUF_SYNC_FILE_H
#define DMABUF_SYNC_FILE_H

#ifdef __linux__
#include <linux/dma-buf.h>
#endif
#include <sys/poll.h>
#include <stdbool.h>
#include <stdint.h>

bool has_dmabuf_export_sync_file(int fd);
bool has_dmabuf_import_sync_file(int fd);
int dmabuf_export_sync_file(int dmabuf, uint32_t flags);
void dmabuf_import_sync_file(int dmabuf, uint32_t flags, int sync_fd);
void dmabuf_import_timeline_fence(int dmabuf, uint32_t flags,
			     int timeline, uint32_t seqno);
bool dmabuf_busy(int dmabuf, uint32_t flags);
bool sync_file_busy(int sync_file);
bool dmabuf_sync_file_busy(int dmabuf, uint32_t flags);

#endif
