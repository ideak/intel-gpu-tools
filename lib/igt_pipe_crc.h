/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2013 Intel Corporation
 */

#ifndef __IGT_PIPE_CRC_H__
#define __IGT_PIPE_CRC_H__

#include <stdbool.h>
#include <stdint.h>

enum pipe;

/**
 * igt_pipe_crc_t:
 *
 * Pipe CRC support structure. Needs to be allocated and set up with
 * igt_pipe_crc_new() for a specific pipe and pipe CRC source value.
 */
typedef struct _igt_pipe_crc igt_pipe_crc_t;

#define DRM_MAX_CRC_NR 10
/**
 * igt_crc_t:
 * @frame: frame number of the capture CRC
 * @n_words: internal field, don't access
 * @crc: internal field, don't access
 *
 * Pipe CRC value. All other members than @frame are private and should not be
 * inspected by testcases.
 */
struct _igt_crc {
	uint32_t frame;
	bool has_valid_frame;
	int n_words;
	uint32_t crc[DRM_MAX_CRC_NR];
};
typedef struct _igt_crc igt_crc_t;

#define IGT_PIPE_CRC_SOURCE_AUTO "auto"
#define AMDGPU_PIPE_CRC_SOURCE_DPRX "dprx"

bool igt_find_crc_mismatch(const igt_crc_t *a, const igt_crc_t *b, int *index);
void igt_assert_crc_equal(const igt_crc_t *a, const igt_crc_t *b);
bool igt_check_crc_equal(const igt_crc_t *a, const igt_crc_t *b);
char *igt_crc_to_string_extended(igt_crc_t *crc, char delimiter, int crc_size);
char *igt_crc_to_string(igt_crc_t *crc);

void igt_require_pipe_crc(int fd);
igt_pipe_crc_t *
igt_pipe_crc_new(int fd, enum pipe pipe, const char *source);
igt_pipe_crc_t *
igt_pipe_crc_new_nonblock(int fd, enum pipe pipe, const char *source);
void igt_pipe_crc_free(igt_pipe_crc_t *pipe_crc);
void igt_pipe_crc_start(igt_pipe_crc_t *pipe_crc);
void igt_pipe_crc_stop(igt_pipe_crc_t *pipe_crc);
__attribute__((warn_unused_result))
int igt_pipe_crc_get_crcs(igt_pipe_crc_t *pipe_crc, int n_crcs,
			  igt_crc_t **out_crcs);
void igt_pipe_crc_drain(igt_pipe_crc_t *pipe_crc);
void igt_pipe_crc_get_single(igt_pipe_crc_t *pipe_crc, igt_crc_t *out_crc);
void igt_pipe_crc_get_current(int drm_fd, igt_pipe_crc_t *pipe_crc, igt_crc_t *crc);
void igt_pipe_crc_get_for_frame(int drm_fd, igt_pipe_crc_t *pipe_crc,
				unsigned int vblank, igt_crc_t *crc);

void igt_pipe_crc_collect_crc(igt_pipe_crc_t *pipe_crc, igt_crc_t *out_crc);

#endif /* __IGT_PIPE_CRC_H__ */
