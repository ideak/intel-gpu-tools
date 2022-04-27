/*
 * Copyright © 2016 Intel Corporation
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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 */

#ifndef __IGT_SYSFS_H__
#define __IGT_SYSFS_H__

#include <stdbool.h>
#include <stdarg.h>

#define for_each_sysfs_gt_path(i915__, path__, pathlen__) \
	for (int gt__ = 0; \
	     igt_sysfs_gt_path(i915__, gt__, path__, pathlen__) != NULL; \
	     gt__++)

#define for_each_sysfs_gt_dirfd(i915__, dirfd__, gt__) \
	for (gt__ = 0; \
	     (dirfd__ = igt_sysfs_gt_open(i915__, gt__)) != -1; \
	     close(dirfd__), gt__++)

#define igt_sysfs_rps_write(dir, id, data, len) \
	igt_sysfs_write(dir, igt_sysfs_dir_id_to_name(dir, id), data, len)

#define igt_sysfs_rps_read(dir, id, data, len) \
	igt_sysfs_read(dir, igt_sysfs_dir_id_to_name(dir, id), data, len)

#define igt_sysfs_rps_set(dir, id, value) \
	igt_sysfs_set(dir, igt_sysfs_dir_id_to_name(dir, id), value)

#define igt_sysfs_rps_get(dir, id) \
	igt_sysfs_get(dir, igt_sysfs_dir_id_to_name(dir, id))

#define igt_sysfs_rps_scanf(dir, id, fmt, ...) \
	igt_sysfs_scanf(dir, igt_sysfs_dir_id_to_name(dir, id), fmt, ##__VA_ARGS__)

#define igt_sysfs_rps_vprintf(dir, id, fmt, ap) \
	igt_sysfs_vprintf(dir, igt_sysfs_dir_id_to_name(id), fmt, ap)

#define igt_sysfs_rps_printf(dir, id, fmt, ...) \
	igt_sysfs_printf(dir, igt_sysfs_dir_id_to_name(dir, id), fmt, ##__VA_ARGS__)

#define igt_sysfs_rps_get_u32(dir, id) \
	igt_sysfs_get_u32(dir, igt_sysfs_dir_id_to_name(dir, id))

#define igt_sysfs_rps_set_u32(dir, id, value) \
	igt_sysfs_set_u32(dir, igt_sysfs_dir_id_to_name(dir, id), value)

#define igt_sysfs_rps_get_boolean(dir, id) \
	igt_sysfs_get_boolean(dir, igt_sysfs_dir_id_to_name(dir, id))

#define igt_sysfs_rps_set_boolean(dir, id, value) \
	igt_sysfs_set_boolean(dir, igt_sysfs_dir_id_to_name(dir, id), value)

enum i915_attr_id {
	RPS_ACT_FREQ_MHZ,
	RPS_CUR_FREQ_MHZ,
	RPS_MIN_FREQ_MHZ,
	RPS_MAX_FREQ_MHZ,
	RPS_RP0_FREQ_MHZ,
	RPS_RP1_FREQ_MHZ,
	RPS_RPn_FREQ_MHZ,
	RPS_IDLE_FREQ_MHZ,
	RPS_BOOST_FREQ_MHZ,
	RC6_ENABLE,
	RC6_RESIDENCY_MS,
	RC6P_RESIDENCY_MS,
	RC6PP_RESIDENCY_MS,
	MEDIA_RC6_RESIDENCY_MS,

	SYSFS_NUM_ATTR,
};

char *igt_sysfs_path(int device, char *path, int pathlen);
int igt_sysfs_open(int device);
char *igt_sysfs_gt_path(int device, int gt, char *path, int pathlen);
int igt_sysfs_gt_open(int device, int gt);
int igt_sysfs_get_num_gt(int device);
bool igt_sysfs_has_attr(int dir, const char *attr);
const char *igt_sysfs_dir_id_to_name(int dir, enum i915_attr_id id);
const char *igt_sysfs_path_id_to_name(const char *path, enum i915_attr_id id);

int igt_sysfs_read(int dir, const char *attr, void *data, int len);
int igt_sysfs_write(int dir, const char *attr, const void *data, int len);

bool igt_sysfs_set(int dir, const char *attr, const char *value);
char *igt_sysfs_get(int dir, const char *attr);

int igt_sysfs_scanf(int dir, const char *attr, const char *fmt, ...)
	__attribute__((format(scanf,3,4)));
int igt_sysfs_vprintf(int dir, const char *attr, const char *fmt, va_list ap)
	__attribute__((format(printf,3,0)));
int igt_sysfs_printf(int dir, const char *attr, const char *fmt, ...)
	__attribute__((format(printf,3,4)));

uint32_t igt_sysfs_get_u32(int dir, const char *attr);
bool igt_sysfs_set_u32(int dir, const char *attr, uint32_t value);

uint64_t igt_sysfs_get_u64(int dir, const char *attr);
bool igt_sysfs_set_u64(int dir, const char *attr, uint64_t value);

bool igt_sysfs_get_boolean(int dir, const char *attr);
bool igt_sysfs_set_boolean(int dir, const char *attr, bool value);

void bind_fbcon(bool enable);
void kick_snd_hda_intel(void);
void fbcon_blink_enable(bool enable);

#endif /* __IGT_SYSFS_H__ */
