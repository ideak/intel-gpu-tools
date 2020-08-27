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

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <inttypes.h>
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/types.h>
#include <dirent.h>
#include <time.h>
#include <poll.h>
#include <math.h>

#include "i915/gem.h"
#include "i915/perf.h"
#include "igt.h"
#include "igt_sysfs.h"
#include "drm.h"

IGT_TEST_DESCRIPTION("Test the i915 perf metrics streaming interface");

#define GEN6_MI_REPORT_PERF_COUNT ((0x28 << 23) | (3 - 2))
#define GEN8_MI_REPORT_PERF_COUNT ((0x28 << 23) | (4 - 2))

#define OAREPORT_REASON_MASK           0x3f
#define OAREPORT_REASON_SHIFT          19
#define OAREPORT_REASON_TIMER          (1<<0)
#define OAREPORT_REASON_INTERNAL       (3<<1)
#define OAREPORT_REASON_CTX_SWITCH     (1<<3)
#define OAREPORT_REASON_GO             (1<<4)
#define OAREPORT_REASON_CLK_RATIO      (1<<5)

#define GFX_OP_PIPE_CONTROL     ((3 << 29) | (3 << 27) | (2 << 24))
#define PIPE_CONTROL_CS_STALL	   (1 << 20)
#define PIPE_CONTROL_GLOBAL_SNAPSHOT_COUNT_RESET	(1 << 19)
#define PIPE_CONTROL_TLB_INVALIDATE     (1 << 18)
#define PIPE_CONTROL_SYNC_GFDT	  (1 << 17)
#define PIPE_CONTROL_MEDIA_STATE_CLEAR  (1 << 16)
#define PIPE_CONTROL_NO_WRITE	   (0 << 14)
#define PIPE_CONTROL_WRITE_IMMEDIATE    (1 << 14)
#define PIPE_CONTROL_WRITE_DEPTH_COUNT  (2 << 14)
#define PIPE_CONTROL_WRITE_TIMESTAMP    (3 << 14)
#define PIPE_CONTROL_DEPTH_STALL	(1 << 13)
#define PIPE_CONTROL_RENDER_TARGET_FLUSH (1 << 12)
#define PIPE_CONTROL_INSTRUCTION_INVALIDATE (1 << 11)
#define PIPE_CONTROL_TEXTURE_CACHE_INVALIDATE   (1 << 10) /* GM45+ only */
#define PIPE_CONTROL_ISP_DIS	    (1 << 9)
#define PIPE_CONTROL_INTERRUPT_ENABLE   (1 << 8)
#define PIPE_CONTROL_FLUSH_ENABLE       (1 << 7) /* Gen7+ only */
/* GT */
#define PIPE_CONTROL_DATA_CACHE_INVALIDATE      (1 << 5)
#define PIPE_CONTROL_VF_CACHE_INVALIDATE	(1 << 4)
#define PIPE_CONTROL_CONST_CACHE_INVALIDATE     (1 << 3)
#define PIPE_CONTROL_STATE_CACHE_INVALIDATE     (1 << 2)
#define PIPE_CONTROL_STALL_AT_SCOREBOARD	(1 << 1)
#define PIPE_CONTROL_DEPTH_CACHE_FLUSH	  (1 << 0)
#define PIPE_CONTROL_PPGTT_WRITE	(0 << 2)
#define PIPE_CONTROL_GLOBAL_GTT_WRITE   (1 << 2)

#define MAX_OA_BUF_SIZE (16 * 1024 * 1024)

#define NUM_PROPERTIES(p) (sizeof(p) / (2 * sizeof(uint64_t)))

struct accumulator {
#define MAX_RAW_OA_COUNTERS 62
	enum drm_i915_oa_format format;

	uint64_t deltas[MAX_RAW_OA_COUNTERS];
};

struct oa_format {
	const char *name;
	size_t size;
	int a40_high_off; /* bytes */
	int a40_low_off;
	int n_a40;
	int a_off;
	int n_a;
	int first_a;
	int b_off;
	int n_b;
	int c_off;
	int n_c;
};

static struct oa_format hsw_oa_formats[I915_OA_FORMAT_MAX] = {
	[I915_OA_FORMAT_A13] = { /* HSW only */
		"A13", .size = 64,
		.a_off = 12, .n_a = 13, },
	[I915_OA_FORMAT_A29] = { /* HSW only */
		"A29", .size = 128,
		.a_off = 12, .n_a = 29, },
	[I915_OA_FORMAT_A13_B8_C8] = { /* HSW only */
		"A13_B8_C8", .size = 128,
		.a_off = 12, .n_a = 13,
		.b_off = 64, .n_b = 8,
		.c_off = 96, .n_c = 8, },
	[I915_OA_FORMAT_A45_B8_C8] = { /* HSW only */
		"A45_B8_C8", .size = 256,
		.a_off = 12,  .n_a = 45,
		.b_off = 192, .n_b = 8,
		.c_off = 224, .n_c = 8, },
	[I915_OA_FORMAT_B4_C8] = { /* HSW only */
		"B4_C8", .size = 64,
		.b_off = 16, .n_b = 4,
		.c_off = 32, .n_c = 8, },
	[I915_OA_FORMAT_B4_C8_A16] = { /* HSW only */
		"B4_C8_A16", .size = 128,
		.b_off = 16, .n_b = 4,
		.c_off = 32, .n_c = 8,
		.a_off = 60, .n_a = 16, .first_a = 29, },
	[I915_OA_FORMAT_C4_B8] = { /* HSW+ (header differs from HSW-Gen8+) */
		"C4_B8", .size = 64,
		.c_off = 16, .n_c = 4,
		.b_off = 28, .n_b = 8 },
};

static struct oa_format gen8_oa_formats[I915_OA_FORMAT_MAX] = {
	[I915_OA_FORMAT_A12] = {
		"A12", .size = 64,
		.a_off = 12, .n_a = 12, .first_a = 7, },
	[I915_OA_FORMAT_A12_B8_C8] = {
		"A12_B8_C8", .size = 128,
		.a_off = 12, .n_a = 12,
		.b_off = 64, .n_b = 8,
		.c_off = 96, .n_c = 8, .first_a = 7, },
	[I915_OA_FORMAT_A32u40_A4u32_B8_C8] = {
		"A32u40_A4u32_B8_C8", .size = 256,
		.a40_high_off = 160, .a40_low_off = 16, .n_a40 = 32,
		.a_off = 144, .n_a = 4, .first_a = 32,
		.b_off = 192, .n_b = 8,
		.c_off = 224, .n_c = 8, },
	[I915_OA_FORMAT_C4_B8] = {
		"C4_B8", .size = 64,
		.c_off = 16, .n_c = 4,
		.b_off = 32, .n_b = 8, },
};

static struct oa_format gen12_oa_formats[I915_OA_FORMAT_MAX] = {
	[I915_OA_FORMAT_A32u40_A4u32_B8_C8] = {
		"A32u40_A4u32_B8_C8", .size = 256,
		.a40_high_off = 160, .a40_low_off = 16, .n_a40 = 32,
		.a_off = 144, .n_a = 4, .first_a = 32,
		.b_off = 192, .n_b = 8,
		.c_off = 224, .n_c = 8, },
};

static bool hsw_undefined_a_counters[45] = {
	[4] = true,
	[6] = true,
	[9] = true,
	[11] = true,
	[14] = true,
	[16] = true,
	[19] = true,
	[21] = true,
	[24] = true,
	[26] = true,
	[29] = true,
	[31] = true,
	[34] = true,
	[43] = true,
	[44] = true,
};

/* No A counters currently reserved/undefined for gen8+ so far */
static bool gen8_undefined_a_counters[45];

static int drm_fd = -1;
static int sysfs = -1;
static int pm_fd = -1;
static int stream_fd = -1;
static uint32_t devid;

static uint64_t gt_max_freq_mhz = 0;
static struct intel_perf *intel_perf = NULL;
static struct intel_perf_metric_set *test_set = NULL;
static bool *undefined_a_counters;
static uint64_t oa_exp_1_millisec;

static igt_render_copyfunc_t render_copy = NULL;
static uint32_t (*read_report_ticks)(uint32_t *report,
				     enum drm_i915_oa_format format);
static void (*sanity_check_reports)(uint32_t *oa_report0, uint32_t *oa_report1,
				    enum drm_i915_oa_format format);

static void
dump_report(const uint32_t *report, uint32_t size, const char *message) {
	uint32_t i;
	igt_debug("%s\n", message);
	for (i = 0; i < size; i += 4) {
		igt_debug("%08x %08x %08x %08x\n",
				report[i],
				report[i + 1],
				report[i + 2],
				report[i + 3]);
	}
}

static struct oa_format
get_oa_format(enum drm_i915_oa_format format)
{
	if (IS_HASWELL(devid))
		return hsw_oa_formats[format];
	else if (IS_GEN12(devid))
		return gen12_oa_formats[format];
	else
		return gen8_oa_formats[format];
}

static char *
pretty_print_oa_period(uint64_t oa_period_ns)
{
	static char result[100];
	static const char *units[4] = { "ns", "us", "ms", "s" };
	double val = oa_period_ns;
	int iter = 0;

	while (iter < (ARRAY_SIZE(units) - 1) &&
	       val >= 1000.0f) {
		val /= 1000.0f;
		iter++;
	}

	snprintf(result, sizeof(result), "%.3f%s", val, units[iter]);
	return result;
}

static void
__perf_close(int fd)
{
	close(fd);
	stream_fd = -1;

	if (pm_fd >= 0) {
		close(pm_fd);
		pm_fd = -1;
	}
}

static int
__perf_open(int fd, struct drm_i915_perf_open_param *param, bool prevent_pm)
{
	int ret;
	int32_t pm_value = 0;

	if (stream_fd >= 0)
		__perf_close(stream_fd);
	if (pm_fd >= 0) {
		close(pm_fd);
		pm_fd = -1;
	}

	ret = igt_ioctl(fd, DRM_IOCTL_I915_PERF_OPEN, param);

	igt_assert(ret >= 0);
	errno = 0;

	if (prevent_pm) {
		pm_fd = open("/dev/cpu_dma_latency", O_RDWR);
		igt_assert(pm_fd >= 0);

		igt_assert_eq(write(pm_fd, &pm_value, sizeof(pm_value)), sizeof(pm_value));
	}

	return ret;
}

static int
lookup_format(int i915_perf_fmt_id)
{
	igt_assert(i915_perf_fmt_id < I915_OA_FORMAT_MAX);
	igt_assert(get_oa_format(i915_perf_fmt_id).name);

	return i915_perf_fmt_id;
}

static uint64_t
read_u64_file(const char *path)
{
	FILE *f;
	uint64_t val;

	f = fopen(path, "r");
	igt_assert(f);

	igt_assert_eq(fscanf(f, "%"PRIu64, &val), 1);

	fclose(f);

	return val;
}

static void
write_u64_file(const char *path, uint64_t val)
{
	FILE *f;

	f = fopen(path, "w");
	igt_assert(f);

	igt_assert(fprintf(f, "%"PRIu64, val) > 0);

	fclose(f);
}

static bool
try_sysfs_read_u64(const char *path, uint64_t *val)
{
	return igt_sysfs_scanf(sysfs, path, "%"PRIu64, val) == 1;
}

static unsigned long
sysfs_read(const char *path)
{
	unsigned long value;

	igt_assert(igt_sysfs_scanf(sysfs, path, "%lu", &value) == 1);

	return value;
}

/* XXX: For Haswell this utility is only applicable to the render basic
 * metric set.
 *
 * C2 corresponds to a clock counter for the Haswell render basic metric set
 * but it's not included in all of the formats.
 */
static uint32_t
hsw_read_report_ticks(uint32_t *report, enum drm_i915_oa_format format)
{
	uint32_t *c = (uint32_t *)(((uint8_t *)report) + get_oa_format(format).c_off);

	igt_assert_neq(get_oa_format(format).n_c, 0);

	return c[2];
}

static uint32_t
gen8_read_report_ticks(uint32_t *report, enum drm_i915_oa_format format)
{
	return report[3];
}

static void
gen8_read_report_clock_ratios(uint32_t *report,
			      uint32_t *slice_freq_mhz,
			      uint32_t *unslice_freq_mhz)
{
	uint32_t unslice_freq = report[0] & 0x1ff;
	uint32_t slice_freq_low = (report[0] >> 25) & 0x7f;
	uint32_t slice_freq_high = (report[0] >> 9) & 0x3;
	uint32_t slice_freq = slice_freq_low | (slice_freq_high << 7);

	*slice_freq_mhz = (slice_freq * 16666) / 1000;
	*unslice_freq_mhz = (unslice_freq * 16666) / 1000;
}

static const char *
gen8_read_report_reason(const uint32_t *report)
{
	uint32_t reason = ((report[0] >> OAREPORT_REASON_SHIFT) &
			   OAREPORT_REASON_MASK);

	if (reason & (1<<0))
		return "timer";
	else if (reason & (1<<1))
	      return "internal trigger 1";
	else if (reason & (1<<2))
	      return "internal trigger 2";
	else if (reason & (1<<3))
	      return "context switch";
	else if (reason & (1<<4))
	      return "GO 1->0 transition (enter RC6)";
	else if (reason & (1<<5))
		return "[un]slice clock ratio change";
	else
		return "unknown";
}

static uint64_t
timebase_scale(uint32_t u32_delta)
{
	return ((uint64_t)u32_delta * NSEC_PER_SEC) / intel_perf->devinfo.timestamp_frequency;
}

/* Returns: the largest OA exponent that will still result in a sampling period
 * less than or equal to the given @period.
 */
static int
max_oa_exponent_for_period_lte(uint64_t period)
{
	/* NB: timebase_scale() takes a uint32_t and an exponent of 30
	 * would already represent a period of ~3 minutes so there's
	 * really no need to consider higher exponents.
	 */
	for (int i = 0; i < 30; i++) {
		uint64_t oa_period = timebase_scale(2 << i);

		if (oa_period > period)
			return max(0, i - 1);
	}

	igt_assert(!"reached");
	return -1;
}

/* Return: the largest OA exponent that will still result in a sampling
 * frequency greater than the given @frequency.
 */
static int
max_oa_exponent_for_freq_gt(uint64_t frequency)
{
	uint64_t period = NSEC_PER_SEC / frequency;

	igt_assert_neq(period, 0);

	return max_oa_exponent_for_period_lte(period - 1);
}

static uint64_t
oa_exponent_to_ns(int exponent)
{
       return 1000000000ULL * (2ULL << exponent) / intel_perf->devinfo.timestamp_frequency;
}

static bool
oa_report_is_periodic(uint32_t oa_exponent, const uint32_t *report)
{
	if (IS_HASWELL(devid)) {
		/* For Haswell we don't have a documented report reason field
		 * (though empirically report[0] bit 10 does seem to correlate
		 * with a timer trigger reason) so we instead infer which
		 * reports are timer triggered by checking if the least
		 * significant bits are zero and the exponent bit is set.
		 */
		uint32_t oa_exponent_mask = (1 << (oa_exponent + 1)) - 1;

		if ((report[1] & oa_exponent_mask) == (1 << oa_exponent))
			return true;
	} else {
		if ((report[0] >> OAREPORT_REASON_SHIFT) &
		    OAREPORT_REASON_TIMER)
			return true;
	}

	return false;
}

static bool
oa_report_ctx_is_valid(uint32_t *report)
{
	if (IS_HASWELL(devid)) {
		return false; /* TODO */
	} else if (IS_GEN8(devid)) {
		return report[0] & (1ul << 25);
	} else if (AT_LEAST_GEN(devid, 9)) {
		return report[0] & (1ul << 16);
	}

	igt_assert(!"Please update this function for newer Gen");
}

static uint32_t
oa_report_get_ctx_id(uint32_t *report)
{
	if (!oa_report_ctx_is_valid(report))
		return 0xffffffff;
	return report[2];
}

static void
scratch_buf_memset(drm_intel_bo *bo, int width, int height, uint32_t color)
{
	int ret;

	ret = drm_intel_bo_map(bo, true /* writable */);
	igt_assert_eq(ret, 0);

	for (int i = 0; i < width * height; i++)
		((uint32_t *)bo->virtual)[i] = color;

	drm_intel_bo_unmap(bo);
}

static void
scratch_buf_init(drm_intel_bufmgr *bufmgr,
		 struct igt_buf *buf,
		 int width, int height,
		 uint32_t color)
{
	size_t stride = width * 4;
	size_t size = stride * height;
	drm_intel_bo *bo = drm_intel_bo_alloc(bufmgr, "", size, 4096);

	scratch_buf_memset(bo, width, height, color);

	memset(buf, 0, sizeof(*buf));

	buf->bo = bo;
	buf->surface[0].stride = stride;
	buf->tiling = I915_TILING_NONE;
	buf->surface[0].size = size;
	buf->bpp = 32;
}

static void
emit_report_perf_count(struct intel_batchbuffer *batch,
		       drm_intel_bo *dst_bo,
		       int dst_offset,
		       uint32_t report_id)
{
	if (IS_HASWELL(devid)) {
		BEGIN_BATCH(3, 1);
		OUT_BATCH(GEN6_MI_REPORT_PERF_COUNT);
		OUT_RELOC(dst_bo, I915_GEM_DOMAIN_INSTRUCTION, I915_GEM_DOMAIN_INSTRUCTION,
			  dst_offset);
		OUT_BATCH(report_id);
		ADVANCE_BATCH();
	} else {
		/* XXX: NB: n dwords arg is actually magic since it internally
		 * automatically accounts for larger addresses on gen >= 8...
		 */
		BEGIN_BATCH(3, 1);
		OUT_BATCH(GEN8_MI_REPORT_PERF_COUNT);
		OUT_RELOC(dst_bo, I915_GEM_DOMAIN_INSTRUCTION, I915_GEM_DOMAIN_INSTRUCTION,
			  dst_offset);
		OUT_BATCH(report_id);
		ADVANCE_BATCH();
	}
}

static void
hsw_sanity_check_render_basic_reports(uint32_t *oa_report0, uint32_t *oa_report1,
				      enum drm_i915_oa_format fmt)
{
	uint32_t time_delta = timebase_scale(oa_report1[1] - oa_report0[1]);
	uint32_t clock_delta;
	uint32_t max_delta;
	struct oa_format format = get_oa_format(fmt);

	igt_assert_neq(time_delta, 0);

	/* As a special case we have to consider that on Haswell we
	 * can't explicitly derive a clock delta for all OA report
	 * formats...
	 */
	if (format.n_c == 0) {
		/* Assume running at max freq for sake of
		 * below sanity check on counters... */
		clock_delta = (gt_max_freq_mhz *
			       (uint64_t)time_delta) / 1000;
	} else {
		uint32_t ticks0 = read_report_ticks(oa_report0, fmt);
		uint32_t ticks1 = read_report_ticks(oa_report1, fmt);
		uint64_t freq;

		clock_delta = ticks1 - ticks0;

		igt_assert_neq(clock_delta, 0);

		freq = ((uint64_t)clock_delta * 1000) / time_delta;
		igt_debug("freq = %"PRIu64"\n", freq);

		igt_assert(freq <= gt_max_freq_mhz);
	}

	igt_debug("clock delta = %"PRIu32"\n", clock_delta);

	/* The maximum rate for any HSW counter =
	 *   clock_delta * N EUs
	 *
	 * Sanity check that no counters exceed this delta.
	 */
	max_delta = clock_delta * intel_perf->devinfo.n_eus;

	/* 40bit A counters were only introduced for Gen8+ */
	igt_assert_eq(format.n_a40, 0);

	for (int j = 0; j < format.n_a; j++) {
		uint32_t *a0 = (uint32_t *)(((uint8_t *)oa_report0) +
					    format.a_off);
		uint32_t *a1 = (uint32_t *)(((uint8_t *)oa_report1) +
					    format.a_off);
		int a_id = format.first_a + j;
		uint32_t delta = a1[j] - a0[j];

		if (undefined_a_counters[a_id])
			continue;

		igt_debug("A%d: delta = %"PRIu32"\n", a_id, delta);
		igt_assert(delta <= max_delta);
	}

	for (int j = 0; j < format.n_b; j++) {
		uint32_t *b0 = (uint32_t *)(((uint8_t *)oa_report0) +
					    format.b_off);
		uint32_t *b1 = (uint32_t *)(((uint8_t *)oa_report1) +
					    format.b_off);
		uint32_t delta = b1[j] - b0[j];

		igt_debug("B%d: delta = %"PRIu32"\n", j, delta);
		igt_assert(delta <= max_delta);
	}

	for (int j = 0; j < format.n_c; j++) {
		uint32_t *c0 = (uint32_t *)(((uint8_t *)oa_report0) +
					    format.c_off);
		uint32_t *c1 = (uint32_t *)(((uint8_t *)oa_report1) +
					    format.c_off);
		uint32_t delta = c1[j] - c0[j];

		igt_debug("C%d: delta = %"PRIu32"\n", j, delta);
		igt_assert(delta <= max_delta);
	}
}

static uint64_t
gen8_read_40bit_a_counter(uint32_t *report, enum drm_i915_oa_format fmt, int a_id)
{
	struct oa_format format = get_oa_format(fmt);
	uint8_t *a40_high = (((uint8_t *)report) + format.a40_high_off);
	uint32_t *a40_low = (uint32_t *)(((uint8_t *)report) +
					 format.a40_low_off);
	uint64_t high = (uint64_t)(a40_high[a_id]) << 32;

	return a40_low[a_id] | high;
}

static uint64_t
gen8_40bit_a_delta(uint64_t value0, uint64_t value1)
{
	if (value0 > value1)
		return (1ULL << 40) + value1 - value0;
	else
		return value1 - value0;
}

static void
accumulate_uint32(size_t offset,
		  uint32_t *report0,
                  uint32_t *report1,
                  uint64_t *delta)
{
	uint32_t value0 = *(uint32_t *)(((uint8_t *)report0) + offset);
	uint32_t value1 = *(uint32_t *)(((uint8_t *)report1) + offset);

	*delta += (uint32_t)(value1 - value0);
}

static void
accumulate_uint40(int a_index,
                  uint32_t *report0,
                  uint32_t *report1,
		  enum drm_i915_oa_format format,
                  uint64_t *delta)
{
	uint64_t value0 = gen8_read_40bit_a_counter(report0, format, a_index),
		 value1 = gen8_read_40bit_a_counter(report1, format, a_index);

	*delta += gen8_40bit_a_delta(value0, value1);
}

static void
accumulate_reports(struct accumulator *accumulator,
		   uint32_t *start,
		   uint32_t *end)
{
	struct oa_format format = get_oa_format(accumulator->format);
	uint64_t *deltas = accumulator->deltas;
	int idx = 0;

	if (intel_gen(devid) >= 8) {
		/* timestamp */
		accumulate_uint32(4, start, end, deltas + idx++);

		/* clock cycles */
		accumulate_uint32(12, start, end, deltas + idx++);
	} else {
		/* timestamp */
		accumulate_uint32(4, start, end, deltas + idx++);
	}

	for (int i = 0; i < format.n_a40; i++) {
		accumulate_uint40(i, start, end, accumulator->format,
				  deltas + idx++);
	}

	for (int i = 0; i < format.n_a; i++) {
		accumulate_uint32(format.a_off + 4 * i,
				  start, end, deltas + idx++);
	}

	for (int i = 0; i < format.n_b; i++) {
		accumulate_uint32(format.b_off + 4 * i,
				  start, end, deltas + idx++);
	}

	for (int i = 0; i < format.n_c; i++) {
		accumulate_uint32(format.c_off + 4 * i,
				  start, end, deltas + idx++);
	}
}

static void
accumulator_print(struct accumulator *accumulator, const char *title)
{
	struct oa_format format = get_oa_format(accumulator->format);
	uint64_t *deltas = accumulator->deltas;
	int idx = 0;

	igt_debug("%s:\n", title);
	if (intel_gen(devid) >= 8) {
		igt_debug("\ttime delta = %"PRIu64"\n", deltas[idx++]);
		igt_debug("\tclock cycle delta = %"PRIu64"\n", deltas[idx++]);

		for (int i = 0; i < format.n_a40; i++)
			igt_debug("\tA%u = %"PRIu64"\n", i, deltas[idx++]);
	} else {
		igt_debug("\ttime delta = %"PRIu64"\n", deltas[idx++]);
	}

	for (int i = 0; i < format.n_a; i++) {
		int a_id = format.first_a + i;
		igt_debug("\tA%u = %"PRIu64"\n", a_id, deltas[idx++]);
	}

	for (int i = 0; i < format.n_a; i++)
		igt_debug("\tB%u = %"PRIu64"\n", i, deltas[idx++]);

	for (int i = 0; i < format.n_c; i++)
		igt_debug("\tC%u = %"PRIu64"\n", i, deltas[idx++]);
}

/* The TestOa metric set is designed so */
static void
gen8_sanity_check_test_oa_reports(uint32_t *oa_report0, uint32_t *oa_report1,
				  enum drm_i915_oa_format fmt)
{
	struct oa_format format = get_oa_format(fmt);
	uint32_t time_delta = timebase_scale(oa_report1[1] - oa_report0[1]);
	uint32_t ticks0 = read_report_ticks(oa_report0, fmt);
	uint32_t ticks1 = read_report_ticks(oa_report1, fmt);
	uint32_t clock_delta = ticks1 - ticks0;
	uint32_t max_delta;
	uint64_t freq;
	uint32_t *rpt0_b = (uint32_t *)(((uint8_t *)oa_report0) +
					format.b_off);
	uint32_t *rpt1_b = (uint32_t *)(((uint8_t *)oa_report1) +
					format.b_off);
	uint32_t b;
	uint32_t ref;


	igt_assert_neq(time_delta, 0);
	igt_assert_neq(clock_delta, 0);

	freq = ((uint64_t)clock_delta * 1000) / time_delta;
	igt_debug("freq = %"PRIu64"\n", freq);

	igt_assert(freq <= gt_max_freq_mhz);

	igt_debug("clock delta = %"PRIu32"\n", clock_delta);

	max_delta = clock_delta * intel_perf->devinfo.n_eus;

	/* Gen8+ has some 40bit A counters... */
	for (int j = 0; j < format.n_a40; j++) {
		uint64_t value0 = gen8_read_40bit_a_counter(oa_report0, fmt, j);
		uint64_t value1 = gen8_read_40bit_a_counter(oa_report1, fmt, j);
		uint64_t delta = gen8_40bit_a_delta(value0, value1);

		if (undefined_a_counters[j])
			continue;

		igt_debug("A%d: delta = %"PRIu64"\n", j, delta);
		igt_assert(delta <= max_delta);
	}

	for (int j = 0; j < format.n_a; j++) {
		uint32_t *a0 = (uint32_t *)(((uint8_t *)oa_report0) +
					    format.a_off);
		uint32_t *a1 = (uint32_t *)(((uint8_t *)oa_report1) +
					    format.a_off);
		int a_id = format.first_a + j;
		uint32_t delta = a1[j] - a0[j];

		if (undefined_a_counters[a_id])
			continue;

		igt_debug("A%d: delta = %"PRIu32"\n", a_id, delta);
		igt_assert(delta <= max_delta);
	}

	/* The TestOa metric set defines all B counters to be a
	 * multiple of the gpu clock
	 */
	if (format.n_b) {
		b = rpt1_b[0] - rpt0_b[0];
		igt_debug("B0: delta = %"PRIu32"\n", b);
		igt_assert_eq(b, 0);

		b = rpt1_b[1] - rpt0_b[1];
		igt_debug("B1: delta = %"PRIu32"\n", b);
		igt_assert_eq(b, clock_delta);

		b = rpt1_b[2] - rpt0_b[2];
		igt_debug("B2: delta = %"PRIu32"\n", b);
		igt_assert_eq(b, clock_delta);

		b = rpt1_b[3] - rpt0_b[3];
		ref = clock_delta / 2;
		igt_debug("B3: delta = %"PRIu32"\n", b);
		igt_assert(b >= ref - 1 && b <= ref + 1);

		b = rpt1_b[4] - rpt0_b[4];
		ref = clock_delta / 3;
		igt_debug("B4: delta = %"PRIu32"\n", b);
		igt_assert(b >= ref - 1 && b <= ref + 1);

		b = rpt1_b[5] - rpt0_b[5];
		ref = clock_delta / 3;
		igt_debug("B5: delta = %"PRIu32"\n", b);
		igt_assert(b >= ref - 1 && b <= ref + 1);

		b = rpt1_b[6] - rpt0_b[6];
		ref = clock_delta / 6;
		igt_debug("B6: delta = %"PRIu32"\n", b);
		igt_assert(b >= ref - 1 && b <= ref + 1);

		b = rpt1_b[7] - rpt0_b[7];
		ref = clock_delta * 2 / 3;
		igt_debug("B7: delta = %"PRIu32"\n", b);
		igt_assert(b >= ref - 1 && b <= ref + 1);
	}

	for (int j = 0; j < format.n_c; j++) {
		uint32_t *c0 = (uint32_t *)(((uint8_t *)oa_report0) +
					    format.c_off);
		uint32_t *c1 = (uint32_t *)(((uint8_t *)oa_report1) +
					    format.c_off);
		uint32_t delta = c1[j] - c0[j];

		igt_debug("C%d: delta = %"PRIu32", max_delta=%"PRIu32"\n",
			  j, delta, max_delta);
		igt_assert(delta <= max_delta);
	}
}

static bool
init_sys_info(void)
{
	const char *test_set_name = NULL;
	struct intel_perf_metric_set *metric_set_iter;

	igt_assert_neq(devid, 0);

	intel_perf = intel_perf_for_fd(drm_fd);
	igt_require(intel_perf);

	igt_debug("n_eu_slices: %"PRIu64"\n", intel_perf->devinfo.n_eu_slices);
	igt_debug("n_eu_sub_slices: %"PRIu64"\n", intel_perf->devinfo.n_eu_sub_slices);
	igt_debug("n_eus: %"PRIu64"\n", intel_perf->devinfo.n_eus);
	igt_debug("timestamp_frequency = %"PRIu64"\n",
		  intel_perf->devinfo.timestamp_frequency);
	igt_assert_neq(intel_perf->devinfo.timestamp_frequency, 0);

	/* We don't have a TestOa metric set for Haswell so use
	 * RenderBasic
	 */
	if (IS_HASWELL(devid)) {
		test_set_name = "RenderBasic";
		read_report_ticks = hsw_read_report_ticks;
		sanity_check_reports = hsw_sanity_check_render_basic_reports;
		undefined_a_counters = hsw_undefined_a_counters;
	} else {
		test_set_name = "TestOa";
		read_report_ticks = gen8_read_report_ticks;
		sanity_check_reports = gen8_sanity_check_test_oa_reports;
		undefined_a_counters = gen8_undefined_a_counters;
	}

	igt_list_for_each_entry(metric_set_iter, &intel_perf->metric_sets, link) {
		if (strcmp(metric_set_iter->symbol_name, test_set_name) == 0) {
			test_set = metric_set_iter;
			break;
		}
	}

	if (!test_set)
		return false;

	igt_debug("%s metric set UUID = %s\n",
		  test_set->symbol_name,
		  test_set->hw_config_guid);

	intel_perf_load_perf_configs(intel_perf, drm_fd);

	if (test_set->perf_oa_metrics_set == 0) {
		igt_debug("Unable to load configurations\n");
		return false;
	}

	oa_exp_1_millisec = max_oa_exponent_for_period_lte(1000000);

	return true;
}

static int
i915_read_reports_until_timestamp(enum drm_i915_oa_format oa_format,
				  uint8_t *buf,
				  uint32_t max_size,
				  uint32_t start_timestamp,
				  uint32_t end_timestamp)
{
	size_t format_size = get_oa_format(oa_format).size;
	uint32_t last_seen_timestamp = start_timestamp;
	int total_len = 0;

	while (last_seen_timestamp < end_timestamp) {
		int offset, len;

		/* Running out of space. */
		if ((max_size - total_len) < format_size) {
			igt_warn("run out of space before reaching "
				 "end timestamp (%u/%u)\n",
				 last_seen_timestamp, end_timestamp);
			return -1;
		}

		while ((len = read(stream_fd, &buf[total_len],
				   max_size - total_len)) < 0 &&
		       errno == EINTR)
			;

		/* Intentionally return an error. */
		if (len <= 0) {
			if (errno == EAGAIN)
				return total_len;
			else {
				igt_warn("error read OA stream : %i\n", errno);
				return -1;
			}
		}

		offset = total_len;
		total_len += len;

		while (offset < total_len) {
			const struct drm_i915_perf_record_header *header =
				(const struct drm_i915_perf_record_header *) &buf[offset];
			uint32_t *report = (uint32_t *) (header + 1);

			if (header->type == DRM_I915_PERF_RECORD_SAMPLE)
				last_seen_timestamp = report[1];

			offset += header->size;
		}
	}

	return total_len;
}

/* CAP_SYS_ADMIN is required to open system wide metrics, unless the system
 * control parameter dev.i915.perf_stream_paranoid == 0 */
static void
test_system_wide_paranoid(void)
{
	igt_fork(child, 1) {
		uint64_t properties[] = {
			/* Include OA reports in samples */
			DRM_I915_PERF_PROP_SAMPLE_OA, true,

			/* OA unit configuration */
			DRM_I915_PERF_PROP_OA_METRICS_SET, test_set->perf_oa_metrics_set,
			DRM_I915_PERF_PROP_OA_FORMAT, test_set->perf_oa_format,
			DRM_I915_PERF_PROP_OA_EXPONENT, oa_exp_1_millisec,
		};
		struct drm_i915_perf_open_param param = {
			.flags = I915_PERF_FLAG_FD_CLOEXEC |
				I915_PERF_FLAG_FD_NONBLOCK,
			.num_properties = sizeof(properties) / 16,
			.properties_ptr = to_user_pointer(properties),
		};

		write_u64_file("/proc/sys/dev/i915/perf_stream_paranoid", 1);

		igt_drop_root();

		do_ioctl_err(drm_fd, DRM_IOCTL_I915_PERF_OPEN, &param, EACCES);
	}

	igt_waitchildren();

	igt_fork(child, 1) {
		uint64_t properties[] = {
			/* Include OA reports in samples */
			DRM_I915_PERF_PROP_SAMPLE_OA, true,

			/* OA unit configuration */
			DRM_I915_PERF_PROP_OA_METRICS_SET, test_set->perf_oa_metrics_set,
			DRM_I915_PERF_PROP_OA_FORMAT, test_set->perf_oa_format,
			DRM_I915_PERF_PROP_OA_EXPONENT, oa_exp_1_millisec,
		};
		struct drm_i915_perf_open_param param = {
			.flags = I915_PERF_FLAG_FD_CLOEXEC |
				I915_PERF_FLAG_FD_NONBLOCK,
			.num_properties = sizeof(properties) / 16,
			.properties_ptr = to_user_pointer(properties),
		};
		write_u64_file("/proc/sys/dev/i915/perf_stream_paranoid", 0);

		igt_drop_root();

		stream_fd = __perf_open(drm_fd, &param, false);
		__perf_close(stream_fd);
	}

	igt_waitchildren();

	/* leave in paranoid state */
	write_u64_file("/proc/sys/dev/i915/perf_stream_paranoid", 1);
}

static void
test_invalid_open_flags(void)
{
	uint64_t properties[] = {
		/* Include OA reports in samples */
		DRM_I915_PERF_PROP_SAMPLE_OA, true,

		/* OA unit configuration */
		DRM_I915_PERF_PROP_OA_METRICS_SET, test_set->perf_oa_metrics_set,
		DRM_I915_PERF_PROP_OA_FORMAT, test_set->perf_oa_format,
		DRM_I915_PERF_PROP_OA_EXPONENT, oa_exp_1_millisec,
	};
	struct drm_i915_perf_open_param param = {
		.flags = ~0, /* Undefined flag bits set! */
		.num_properties = sizeof(properties) / 16,
		.properties_ptr = to_user_pointer(properties),
	};

	do_ioctl_err(drm_fd, DRM_IOCTL_I915_PERF_OPEN, &param, EINVAL);
}

static void
test_invalid_oa_metric_set_id(void)
{
	uint64_t properties[] = {
		/* Include OA reports in samples */
		DRM_I915_PERF_PROP_SAMPLE_OA, true,

		/* OA unit configuration */
		DRM_I915_PERF_PROP_OA_FORMAT, test_set->perf_oa_format,
		DRM_I915_PERF_PROP_OA_EXPONENT, oa_exp_1_millisec,
		DRM_I915_PERF_PROP_OA_METRICS_SET, UINT64_MAX,
	};
	struct drm_i915_perf_open_param param = {
		.flags = I915_PERF_FLAG_FD_CLOEXEC |
			I915_PERF_FLAG_FD_NONBLOCK,
		.num_properties = sizeof(properties) / 16,
		.properties_ptr = to_user_pointer(properties),
	};

	do_ioctl_err(drm_fd, DRM_IOCTL_I915_PERF_OPEN, &param, EINVAL);

	properties[ARRAY_SIZE(properties) - 1] = 0; /* ID 0 is also be reserved as invalid */
	do_ioctl_err(drm_fd, DRM_IOCTL_I915_PERF_OPEN, &param, EINVAL);

	/* Check that we aren't just seeing false positives... */
	properties[ARRAY_SIZE(properties) - 1] = test_set->perf_oa_metrics_set;
	stream_fd = __perf_open(drm_fd, &param, false);
	__perf_close(stream_fd);

	/* There's no valid default OA metric set ID... */
	param.num_properties--;
	do_ioctl_err(drm_fd, DRM_IOCTL_I915_PERF_OPEN, &param, EINVAL);
}

static void
test_invalid_oa_format_id(void)
{
	uint64_t properties[] = {
		/* Include OA reports in samples */
		DRM_I915_PERF_PROP_SAMPLE_OA, true,

		/* OA unit configuration */
		DRM_I915_PERF_PROP_OA_METRICS_SET, test_set->perf_oa_metrics_set,
		DRM_I915_PERF_PROP_OA_EXPONENT, oa_exp_1_millisec,
		DRM_I915_PERF_PROP_OA_FORMAT, UINT64_MAX,
	};
	struct drm_i915_perf_open_param param = {
		.flags = I915_PERF_FLAG_FD_CLOEXEC |
			I915_PERF_FLAG_FD_NONBLOCK,
		.num_properties = sizeof(properties) / 16,
		.properties_ptr = to_user_pointer(properties),
	};

	do_ioctl_err(drm_fd, DRM_IOCTL_I915_PERF_OPEN, &param, EINVAL);

	properties[ARRAY_SIZE(properties) - 1] = 0; /* ID 0 is also be reserved as invalid */
	do_ioctl_err(drm_fd, DRM_IOCTL_I915_PERF_OPEN, &param, EINVAL);

	/* Check that we aren't just seeing false positives... */
	properties[ARRAY_SIZE(properties) - 1] = test_set->perf_oa_format;
	stream_fd = __perf_open(drm_fd, &param, false);
	__perf_close(stream_fd);

	/* There's no valid default OA format... */
	param.num_properties--;
	do_ioctl_err(drm_fd, DRM_IOCTL_I915_PERF_OPEN, &param, EINVAL);
}

static void
test_missing_sample_flags(void)
{
	uint64_t properties[] = {
		/* No _PROP_SAMPLE_xyz flags */

		/* OA unit configuration */
		DRM_I915_PERF_PROP_OA_METRICS_SET, test_set->perf_oa_metrics_set,
		DRM_I915_PERF_PROP_OA_EXPONENT, oa_exp_1_millisec,
		DRM_I915_PERF_PROP_OA_FORMAT, test_set->perf_oa_format,
	};
	struct drm_i915_perf_open_param param = {
		.flags = I915_PERF_FLAG_FD_CLOEXEC,
		.num_properties = sizeof(properties) / 16,
		.properties_ptr = to_user_pointer(properties),
	};

	do_ioctl_err(drm_fd, DRM_IOCTL_I915_PERF_OPEN, &param, EINVAL);
}

static void
read_2_oa_reports(int format_id,
		  int exponent,
		  uint32_t *oa_report0,
		  uint32_t *oa_report1,
		  bool timer_only)
{
	size_t format_size = get_oa_format(format_id).size;
	size_t sample_size = (sizeof(struct drm_i915_perf_record_header) +
			      format_size);
	const struct drm_i915_perf_record_header *header;
	uint32_t exponent_mask = (1 << (exponent + 1)) - 1;

	/* Note: we allocate a large buffer so that each read() iteration
	 * should scrape *all* pending records.
	 *
	 * The largest buffer the OA unit supports is 16MB.
	 *
	 * Being sure we are fetching all buffered reports allows us to
	 * potentially throw away / skip all reports whenever we see
	 * a _REPORT_LOST notification as a way of being sure are
	 * measurements aren't skewed by a lost report.
	 *
	 * Note: that is is useful for some tests but also not something
	 * applications would be expected to resort to. Lost reports are
	 * somewhat unpredictable but typically don't pose a problem - except
	 * to indicate that the OA unit may be over taxed if lots of reports
	 * are being lost.
	 */
	int max_reports = MAX_OA_BUF_SIZE / format_size;
	int buf_size = sample_size * max_reports * 1.5;
	uint8_t *buf = malloc(buf_size);
	int n = 0;

	for (int i = 0; i < 1000; i++) {
		ssize_t len;

		while ((len = read(stream_fd, buf, buf_size)) < 0 &&
		       errno == EINTR)
			;

		igt_assert(len > 0);
		igt_debug("read %d bytes\n", (int)len);

		for (size_t offset = 0; offset < len; offset += header->size) {
			const uint32_t *report;

			header = (void *)(buf + offset);

			igt_assert_eq(header->pad, 0); /* Reserved */

			/* Currently the only test that should ever expect to
			 * see a _BUFFER_LOST error is the buffer_fill test,
			 * otherwise something bad has probably happened...
			 */
			igt_assert_neq(header->type, DRM_I915_PERF_RECORD_OA_BUFFER_LOST);

			/* At high sampling frequencies the OA HW might not be
			 * able to cope with all write requests and will notify
			 * us that a report was lost. We restart our read of
			 * two sequential reports due to the timeline blip this
			 * implies
			 */
			if (header->type == DRM_I915_PERF_RECORD_OA_REPORT_LOST) {
				igt_debug("read restart: OA trigger collision / report lost\n");
				n = 0;

				/* XXX: break, because we don't know where
				 * within the series of already read reports
				 * there could be a blip from the lost report.
				 */
				break;
			}

			/* Currently the only other record type expected is a
			 * _SAMPLE. Notably this test will need updating if
			 * i915-perf is extended in the future with additional
			 * record types.
			 */
			igt_assert_eq(header->type, DRM_I915_PERF_RECORD_SAMPLE);

			igt_assert_eq(header->size, sample_size);

			report = (const void *)(header + 1);
			dump_report(report, 64, "oa-formats");

			igt_debug("read report: reason = %x, timestamp = %x, exponent mask=%x\n",
				  report[0], report[1], exponent_mask);

			/* Don't expect zero for timestamps */
			igt_assert_neq(report[1], 0);

			if (timer_only) {
				if (!oa_report_is_periodic(exponent, report)) {
					igt_debug("skipping non timer report\n");
					continue;
				}
			}

			if (n++ == 0)
				memcpy(oa_report0, report, format_size);
			else {
				memcpy(oa_report1, report, format_size);
				free(buf);
				return;
			}
		}
	}

	free(buf);

	igt_assert(!"reached");
}

static void
open_and_read_2_oa_reports(int format_id,
			   int exponent,
			   uint32_t *oa_report0,
			   uint32_t *oa_report1,
			   bool timer_only)
{
	uint64_t properties[] = {
		/* Include OA reports in samples */
		DRM_I915_PERF_PROP_SAMPLE_OA, true,

		/* OA unit configuration */
		DRM_I915_PERF_PROP_OA_METRICS_SET, test_set->perf_oa_metrics_set,
		DRM_I915_PERF_PROP_OA_FORMAT, format_id,
		DRM_I915_PERF_PROP_OA_EXPONENT, exponent,

	};
	struct drm_i915_perf_open_param param = {
		.flags = I915_PERF_FLAG_FD_CLOEXEC,
		.num_properties = sizeof(properties) / 16,
		.properties_ptr = to_user_pointer(properties),
	};

	stream_fd = __perf_open(drm_fd, &param, false);

	read_2_oa_reports(format_id, exponent,
			  oa_report0, oa_report1, timer_only);

	__perf_close(stream_fd);
}

static void
print_reports(uint32_t *oa_report0, uint32_t *oa_report1, int fmt)
{
	struct oa_format format = get_oa_format(fmt);

	igt_debug("TIMESTAMP: 1st = %"PRIu32", 2nd = %"PRIu32", delta = %"PRIu32"\n",
		  oa_report0[1], oa_report1[1], oa_report1[1] - oa_report0[1]);

	if (IS_HASWELL(devid) && format.n_c == 0) {
		igt_debug("CLOCK = N/A\n");
	} else {
		uint32_t clock0 = read_report_ticks(oa_report0, fmt);
		uint32_t clock1 = read_report_ticks(oa_report1, fmt);

		igt_debug("CLOCK: 1st = %"PRIu32", 2nd = %"PRIu32", delta = %"PRIu32"\n",
			  clock0, clock1, clock1 - clock0);
	}

	if (intel_gen(devid) >= 8) {
		uint32_t slice_freq0, slice_freq1, unslice_freq0, unslice_freq1;
		const char *reason0 = gen8_read_report_reason(oa_report0);
		const char *reason1 = gen8_read_report_reason(oa_report1);

		igt_debug("CTX ID: 1st = %"PRIu32", 2nd = %"PRIu32"\n",
			  oa_report0[2], oa_report1[2]);

		gen8_read_report_clock_ratios(oa_report0,
					      &slice_freq0, &unslice_freq0);
		gen8_read_report_clock_ratios(oa_report1,
					      &slice_freq1, &unslice_freq1);

		igt_debug("SLICE CLK: 1st = %umhz, 2nd = %umhz, delta = %d\n",
			  slice_freq0, slice_freq1,
			  ((int)slice_freq1 - (int)slice_freq0));
		igt_debug("UNSLICE CLK: 1st = %umhz, 2nd = %umhz, delta = %d\n",
			  unslice_freq0, unslice_freq1,
			  ((int)unslice_freq1 - (int)unslice_freq0));

		igt_debug("REASONS: 1st = \"%s\", 2nd = \"%s\"\n", reason0, reason1);
	}

	/* Gen8+ has some 40bit A counters... */
	for (int j = 0; j < format.n_a40; j++) {
		uint64_t value0 = gen8_read_40bit_a_counter(oa_report0, fmt, j);
		uint64_t value1 = gen8_read_40bit_a_counter(oa_report1, fmt, j);
		uint64_t delta = gen8_40bit_a_delta(value0, value1);

		if (undefined_a_counters[j])
			continue;

		igt_debug("A%d: 1st = %"PRIu64", 2nd = %"PRIu64", delta = %"PRIu64"\n",
			  j, value0, value1, delta);
	}

	for (int j = 0; j < format.n_a; j++) {
		uint32_t *a0 = (uint32_t *)(((uint8_t *)oa_report0) +
					    format.a_off);
		uint32_t *a1 = (uint32_t *)(((uint8_t *)oa_report1) +
					    format.a_off);
		int a_id = format.first_a + j;
		uint32_t delta = a1[j] - a0[j];

		if (undefined_a_counters[a_id])
			continue;

		igt_debug("A%d: 1st = %"PRIu32", 2nd = %"PRIu32", delta = %"PRIu32"\n",
			  a_id, a0[j], a1[j], delta);
	}

	for (int j = 0; j < format.n_b; j++) {
		uint32_t *b0 = (uint32_t *)(((uint8_t *)oa_report0) +
					    format.b_off);
		uint32_t *b1 = (uint32_t *)(((uint8_t *)oa_report1) +
					    format.b_off);
		uint32_t delta = b1[j] - b0[j];

		igt_debug("B%d: 1st = %"PRIu32", 2nd = %"PRIu32", delta = %"PRIu32"\n",
			  j, b0[j], b1[j], delta);
	}

	for (int j = 0; j < format.n_c; j++) {
		uint32_t *c0 = (uint32_t *)(((uint8_t *)oa_report0) +
					    format.c_off);
		uint32_t *c1 = (uint32_t *)(((uint8_t *)oa_report1) +
					    format.c_off);
		uint32_t delta = c1[j] - c0[j];

		igt_debug("C%d: 1st = %"PRIu32", 2nd = %"PRIu32", delta = %"PRIu32"\n",
			  j, c0[j], c1[j], delta);
	}
}

/* Debug function, only useful when reports don't make sense. */
#if 0
static void
print_report(uint32_t *report, int fmt)
{
	struct oa_format format = get_oa_format(fmt);

	igt_debug("TIMESTAMP: %"PRIu32"\n", report[1]);

	if (IS_HASWELL(devid) && format.n_c == 0) {
		igt_debug("CLOCK = N/A\n");
	} else {
		uint32_t clock = read_report_ticks(report, fmt);

		igt_debug("CLOCK: %"PRIu32"\n", clock);
	}

	if (intel_gen(devid) >= 8) {
		uint32_t slice_freq, unslice_freq;
		const char *reason = gen8_read_report_reason(report);

		gen8_read_report_clock_ratios(report, &slice_freq, &unslice_freq);

		igt_debug("SLICE CLK: %umhz\n", slice_freq);
		igt_debug("UNSLICE CLK: %umhz\n", unslice_freq);
		igt_debug("REASON: \"%s\"\n", reason);
		igt_debug("CTX ID: %"PRIu32"/%"PRIx32"\n", report[2], report[2]);
	}

	/* Gen8+ has some 40bit A counters... */
	for (int j = 0; j < format.n_a40; j++) {
		uint64_t value = gen8_read_40bit_a_counter(report, fmt, j);

		if (undefined_a_counters[j])
			continue;

		igt_debug("A%d: %"PRIu64"\n", j, value);
	}

	for (int j = 0; j < format.n_a; j++) {
		uint32_t *a = (uint32_t *)(((uint8_t *)report) +
					   format.a_off);
		int a_id = format.first_a + j;

		if (undefined_a_counters[a_id])
			continue;

		igt_debug("A%d: %"PRIu32"\n", a_id, a[j]);
	}

	for (int j = 0; j < format.n_b; j++) {
		uint32_t *b = (uint32_t *)(((uint8_t *)report) +
					   format.b_off);

		igt_debug("B%d: %"PRIu32"\n", j, b[j]);
	}

	for (int j = 0; j < format.n_c; j++) {
		uint32_t *c = (uint32_t *)(((uint8_t *)report) +
					   format.c_off);

		igt_debug("C%d: %"PRIu32"\n", j, c[j]);
	}
}
#endif

static void
test_oa_formats(void)
{
	for (int i = 0; i < I915_OA_FORMAT_MAX; i++) {
		struct oa_format format = get_oa_format(i);
		uint32_t oa_report0[64];
		uint32_t oa_report1[64];

		if (!format.name) /* sparse, indexed by ID */
			continue;

		igt_debug("Checking OA format %s\n", format.name);

		open_and_read_2_oa_reports(i,
					   oa_exp_1_millisec,
					   oa_report0,
					   oa_report1,
					   false); /* timer reports only */

		print_reports(oa_report0, oa_report1, i);
		sanity_check_reports(oa_report0, oa_report1, i);
	}
}


enum load {
	LOW,
	HIGH
};

#define LOAD_HELPER_PAUSE_USEC 500

static struct load_helper {
	int devid;
	drm_intel_bufmgr *bufmgr;
	drm_intel_context *context;
	uint32_t context_id;
	struct intel_batchbuffer *batch;
	enum load load;
	bool exit;
	struct igt_helper_process igt_proc;
	struct igt_buf src, dst;
} lh = { 0, };

static void load_helper_signal_handler(int sig)
{
	if (sig == SIGUSR2)
		lh.load = lh.load == LOW ? HIGH : LOW;
	else
		lh.exit = true;
}

static void load_helper_set_load(enum load load)
{
	igt_assert(lh.igt_proc.running);

	if (lh.load == load)
		return;

	lh.load = load;
	kill(lh.igt_proc.pid, SIGUSR2);
}

static void load_helper_run(enum load load)
{
	/*
	 * FIXME fork helpers won't get cleaned up when started from within a
	 * subtest, so handle the case where it sticks around a bit too long.
	 */
	if (lh.igt_proc.running) {
		load_helper_set_load(load);
		return;
	}

	lh.load = load;

	igt_fork_helper(&lh.igt_proc) {
		signal(SIGUSR1, load_helper_signal_handler);
		signal(SIGUSR2, load_helper_signal_handler);

		while (!lh.exit) {
			int ret;

			render_copy(lh.batch,
				    lh.context,
				    &lh.src, 0, 0, 1920, 1080,
				    &lh.dst, 0, 0);

			intel_batchbuffer_flush_with_context(lh.batch,
							     lh.context);

			ret = drm_intel_gem_context_get_id(lh.context,
							   &lh.context_id);
			igt_assert_eq(ret, 0);

			drm_intel_bo_wait_rendering(lh.dst.bo);

			/* Lower the load by pausing after every submitted
			 * write. */
			if (lh.load == LOW)
				usleep(LOAD_HELPER_PAUSE_USEC);
		}
	}
}

static void load_helper_stop(void)
{
	kill(lh.igt_proc.pid, SIGUSR1);
	igt_assert(igt_wait_helper(&lh.igt_proc) == 0);
}

static void load_helper_init(void)
{
	int ret;

	lh.devid = intel_get_drm_devid(drm_fd);

	/* MI_STORE_DATA can only use GTT address on gen4+/g33 and needs
	 * snoopable mem on pre-gen6. Hence load-helper only works on gen6+, but
	 * that's also all we care about for the rps testcase*/
	igt_assert(intel_gen(lh.devid) >= 6);
	lh.bufmgr = drm_intel_bufmgr_gem_init(drm_fd, 4096);
	igt_assert(lh.bufmgr);

	drm_intel_bufmgr_gem_enable_reuse(lh.bufmgr);

	lh.context = drm_intel_gem_context_create(lh.bufmgr);
	igt_assert(lh.context);

	lh.context_id = 0xffffffff;
	ret = drm_intel_gem_context_get_id(lh.context, &lh.context_id);
	igt_assert_eq(ret, 0);
	igt_assert_neq(lh.context_id, 0xffffffff);

	lh.batch = intel_batchbuffer_alloc(lh.bufmgr, lh.devid);
	igt_assert(lh.batch);

	scratch_buf_init(lh.bufmgr, &lh.dst, 1920, 1080, 0);
	scratch_buf_init(lh.bufmgr, &lh.src, 1920, 1080, 0);
}

static void load_helper_fini(void)
{
	if (lh.igt_proc.running)
		load_helper_stop();

	if (lh.src.bo)
		drm_intel_bo_unreference(lh.src.bo);
	if (lh.dst.bo)
		drm_intel_bo_unreference(lh.dst.bo);

	if (lh.batch)
		intel_batchbuffer_free(lh.batch);

	if (lh.context)
		drm_intel_gem_context_destroy(lh.context);

	if (lh.bufmgr)
		drm_intel_bufmgr_destroy(lh.bufmgr);
}

static bool expected_report_timing_delta(uint32_t delta, uint32_t expected_delta)
{
	/*
	 * On ICL, the OA unit appears to be a bit more relaxed about
	 * its timing for emitting OA reports (often missing the
	 * deadline by 1 timestamp).
	 */
	if (IS_ICELAKE(devid))
		return delta <= (expected_delta + 3);
	else
		return delta <= expected_delta;
}

static void
test_oa_exponents(void)
{
	load_helper_init();
	load_helper_run(HIGH);

	/* It's asking a lot to sample with a 160 nanosecond period and the
	 * test can fail due to buffer overflows if it wasn't possible to
	 * keep up, so we don't start from an exponent of zero...
	 */
	for (int exponent = 5; exponent < 20; exponent++) {
		uint64_t properties[] = {
			/* Include OA reports in samples */
			DRM_I915_PERF_PROP_SAMPLE_OA, true,

			/* OA unit configuration */
			DRM_I915_PERF_PROP_OA_METRICS_SET, test_set->perf_oa_metrics_set,
			DRM_I915_PERF_PROP_OA_FORMAT, test_set->perf_oa_format,
			DRM_I915_PERF_PROP_OA_EXPONENT, exponent,
		};
		struct drm_i915_perf_open_param param = {
			.flags = I915_PERF_FLAG_FD_CLOEXEC,
			.num_properties = ARRAY_SIZE(properties) / 2,
			.properties_ptr = to_user_pointer(properties),
		};
		uint64_t expected_timestamp_delta = 2ULL << exponent;
		size_t format_size = get_oa_format(test_set->perf_oa_format).size;
		size_t sample_size = (sizeof(struct drm_i915_perf_record_header) +
				      format_size);
		int max_reports = MAX_OA_BUF_SIZE / format_size;
		int buf_size = sample_size * max_reports * 1.5;
		uint8_t *buf = calloc(1, buf_size);
		int ret, n_timer_reports = 0;
		uint32_t matches = 0;
		struct {
			uint32_t report[64];
		} timer_reports[30];

		igt_debug("testing OA exponent %d,"
			  " expected ts delta = %"PRIu64" (%"PRIu64"ns/%.2fus/%.2fms)\n",
			  exponent, expected_timestamp_delta,
			  oa_exponent_to_ns(exponent),
			  oa_exponent_to_ns(exponent) / 1000.0,
			  oa_exponent_to_ns(exponent) / (1000.0 * 1000.0));

		stream_fd = __perf_open(drm_fd, &param, true /* prevent_pm */);

		while (n_timer_reports < ARRAY_SIZE(timer_reports)) {
			struct drm_i915_perf_record_header *header;

			while ((ret = read(stream_fd, buf, buf_size)) < 0 &&
			       errno == EINTR)
				;

			/* igt_debug(" > read %i bytes\n", ret); */

			/* We should never have no data. */
			igt_assert(ret > 0);

			for (int offset = 0;
			     offset < ret && n_timer_reports < ARRAY_SIZE(timer_reports);
			     offset += header->size) {
				uint32_t *report;

				header = (void *)(buf + offset);

				if (header->type == DRM_I915_PERF_RECORD_OA_BUFFER_LOST) {
					igt_assert(!"reached");
					break;
				}

				if (header->type == DRM_I915_PERF_RECORD_OA_REPORT_LOST)
					igt_debug("report loss\n");

				if (header->type != DRM_I915_PERF_RECORD_SAMPLE)
					continue;

				report = (void *)(header + 1);

				if (!oa_report_is_periodic(exponent, report))
					continue;

				memcpy(timer_reports[n_timer_reports].report, report,
				       sizeof(timer_reports[n_timer_reports].report));
				n_timer_reports++;
			}
		}

		__perf_close(stream_fd);

		igt_debug("report%04i ts=%08x hw_id=0x%08x\n", 0,
			  timer_reports[0].report[1],
			  oa_report_get_ctx_id(timer_reports[0].report));
		for (int i = 1; i < n_timer_reports; i++) {
			uint32_t delta =
				timer_reports[i].report[1] - timer_reports[i - 1].report[1];

			igt_debug("report%04i ts=%08x hw_id=0x%08x delta=%u %s\n", i,
				  timer_reports[i].report[1],
				  oa_report_get_ctx_id(timer_reports[i].report),
				  delta, expected_report_timing_delta(delta,
								      expected_timestamp_delta) ? "" : "******");

			matches += expected_report_timing_delta(delta,expected_timestamp_delta);
		}

		igt_debug("matches=%u/%u\n", matches, n_timer_reports - 1);

		/*
		 * Expect half the reports to match the timing
		 * expectation. The results are quite erratic because
		 * the condition under which the HW reaches
		 * expectations depends on memory controller pressure
		 * etc...
		 */
		igt_assert_lte(n_timer_reports / 2, matches);
	}

	load_helper_stop();
	load_helper_fini();
}

/* The OA exponent selects a timestamp counter bit to trigger reports on.
 *
 * With a 64bit timestamp and least significant bit approx == 80ns then the MSB
 * equates to > 40 thousand years and isn't exposed via the i915 perf interface.
 *
 * The max exponent exposed is expected to be 31, which is still a fairly
 * ridiculous period (>5min) but is the maximum exponent where it's still
 * possible to use periodic sampling as a means for tracking the overflow of
 * 32bit OA report timestamps.
 */
static void
test_invalid_oa_exponent(void)
{
	uint64_t properties[] = {
		/* Include OA reports in samples */
		DRM_I915_PERF_PROP_SAMPLE_OA, true,

		/* OA unit configuration */
		DRM_I915_PERF_PROP_OA_METRICS_SET, test_set->perf_oa_metrics_set,
		DRM_I915_PERF_PROP_OA_FORMAT, test_set->perf_oa_format,
		DRM_I915_PERF_PROP_OA_EXPONENT, 31, /* maximum exponent expected
						       to be accepted */
	};
	struct drm_i915_perf_open_param param = {
		.flags = I915_PERF_FLAG_FD_CLOEXEC,
		.num_properties = sizeof(properties) / 16,
		.properties_ptr = to_user_pointer(properties),
	};

	stream_fd = __perf_open(drm_fd, &param, false);

	__perf_close(stream_fd);

	for (int i = 32; i < 65; i++) {
		properties[7] = i;
		do_ioctl_err(drm_fd, DRM_IOCTL_I915_PERF_OPEN, &param, EINVAL);
	}
}

/* The lowest periodic sampling exponent equates to a period of 160 nanoseconds
 * or a frequency of 6.25MHz which is only possible to request as root by
 * default. By default the maximum OA sampling rate is 100KHz
 */
static void
test_low_oa_exponent_permissions(void)
{
	int max_freq = read_u64_file("/proc/sys/dev/i915/oa_max_sample_rate");
	int bad_exponent = max_oa_exponent_for_freq_gt(max_freq);
	int ok_exponent = bad_exponent + 1;
	uint64_t properties[] = {
		/* Include OA reports in samples */
		DRM_I915_PERF_PROP_SAMPLE_OA, true,

		/* OA unit configuration */
		DRM_I915_PERF_PROP_OA_METRICS_SET, test_set->perf_oa_metrics_set,
		DRM_I915_PERF_PROP_OA_FORMAT, test_set->perf_oa_format,
		DRM_I915_PERF_PROP_OA_EXPONENT, bad_exponent,
	};
	struct drm_i915_perf_open_param param = {
		.flags = I915_PERF_FLAG_FD_CLOEXEC,
		.num_properties = sizeof(properties) / 16,
		.properties_ptr = to_user_pointer(properties),
	};
	uint64_t oa_period, oa_freq;

	igt_assert_eq(max_freq, 100000);

	/* Avoid EACCES errors opening a stream without CAP_SYS_ADMIN */
	write_u64_file("/proc/sys/dev/i915/perf_stream_paranoid", 0);

	igt_fork(child, 1) {
		igt_drop_root();

		do_ioctl_err(drm_fd, DRM_IOCTL_I915_PERF_OPEN, &param, EACCES);
	}

	igt_waitchildren();

	properties[7] = ok_exponent;

	igt_fork(child, 1) {
		igt_drop_root();

		stream_fd = __perf_open(drm_fd, &param, false);
		__perf_close(stream_fd);
	}

	igt_waitchildren();

	oa_period = timebase_scale(2 << ok_exponent);
	oa_freq = NSEC_PER_SEC / oa_period;
	write_u64_file("/proc/sys/dev/i915/oa_max_sample_rate", oa_freq - 100);

	igt_fork(child, 1) {
		igt_drop_root();

		do_ioctl_err(drm_fd, DRM_IOCTL_I915_PERF_OPEN, &param, EACCES);
	}

	igt_waitchildren();

	/* restore the defaults */
	write_u64_file("/proc/sys/dev/i915/oa_max_sample_rate", 100000);
	write_u64_file("/proc/sys/dev/i915/perf_stream_paranoid", 1);
}

static void
test_per_context_mode_unprivileged(void)
{
	uint64_t properties[] = {
		/* Single context sampling */
		DRM_I915_PERF_PROP_CTX_HANDLE, UINT64_MAX, /* updated below */

		/* Include OA reports in samples */
		DRM_I915_PERF_PROP_SAMPLE_OA, true,

		/* OA unit configuration */
		DRM_I915_PERF_PROP_OA_METRICS_SET, test_set->perf_oa_metrics_set,
		DRM_I915_PERF_PROP_OA_FORMAT, test_set->perf_oa_format,
		DRM_I915_PERF_PROP_OA_EXPONENT, oa_exp_1_millisec,
	};
	struct drm_i915_perf_open_param param = {
		.flags = I915_PERF_FLAG_FD_CLOEXEC,
		.num_properties = sizeof(properties) / 16,
		.properties_ptr = to_user_pointer(properties),
	};

	/* should be default, but just to be sure... */
	write_u64_file("/proc/sys/dev/i915/perf_stream_paranoid", 1);

	igt_fork(child, 1) {
		drm_intel_context *context;
		drm_intel_bufmgr *bufmgr;
		uint32_t ctx_id = 0xffffffff; /* invalid id */
		int ret;

		igt_drop_root();

		bufmgr = drm_intel_bufmgr_gem_init(drm_fd, 4096);
		context = drm_intel_gem_context_create(bufmgr);

		igt_assert(context);

		ret = drm_intel_gem_context_get_id(context, &ctx_id);
		igt_assert_eq(ret, 0);
		igt_assert_neq(ctx_id, 0xffffffff);

		properties[1] = ctx_id;

		stream_fd = __perf_open(drm_fd, &param, false);
		__perf_close(stream_fd);

		drm_intel_gem_context_destroy(context);
		drm_intel_bufmgr_destroy(bufmgr);
	}

	igt_waitchildren();
}

static int64_t
get_time(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);

	return ts.tv_sec * 1000000000 + ts.tv_nsec;
}

/* Note: The interface doesn't currently provide strict guarantees or control
 * over the upper bound for how long it might take for a POLLIN event after
 * some OA report is written by the OA unit.
 *
 * The plan is to add a property later that gives some control over the maximum
 * latency, but for now we expect it is tuned for a fairly low latency
 * suitable for applications wanting to provide live feedback for captured
 * metrics.
 *
 * At the time of writing this test the driver was using a fixed 200Hz hrtimer
 * regardless of the OA sampling exponent.
 *
 * There is no lower bound since a stream configured for periodic sampling may
 * still contain other automatically triggered reports.
 *
 * What we try and check for here is that blocking reads don't return EAGAIN
 * and that we aren't spending any significant time burning the cpu in
 * kernelspace.
 */
static void
test_blocking(uint64_t requested_oa_period, bool set_kernel_hrtimer, uint64_t kernel_hrtimer)
{
	int oa_exponent = max_oa_exponent_for_period_lte(requested_oa_period);
	uint64_t oa_period = oa_exponent_to_ns(oa_exponent);
	uint64_t properties[] = {
		/* Include OA reports in samples */
		DRM_I915_PERF_PROP_SAMPLE_OA, true,

		/* OA unit configuration */
		DRM_I915_PERF_PROP_OA_METRICS_SET, test_set->perf_oa_metrics_set,
		DRM_I915_PERF_PROP_OA_FORMAT, test_set->perf_oa_format,
		DRM_I915_PERF_PROP_OA_EXPONENT, oa_exponent,

		/* Kernel configuration (optional) */
		DRM_I915_PERF_PROP_POLL_OA_PERIOD, kernel_hrtimer,
	};
	struct drm_i915_perf_open_param param = {
		.flags = I915_PERF_FLAG_FD_CLOEXEC |
			I915_PERF_FLAG_DISABLED,
		.num_properties = set_kernel_hrtimer ?
				  NUM_PROPERTIES(properties) :
				  NUM_PROPERTIES(properties) - 1,
		.properties_ptr = to_user_pointer(properties),
	};
	uint8_t buf[1024 * 1024];
	struct tms start_times;
	struct tms end_times;
	int64_t user_ns, kernel_ns;
	int64_t tick_ns = 1000000000 / sysconf(_SC_CLK_TCK);
	int64_t test_duration_ns = tick_ns * 1000;

	int max_iterations = (test_duration_ns / oa_period) + 2;
	int n_extra_iterations = 0;

	/* It's a bit tricky to put a lower limit here, but we expect a
	 * relatively low latency for seeing reports, while we don't currently
	 * give any control over this in the api.
	 *
	 * We assume a maximum latency of 6 millisecond to deliver a POLLIN and
	 * read() after a new sample is written (46ms per iteration) considering
	 * the knowledge that that the driver uses a 200Hz hrtimer (5ms period)
	 * to check for data and giving some time to read().
	 */
	int min_iterations = (test_duration_ns / (oa_period + kernel_hrtimer + kernel_hrtimer / 5));

	int64_t start, end;
	int n = 0;
	uint64_t max_expected_kernel_ns;

	stream_fd = __perf_open(drm_fd, &param, true /* prevent_pm */);

	times(&start_times);

	igt_debug("tick length = %dns, test duration = %"PRIu64"ns, min iter. = %d,"
		  " estimated max iter. = %d, oa_period = %s\n",
		  (int)tick_ns, test_duration_ns,
		  min_iterations, max_iterations,
		  pretty_print_oa_period(oa_period));

	/* In the loop we perform blocking polls while the HW is sampling at
	 * ~25Hz, with the expectation that we spend most of our time blocked
	 * in the kernel, and shouldn't be burning cpu cycles in the kernel in
	 * association with this process (verified by looking at stime before
	 * and after loop).
	 *
	 * We're looking to assert that less than 1% of the test duration is
	 * spent in the kernel dealing with polling and read()ing.
	 *
	 * The test runs for a relatively long time considering the very low
	 * resolution of stime in ticks of typically 10 milliseconds. Since we
	 * don't know the fractional part of tick values we read from userspace
	 * so our minimum threshold needs to be >= one tick since any
	 * measurement might really be +- tick_ns (assuming we effectively get
	 * floor(real_stime)).
	 *
	 * We Loop for 1000 x tick_ns so one tick corresponds to 0.1%
	 *
	 * Also enable the stream just before poll/read to minimize
	 * the error delta.
	 */
	start = get_time();
	do_ioctl(stream_fd, I915_PERF_IOCTL_ENABLE, 0);
	for (/* nop */; ((end = get_time()) - start) < test_duration_ns; /* nop */) {
		struct drm_i915_perf_record_header *header;
		bool timer_report_read = false;
		bool non_timer_report_read = false;
		int ret;

		while ((ret = read(stream_fd, buf, sizeof(buf))) < 0 &&
		       errno == EINTR)
			;

		igt_assert(ret > 0);

		/* For Haswell reports don't contain a well defined reason
		 * field we so assume all reports to be 'periodic'. For gen8+
		 * we want to to consider that the HW automatically writes some
		 * non periodic reports (e.g. on context switch) which might
		 * lead to more successful read()s than expected due to
		 * periodic sampling and we don't want these extra reads to
		 * cause the test to fail...
		 */
		if (intel_gen(devid) >= 8) {
			for (int offset = 0; offset < ret; offset += header->size) {
				header = (void *)(buf + offset);

				if (header->type == DRM_I915_PERF_RECORD_SAMPLE) {
					uint32_t *report = (void *)(header + 1);

					if (oa_report_is_periodic(oa_exponent,
								  report))
						timer_report_read = true;
					else
						non_timer_report_read = true;
				}
			}
		}

		if (non_timer_report_read && !timer_report_read)
			n_extra_iterations++;

		n++;
	}

	times(&end_times);

	/* Using nanosecond units is fairly silly here, given the tick in-
	 * precision - ah well, it's consistent with the get_time() units.
	 */
	user_ns = (end_times.tms_utime - start_times.tms_utime) * tick_ns;
	kernel_ns = (end_times.tms_stime - start_times.tms_stime) * tick_ns;

	igt_debug("%d blocking reads during test with %"PRIu64" Hz OA sampling (expect no more than %d)\n",
		  n, NSEC_PER_SEC / oa_period, max_iterations);
	igt_debug("%d extra iterations seen, not related to periodic sampling (e.g. context switches)\n",
		  n_extra_iterations);
	igt_debug("time in userspace = %"PRIu64"ns (+-%dns) (start utime = %d, end = %d)\n",
		  user_ns, (int)tick_ns,
		  (int)start_times.tms_utime, (int)end_times.tms_utime);
	igt_debug("time in kernelspace = %"PRIu64"ns (+-%dns) (start stime = %d, end = %d)\n",
		  kernel_ns, (int)tick_ns,
		  (int)start_times.tms_stime, (int)end_times.tms_stime);

	/* With completely broken blocking (but also not returning an error) we
	 * could end up with an open loop,
	 */
	igt_assert(n <= (max_iterations + n_extra_iterations));

	/* Make sure the driver is reporting new samples with a reasonably
	 * low latency...
	 */
	igt_assert(n > (min_iterations + n_extra_iterations));

	if (set_kernel_hrtimer) {
		max_expected_kernel_ns = kernel_hrtimer * (test_duration_ns / 100)
					 /(5 * 1000 * 1000);
	} else {
		max_expected_kernel_ns = test_duration_ns / 100;
	}
	igt_assert(kernel_ns <= max_expected_kernel_ns);

	__perf_close(stream_fd);
}

static void
test_polling(uint64_t requested_oa_period, bool set_kernel_hrtimer, uint64_t kernel_hrtimer)
{
	int oa_exponent = max_oa_exponent_for_period_lte(requested_oa_period);
	uint64_t oa_period = oa_exponent_to_ns(oa_exponent);
	uint64_t properties[] = {
		/* Include OA reports in samples */
		DRM_I915_PERF_PROP_SAMPLE_OA, true,

		/* OA unit configuration */
		DRM_I915_PERF_PROP_OA_METRICS_SET, test_set->perf_oa_metrics_set,
		DRM_I915_PERF_PROP_OA_FORMAT, test_set->perf_oa_format,
		DRM_I915_PERF_PROP_OA_EXPONENT, oa_exponent,

		/* Kernel configuration (optional) */
		DRM_I915_PERF_PROP_POLL_OA_PERIOD, kernel_hrtimer,
	};
	struct drm_i915_perf_open_param param = {
		.flags = I915_PERF_FLAG_FD_CLOEXEC |
			I915_PERF_FLAG_DISABLED |
			I915_PERF_FLAG_FD_NONBLOCK,
		.num_properties = set_kernel_hrtimer ?
				  NUM_PROPERTIES(properties) :
				  NUM_PROPERTIES(properties) - 1,
		.properties_ptr = to_user_pointer(properties),
	};
	uint8_t buf[1024 * 1024];
	struct tms start_times;
	struct tms end_times;
	int64_t user_ns, kernel_ns;
	int64_t tick_ns = 1000000000 / sysconf(_SC_CLK_TCK);
	int64_t test_duration_ns = tick_ns * 1000;

	int max_iterations = (test_duration_ns / oa_period) + 2;
	int n_extra_iterations = 0;

	/* It's a bit tricky to put a lower limit here, but we expect a
	 * relatively low latency for seeing reports.
	 *
	 * We assume a maximum latency of kernel_hrtimer + some margin
	 * to deliver a POLLIN and read() after a new sample is
	 * written (40ms + hrtimer + margin per iteration) considering
	 * the knowledge that that the driver uses a 200Hz hrtimer
	 * (5ms period) to check for data and giving some time to
	 * read().
	 */
	int min_iterations = (test_duration_ns / (oa_period + (kernel_hrtimer + kernel_hrtimer / 5)));
	int64_t start, end;
	int n = 0;
	uint64_t max_expected_kernel_ns;

	stream_fd = __perf_open(drm_fd, &param, true /* prevent_pm */);

	times(&start_times);

	igt_debug("tick length = %dns, oa period = %s, "
		  "test duration = %"PRIu64"ns, min iter. = %d, max iter. = %d\n",
		  (int)tick_ns, pretty_print_oa_period(oa_period), test_duration_ns,
		  min_iterations, max_iterations);

	/* In the loop we perform blocking polls while the HW is sampling at
	 * ~25Hz, with the expectation that we spend most of our time blocked
	 * in the kernel, and shouldn't be burning cpu cycles in the kernel in
	 * association with this process (verified by looking at stime before
	 * and after loop).
	 *
	 * We're looking to assert that less than 1% of the test duration is
	 * spent in the kernel dealing with polling and read()ing.
	 *
	 * The test runs for a relatively long time considering the very low
	 * resolution of stime in ticks of typically 10 milliseconds. Since we
	 * don't know the fractional part of tick values we read from userspace
	 * so our minimum threshold needs to be >= one tick since any
	 * measurement might really be +- tick_ns (assuming we effectively get
	 * floor(real_stime)).
	 *
	 * We Loop for 1000 x tick_ns so one tick corresponds to 0.1%
	 *
	 * Also enable the stream just before poll/read to minimize
	 * the error delta.
	 */
	start = get_time();
	do_ioctl(stream_fd, I915_PERF_IOCTL_ENABLE, 0);
	for (/* nop */; ((end = get_time()) - start) < test_duration_ns; /* nop */) {
		struct pollfd pollfd = { .fd = stream_fd, .events = POLLIN };
		struct drm_i915_perf_record_header *header;
		bool timer_report_read = false;
		bool non_timer_report_read = false;
		int ret;

		while ((ret = poll(&pollfd, 1, -1)) < 0 &&
		       errno == EINTR)
			;
		igt_assert_eq(ret, 1);
		igt_assert(pollfd.revents & POLLIN);

		while ((ret = read(stream_fd, buf, sizeof(buf))) < 0 &&
		       errno == EINTR)
			;

		/* Don't expect to see EAGAIN if we've had a POLLIN event
		 *
		 * XXX: actually this is technically overly strict since we do
		 * knowingly allow false positive POLLIN events. At least in
		 * the future when supporting context filtering of metrics for
		 * Gen8+ handled in the kernel then POLLIN events may be
		 * delivered when we know there are pending reports to process
		 * but before we've done any filtering to know for certain that
		 * any reports are destined to be copied to userspace.
		 *
		 * Still, for now it's a reasonable sanity check.
		 */
		if (ret < 0)
			igt_debug("Unexpected error when reading after poll = %d\n", errno);
		igt_assert_neq(ret, -1);

		/* For Haswell reports don't contain a well defined reason
		 * field we so assume all reports to be 'periodic'. For gen8+
		 * we want to to consider that the HW automatically writes some
		 * non periodic reports (e.g. on context switch) which might
		 * lead to more successful read()s than expected due to
		 * periodic sampling and we don't want these extra reads to
		 * cause the test to fail...
		 */
		if (intel_gen(devid) >= 8) {
			for (int offset = 0; offset < ret; offset += header->size) {
				header = (void *)(buf + offset);

				if (header->type == DRM_I915_PERF_RECORD_SAMPLE) {
					uint32_t *report = (void *)(header + 1);

					if (oa_report_is_periodic(oa_exponent, report))
						timer_report_read = true;
					else
						non_timer_report_read = true;
				}
			}
		}

		if (non_timer_report_read && !timer_report_read)
			n_extra_iterations++;

		/* At this point, after consuming pending reports (and hoping
		 * the scheduler hasn't stopped us for too long we now
		 * expect EAGAIN on read.
		 */
		while ((ret = read(stream_fd, buf, sizeof(buf))) < 0 &&
		       errno == EINTR)
			;
		igt_assert_eq(ret, -1);
		igt_assert_eq(errno, EAGAIN);

		n++;
	}

	times(&end_times);

	/* Using nanosecond units is fairly silly here, given the tick in-
	 * precision - ah well, it's consistent with the get_time() units.
	 */
	user_ns = (end_times.tms_utime - start_times.tms_utime) * tick_ns;
	kernel_ns = (end_times.tms_stime - start_times.tms_stime) * tick_ns;

	igt_debug("%d non-blocking reads during test with %"PRIu64" Hz OA sampling (expect no more than %d)\n",
		  n, NSEC_PER_SEC / oa_period, max_iterations);
	igt_debug("%d extra iterations seen, not related to periodic sampling (e.g. context switches)\n",
		  n_extra_iterations);
	igt_debug("time in userspace = %"PRIu64"ns (+-%dns) (start utime = %d, end = %d)\n",
		  user_ns, (int)tick_ns,
		  (int)start_times.tms_utime, (int)end_times.tms_utime);
	igt_debug("time in kernelspace = %"PRIu64"ns (+-%dns) (start stime = %d, end = %d)\n",
		  kernel_ns, (int)tick_ns,
		  (int)start_times.tms_stime, (int)end_times.tms_stime);

	/* With completely broken blocking while polling (but still somehow
	 * reporting a POLLIN event) we could end up with an open loop.
	 */
	igt_assert(n <= (max_iterations + n_extra_iterations));

	/* Make sure the driver is reporting new samples with a reasonably
	 * low latency...
	 */
	igt_assert(n > (min_iterations + n_extra_iterations));

	if (set_kernel_hrtimer) {
		max_expected_kernel_ns = kernel_hrtimer * (test_duration_ns / 100)
					 /(5 * 1000 * 1000);
	} else {
		max_expected_kernel_ns = test_duration_ns / 100;
	}
	igt_assert(kernel_ns <= max_expected_kernel_ns);

	__perf_close(stream_fd);
}

static void test_polling_small_buf(void)
{
	int oa_exponent = max_oa_exponent_for_period_lte(40 * 1000); /* 40us */
	uint64_t properties[] = {
		/* Include OA reports in samples */
		DRM_I915_PERF_PROP_SAMPLE_OA, true,

		/* OA unit configuration */
		DRM_I915_PERF_PROP_OA_METRICS_SET, test_set->perf_oa_metrics_set,
		DRM_I915_PERF_PROP_OA_FORMAT, test_set->perf_oa_format,
		DRM_I915_PERF_PROP_OA_EXPONENT, oa_exponent,
	};
	struct drm_i915_perf_open_param param = {
		.flags = I915_PERF_FLAG_FD_CLOEXEC |
			I915_PERF_FLAG_DISABLED |
			I915_PERF_FLAG_FD_NONBLOCK,
		.num_properties = NUM_PROPERTIES(properties),
		.properties_ptr = to_user_pointer(properties),
	};
	uint32_t test_duration = 80 * 1000 * 1000;
	int sample_size = (sizeof(struct drm_i915_perf_record_header) +
			   get_oa_format(test_set->perf_oa_format).size);
	int n_expected_reports = test_duration / oa_exponent_to_ns(oa_exponent);
	int n_expect_read_bytes = n_expected_reports * sample_size;
	struct timespec ts = {};
	int n_bytes_read = 0;
	uint32_t n_polls = 0;

	stream_fd = __perf_open(drm_fd, &param, true /* prevent_pm */);
	do_ioctl(stream_fd, I915_PERF_IOCTL_ENABLE, 0);

	while (igt_nsec_elapsed(&ts) < test_duration) {
		struct pollfd pollfd = { .fd = stream_fd, .events = POLLIN };

		ppoll(&pollfd, 1, NULL, NULL);
		if (pollfd.revents & POLLIN) {
			uint8_t buf[1024];
			int ret;

			ret = read(stream_fd, buf, sizeof(buf));
			if (ret >= 0)
				n_bytes_read += ret;
		}

		n_polls++;
	}

	igt_info("Read %d expected %d (%.2f%% of the expected number), polls=%u\n",
		 n_bytes_read, n_expect_read_bytes,
		 n_bytes_read * 100.0f / n_expect_read_bytes,
		 n_polls);

	__perf_close(stream_fd);

	igt_assert(abs(n_expect_read_bytes - n_bytes_read) <
		   0.20 * n_expect_read_bytes);
}

static int
num_valid_reports_captured(struct drm_i915_perf_open_param *param,
			   int64_t *duration_ns)
{
	uint8_t buf[1024 * 1024];
	int64_t start, end;
	int num_reports = 0;

	igt_debug("Expected duration = %"PRId64"\n", *duration_ns);

	stream_fd = __perf_open(drm_fd, param, true);

	start = get_time();
	do_ioctl(stream_fd, I915_PERF_IOCTL_ENABLE, 0);
	for (/* nop */; ((end = get_time()) - start) < *duration_ns; /* nop */) {
		struct drm_i915_perf_record_header *header;
		int ret;

		while ((ret = read(stream_fd, buf, sizeof(buf))) < 0 &&
		       errno == EINTR)
			;

		igt_assert(ret > 0);

		for (int offset = 0; offset < ret; offset += header->size) {
			header = (void *)(buf + offset);

			if (header->type == DRM_I915_PERF_RECORD_SAMPLE) {
				uint32_t *report = (void *)(header + 1);

				if ((report[0] >> OAREPORT_REASON_SHIFT) &
				    OAREPORT_REASON_TIMER)
					num_reports++;
			}
		}
	}
	__perf_close(stream_fd);

	*duration_ns = end - start;

	igt_debug("Actual duration = %"PRIu64"\n", *duration_ns);

	return num_reports;
}

static void
gen12_test_oa_tlb_invalidate(void)
{
	int oa_exponent = max_oa_exponent_for_period_lte(30000000);
	uint64_t properties[] = {
		DRM_I915_PERF_PROP_SAMPLE_OA, true,

		DRM_I915_PERF_PROP_OA_METRICS_SET, test_set->perf_oa_metrics_set,
		DRM_I915_PERF_PROP_OA_FORMAT, test_set->perf_oa_format,
		DRM_I915_PERF_PROP_OA_EXPONENT, oa_exponent,
	};
	struct drm_i915_perf_open_param param = {
		.flags = I915_PERF_FLAG_FD_CLOEXEC |
			I915_PERF_FLAG_DISABLED,
		.num_properties = sizeof(properties) / 16,
		.properties_ptr = to_user_pointer(properties),
	};
	int num_reports1, num_reports2, num_expected_reports;
	int64_t duration;

	/* Capture reports for 5 seconds twice and then make sure you get around
	 * the same number of reports. In the case of failure, the number of
	 * reports will vary largely since the beginning of the OA buffer
	 * will have invalid entries.
	 */
	duration = 5LL * NSEC_PER_SEC;
	num_reports1 = num_valid_reports_captured(&param, &duration);
	num_expected_reports = duration / oa_exponent_to_ns(oa_exponent);
	igt_debug("expected num reports = %d\n", num_expected_reports);
	igt_debug("actual num reports = %d\n", num_reports1);
	igt_assert(num_reports1 > 0.95 * num_expected_reports);

	duration = 5LL * NSEC_PER_SEC;
	num_reports2 = num_valid_reports_captured(&param, &duration);
	num_expected_reports = duration / oa_exponent_to_ns(oa_exponent);
	igt_debug("expected num reports = %d\n", num_expected_reports);
	igt_debug("actual num reports = %d\n", num_reports2);
	igt_assert(num_reports2 > 0.95 * num_expected_reports);
}


static void
test_buffer_fill(void)
{
	/* ~5 micro second period */
	int oa_exponent = max_oa_exponent_for_period_lte(5000);
	uint64_t oa_period = oa_exponent_to_ns(oa_exponent);
	uint64_t properties[] = {
		/* Include OA reports in samples */
		DRM_I915_PERF_PROP_SAMPLE_OA, true,

		/* OA unit configuration */
		DRM_I915_PERF_PROP_OA_METRICS_SET, test_set->perf_oa_metrics_set,
		DRM_I915_PERF_PROP_OA_FORMAT, test_set->perf_oa_format,
		DRM_I915_PERF_PROP_OA_EXPONENT, oa_exponent,
	};
	struct drm_i915_perf_open_param param = {
		.flags = I915_PERF_FLAG_FD_CLOEXEC,
		.num_properties = sizeof(properties) / 16,
		.properties_ptr = to_user_pointer(properties),
	};
	struct drm_i915_perf_record_header *header;
	int buf_size = 65536 * (256 + sizeof(struct drm_i915_perf_record_header));
	uint8_t *buf = malloc(buf_size);
	int len;
	size_t oa_buf_size = MAX_OA_BUF_SIZE;
	size_t report_size = get_oa_format(test_set->perf_oa_format).size;
	int n_full_oa_reports = oa_buf_size / report_size;
	uint64_t fill_duration = n_full_oa_reports * oa_period;

	igt_assert(fill_duration < 1000000000);

	stream_fd = __perf_open(drm_fd, &param, true /* prevent_pm */);

	for (int i = 0; i < 5; i++) {
		bool overflow_seen;
		uint32_t n_periodic_reports;
		uint32_t first_timestamp = 0, last_timestamp = 0;
		uint32_t last_periodic_report[64];

		do_ioctl(stream_fd, I915_PERF_IOCTL_ENABLE, 0);

		nanosleep(&(struct timespec){ .tv_sec = 0,
					      .tv_nsec = fill_duration * 1.25 },
			  NULL);

		while ((len = read(stream_fd, buf, buf_size)) == -1 && errno == EINTR)
			;

		igt_assert_neq(len, -1);

		overflow_seen = false;
		for (int offset = 0; offset < len; offset += header->size) {
			header = (void *)(buf + offset);

			if (header->type == DRM_I915_PERF_RECORD_OA_BUFFER_LOST)
				overflow_seen = true;
		}

		igt_assert_eq(overflow_seen, true);

		do_ioctl(stream_fd, I915_PERF_IOCTL_DISABLE, 0);

		igt_debug("fill_duration = %"PRIu64"ns, oa_exponent = %u\n",
			  fill_duration, oa_exponent);

		do_ioctl(stream_fd, I915_PERF_IOCTL_ENABLE, 0);

		nanosleep(&(struct timespec){ .tv_sec = 0,
					.tv_nsec = fill_duration / 2 },
			NULL);

		n_periodic_reports = 0;

		/* Because of the race condition between notification of new
		 * reports and reports landing in memory, we need to rely on
		 * timestamps to figure whether we've read enough of them.
		 */
		while (((last_timestamp - first_timestamp) * oa_period) < (fill_duration / 2)) {

			igt_debug("dts=%u elapsed=%"PRIu64" duration=%"PRIu64"\n",
				  last_timestamp - first_timestamp,
				  (last_timestamp - first_timestamp) * oa_period,
				  fill_duration / 2);

			while ((len = read(stream_fd, buf, buf_size)) == -1 && errno == EINTR)
				;

			igt_assert_neq(len, -1);

			for (int offset = 0; offset < len; offset += header->size) {
				uint32_t *report;

				header = (void *) (buf + offset);
				report = (void *) (header + 1);

				switch (header->type) {
				case DRM_I915_PERF_RECORD_OA_REPORT_LOST:
					igt_debug("report loss, trying again\n");
					break;
				case DRM_I915_PERF_RECORD_SAMPLE:
					igt_debug(" > report ts=%u"
						  " ts_delta_last_periodic=%8u is_timer=%i ctx_id=%8x nb_periodic=%u\n",
						  report[1],
						  n_periodic_reports > 0 ? report[1] - last_periodic_report[1] : 0,
						  oa_report_is_periodic(oa_exponent, report),
						  oa_report_get_ctx_id(report),
						  n_periodic_reports);

					if (first_timestamp == 0)
						first_timestamp = report[1];
					last_timestamp = report[1];

					if (oa_report_is_periodic(oa_exponent, report)) {
						memcpy(last_periodic_report, report,
						       sizeof(last_periodic_report));
						n_periodic_reports++;
					}
					break;
				case DRM_I915_PERF_RECORD_OA_BUFFER_LOST:
					igt_assert(!"unexpected overflow");
					break;
				}
			}
		}

		do_ioctl(stream_fd, I915_PERF_IOCTL_DISABLE, 0);

		igt_debug("%f < %zu < %f\n",
			  report_size * n_full_oa_reports * 0.45,
			  n_periodic_reports * report_size,
			  report_size * n_full_oa_reports * 0.55);

		igt_assert(n_periodic_reports * report_size >
			   report_size * n_full_oa_reports * 0.45);
		igt_assert(n_periodic_reports * report_size <
			   report_size * n_full_oa_reports * 0.55);
	}

	free(buf);

	__perf_close(stream_fd);
}

static void
test_enable_disable(void)
{
	/* ~5 micro second period */
	int oa_exponent = max_oa_exponent_for_period_lte(5000);
	uint64_t oa_period = oa_exponent_to_ns(oa_exponent);
	uint64_t properties[] = {
		/* Include OA reports in samples */
		DRM_I915_PERF_PROP_SAMPLE_OA, true,

		/* OA unit configuration */
		DRM_I915_PERF_PROP_OA_METRICS_SET, test_set->perf_oa_metrics_set,
		DRM_I915_PERF_PROP_OA_FORMAT, test_set->perf_oa_format,
		DRM_I915_PERF_PROP_OA_EXPONENT, oa_exponent,
	};
	struct drm_i915_perf_open_param param = {
		.flags = I915_PERF_FLAG_FD_CLOEXEC |
			 I915_PERF_FLAG_DISABLED, /* Verify we start disabled */
		.num_properties = sizeof(properties) / 16,
		.properties_ptr = to_user_pointer(properties),
	};
	int buf_size = 65536 * (256 + sizeof(struct drm_i915_perf_record_header));
	uint8_t *buf = malloc(buf_size);
	size_t oa_buf_size = MAX_OA_BUF_SIZE;
	size_t report_size = get_oa_format(test_set->perf_oa_format).size;
	int n_full_oa_reports = oa_buf_size / report_size;
	uint64_t fill_duration = n_full_oa_reports * oa_period;

	load_helper_init();
	load_helper_run(HIGH);

	stream_fd = __perf_open(drm_fd, &param, true /* prevent_pm */);

	for (int i = 0; i < 5; i++) {
		int len;
		uint32_t n_periodic_reports;
		struct drm_i915_perf_record_header *header;
		uint32_t first_timestamp = 0, last_timestamp = 0;
		uint32_t last_periodic_report[64];

		/* Giving enough time for an overflow might help catch whether
		 * the OA unit has been enabled even if the driver might at
		 * least avoid copying reports while disabled.
		 */
		nanosleep(&(struct timespec){ .tv_sec = 0,
					      .tv_nsec = fill_duration * 1.25 },
			  NULL);

		while ((len = read(stream_fd, buf, buf_size)) == -1 && errno == EINTR)
			;

		igt_assert_eq(len, -1);
		igt_assert_eq(errno, EIO);

		do_ioctl(stream_fd, I915_PERF_IOCTL_ENABLE, 0);

		nanosleep(&(struct timespec){ .tv_sec = 0,
					      .tv_nsec = fill_duration / 2 },
			NULL);

		n_periodic_reports = 0;

		/* Because of the race condition between notification of new
		 * reports and reports landing in memory, we need to rely on
		 * timestamps to figure whether we've read enough of them.
		 */
		while (((last_timestamp - first_timestamp) * oa_period) < (fill_duration / 2)) {

			while ((len = read(stream_fd, buf, buf_size)) == -1 && errno == EINTR)
				;

			igt_assert_neq(len, -1);

			for (int offset = 0; offset < len; offset += header->size) {
				uint32_t *report;

				header = (void *) (buf + offset);
				report = (void *) (header + 1);

				switch (header->type) {
				case DRM_I915_PERF_RECORD_OA_REPORT_LOST:
					break;
				case DRM_I915_PERF_RECORD_SAMPLE:
					if (first_timestamp == 0)
						first_timestamp = report[1];
					last_timestamp = report[1];

					igt_debug(" > report ts=%8x"
						  " ts_delta_last_periodic=%s%8u"
						  " is_timer=%i ctx_id=0x%8x\n",
						  report[1],
						  oa_report_is_periodic(oa_exponent, report) ? " " : "*",
						  n_periodic_reports > 0 ? (report[1] - last_periodic_report[1]) : 0,
						  oa_report_is_periodic(oa_exponent, report),
						  oa_report_get_ctx_id(report));

					if (oa_report_is_periodic(oa_exponent, report)) {
						memcpy(last_periodic_report, report,
						       sizeof(last_periodic_report));

						/* We want to measure only the
						 * periodic reports, ctx-switch
						 * might inflate the content of
						 * the buffer and skew or
						 * measurement.
						 */
						n_periodic_reports++;
					}
					break;
				case DRM_I915_PERF_RECORD_OA_BUFFER_LOST:
					igt_assert(!"unexpected overflow");
					break;
				}
			}

		}

		do_ioctl(stream_fd, I915_PERF_IOCTL_DISABLE, 0);

		igt_debug("%f < %zu < %f\n",
			  report_size * n_full_oa_reports * 0.45,
			  n_periodic_reports * report_size,
			  report_size * n_full_oa_reports * 0.55);

		igt_assert((n_periodic_reports * report_size) >
			   (report_size * n_full_oa_reports * 0.45));
		igt_assert((n_periodic_reports * report_size) <
			   report_size * n_full_oa_reports * 0.55);


		/* It's considered an error to read a stream while it's disabled
		 * since it would block indefinitely...
		 */
		len = read(stream_fd, buf, buf_size);

		igt_assert_eq(len, -1);
		igt_assert_eq(errno, EIO);
	}

	free(buf);

	__perf_close(stream_fd);

	load_helper_stop();
	load_helper_fini();
}

static void
test_short_reads(void)
{
	int oa_exponent = max_oa_exponent_for_period_lte(5000);
	uint64_t properties[] = {
		/* Include OA reports in samples */
		DRM_I915_PERF_PROP_SAMPLE_OA, true,

		/* OA unit configuration */
		DRM_I915_PERF_PROP_OA_METRICS_SET, test_set->perf_oa_metrics_set,
		DRM_I915_PERF_PROP_OA_FORMAT, test_set->perf_oa_format,
		DRM_I915_PERF_PROP_OA_EXPONENT, oa_exponent,
	};
	struct drm_i915_perf_open_param param = {
		.flags = I915_PERF_FLAG_FD_CLOEXEC,
		.num_properties = sizeof(properties) / 16,
		.properties_ptr = to_user_pointer(properties),
	};
	size_t record_size = 256 + sizeof(struct drm_i915_perf_record_header);
	size_t page_size = sysconf(_SC_PAGE_SIZE);
	int zero_fd = open("/dev/zero", O_RDWR|O_CLOEXEC);
	uint8_t *pages = mmap(NULL, page_size * 2,
			      PROT_READ|PROT_WRITE, MAP_PRIVATE, zero_fd, 0);
	struct drm_i915_perf_record_header *header;
	int ret;

	igt_assert_neq(zero_fd, -1);
	close(zero_fd);
	zero_fd = -1;

	igt_assert(pages);

	ret = mprotect(pages + page_size, page_size, PROT_NONE);
	igt_assert_eq(ret, 0);

	stream_fd = __perf_open(drm_fd, &param, false);

	nanosleep(&(struct timespec){ .tv_sec = 0, .tv_nsec = 5000000 }, NULL);

	/* At this point there should be lots of pending reports to read */

	/* A read that can return at least one record should result in a short
	 * read not an EFAULT if the buffer is smaller than the requested read
	 * size...
	 *
	 * Expect to see a sample record here, but at least skip over any
	 * _RECORD_LOST notifications.
	 */
	do {
		header = (void *)(pages + page_size - record_size);
		ret = read(stream_fd,
			   header,
			   page_size);
		igt_assert(ret > 0);
	} while (header->type == DRM_I915_PERF_RECORD_OA_REPORT_LOST);

	igt_assert_eq(ret, record_size);

	/* A read that can't return a single record because it would result
	 * in a fault on buffer overrun should result in an EFAULT error...
	 */
	ret = read(stream_fd, pages + page_size - 16, page_size);
	igt_assert_eq(ret, -1);
	igt_assert_eq(errno, EFAULT);

	/* A read that can't return a single record because the buffer is too
	 * small should result in an ENOSPC error..
	 *
	 * Again, skip over _RECORD_LOST records (smaller than record_size/2)
	 */
	do {
		header = (void *)(pages + page_size - record_size / 2);
		ret = read(stream_fd,
			   header,
			   record_size / 2);
	} while (ret > 0 && header->type == DRM_I915_PERF_RECORD_OA_REPORT_LOST);

	igt_assert_eq(ret, -1);
	igt_assert_eq(errno, ENOSPC);

	__perf_close(stream_fd);

	munmap(pages, page_size * 2);
}

static void
test_non_sampling_read_error(void)
{
	uint64_t properties[] = {
		/* XXX: even without periodic sampling we have to
		 * specify at least one sample layout property...
		 */
		DRM_I915_PERF_PROP_SAMPLE_OA, true,

		/* OA unit configuration */
		DRM_I915_PERF_PROP_OA_METRICS_SET, test_set->perf_oa_metrics_set,
		DRM_I915_PERF_PROP_OA_FORMAT, test_set->perf_oa_format,

		/* XXX: no sampling exponent */
	};
	struct drm_i915_perf_open_param param = {
		.flags = I915_PERF_FLAG_FD_CLOEXEC,
		.num_properties = sizeof(properties) / 16,
		.properties_ptr = to_user_pointer(properties),
	};
	int ret;
	uint8_t buf[1024];

	stream_fd = __perf_open(drm_fd, &param, false);

	ret = read(stream_fd, buf, sizeof(buf));
	igt_assert_eq(ret, -1);
	igt_assert_eq(errno, EIO);

	__perf_close(stream_fd);
}

/* Check that attempts to read from a stream while it is disable will return
 * EIO instead of blocking indefinitely.
 */
static void
test_disabled_read_error(void)
{
	int oa_exponent = 5; /* 5 micro seconds */
	uint64_t properties[] = {
		/* XXX: even without periodic sampling we have to
		 * specify at least one sample layout property...
		 */
		DRM_I915_PERF_PROP_SAMPLE_OA, true,

		/* OA unit configuration */
		DRM_I915_PERF_PROP_OA_METRICS_SET, test_set->perf_oa_metrics_set,
		DRM_I915_PERF_PROP_OA_FORMAT, test_set->perf_oa_format,
		DRM_I915_PERF_PROP_OA_EXPONENT, oa_exponent,
	};
	struct drm_i915_perf_open_param param = {
		.flags = I915_PERF_FLAG_FD_CLOEXEC |
			 I915_PERF_FLAG_DISABLED, /* XXX: open disabled */
		.num_properties = sizeof(properties) / 16,
		.properties_ptr = to_user_pointer(properties),
	};
	uint32_t oa_report0[64];
	uint32_t oa_report1[64];
	uint32_t buf[128] = { 0 };
	int ret;

	stream_fd = __perf_open(drm_fd, &param, false);

	ret = read(stream_fd, buf, sizeof(buf));
	igt_assert_eq(ret, -1);
	igt_assert_eq(errno, EIO);

	__perf_close(stream_fd);


	param.flags &= ~I915_PERF_FLAG_DISABLED;
	stream_fd = __perf_open(drm_fd, &param, false);

	read_2_oa_reports(test_set->perf_oa_format,
			  oa_exponent,
			  oa_report0,
			  oa_report1,
			  false); /* not just timer reports */

	do_ioctl(stream_fd, I915_PERF_IOCTL_DISABLE, 0);

	ret = read(stream_fd, buf, sizeof(buf));
	igt_assert_eq(ret, -1);
	igt_assert_eq(errno, EIO);

	do_ioctl(stream_fd, I915_PERF_IOCTL_ENABLE, 0);

	read_2_oa_reports(test_set->perf_oa_format,
			  oa_exponent,
			  oa_report0,
			  oa_report1,
			  false); /* not just timer reports */

	__perf_close(stream_fd);
}

static void
gen12_test_mi_rpc(void)
{
	uint64_t properties[] = {
		/* On Gen12, MI RPC uses OAR. OAR is configured only for the
		 * render context that wants to measure the performance. Hence a
		 * context must be specified in the gen12 MI RPC when compared
		 * to previous gens.
		 *
		 * Have a random value here for the context id, but initialize
		 * it once you figure out the context ID for the work to be
		 * measured
		 */
		DRM_I915_PERF_PROP_CTX_HANDLE, UINT64_MAX,

		/* OA unit configuration:
		 * DRM_I915_PERF_PROP_SAMPLE_OA is no longer required for Gen12
		 * because the OAR unit increments counters only for the
		 * relevant context. No other parameters are needed since we do
		 * not rely on the OA buffer anymore to normalize the counter
		 * values.
		 */
		DRM_I915_PERF_PROP_OA_METRICS_SET, test_set->perf_oa_metrics_set,
		DRM_I915_PERF_PROP_OA_FORMAT, test_set->perf_oa_format,
	};
	struct drm_i915_perf_open_param param = {
		.flags = I915_PERF_FLAG_FD_CLOEXEC,
		.num_properties = ARRAY_SIZE(properties) / 2,
		.properties_ptr = to_user_pointer(properties),
	};
	drm_intel_bo *bo;
	drm_intel_bufmgr *bufmgr;
	drm_intel_context *context;
	struct intel_batchbuffer *batch;
#define INVALID_CTX_ID 0xffffffff
	uint32_t ctx_id = INVALID_CTX_ID;
	uint32_t *report32;
	int ret;
	size_t format_size_32;
	struct oa_format format = get_oa_format(test_set->perf_oa_format);

	/* Ensure perf_stream_paranoid is set to 1 by default */
	write_u64_file("/proc/sys/dev/i915/perf_stream_paranoid", 1);

	bufmgr = drm_intel_bufmgr_gem_init(drm_fd, 4096);
	igt_assert(bufmgr);

	drm_intel_bufmgr_gem_enable_reuse(bufmgr);

	context = drm_intel_gem_context_create(bufmgr);
	igt_assert(context);

	ret = drm_intel_gem_context_get_id(context, &ctx_id);
	igt_assert_eq(ret, 0);
	igt_assert_neq(ctx_id, INVALID_CTX_ID);
	properties[1] = ctx_id;

	batch = intel_batchbuffer_alloc(bufmgr, devid);
	bo = drm_intel_bo_alloc(bufmgr, "mi_rpc dest bo", 4096, 64);

	ret = drm_intel_bo_map(bo, true);
	igt_assert_eq(ret, 0);
	memset(bo->virtual, 0x80, 4096);
	drm_intel_bo_unmap(bo);

	stream_fd = __perf_open(drm_fd, &param, false);

#define REPORT_ID 0xdeadbeef
#define REPORT_OFFSET 0
	emit_report_perf_count(batch,
			       bo,
			       REPORT_OFFSET,
			       REPORT_ID);
	intel_batchbuffer_flush_with_context(batch, context);

	ret = drm_intel_bo_map(bo, false);
	igt_assert_eq(ret, 0);

	report32 = bo->virtual;
	format_size_32 = format.size >> 2;
	dump_report(report32, format_size_32, "mi-rpc");

	/* Sanity check reports
	 * reportX_32[0]: report id passed with mi-rpc
	 * reportX_32[1]: timestamp. NOTE: wraps around in ~6 minutes.
	 *
	 * reportX_32[format.b_off]: check if the entire report was filled.
	 * B0 counter falls in the last 64 bytes of this report format.
	 * Since reports are filled in 64 byte blocks, we should be able to
	 * assure that the report was filled by checking the B0 counter. B0
	 * counter is defined to be zero, so we can easily validate it.
	 *
	 * reportX_32[format_size_32]: outside report, make sure only the report
	 * size amount of data was written.
	 */
	igt_assert_eq(report32[0], REPORT_ID);
	igt_assert_neq(report32[1], 0);
	igt_assert_neq(report32[format.b_off >> 2], 0x80808080);
	igt_assert_eq(report32[format_size_32], 0x80808080);

	drm_intel_bo_unmap(bo);
	drm_intel_bo_unreference(bo);
	intel_batchbuffer_free(batch);
	drm_intel_gem_context_destroy(context);
	drm_intel_bufmgr_destroy(bufmgr);
	__perf_close(stream_fd);
}

static void
test_mi_rpc(void)
{
	uint64_t properties[] = {
		/* Note: we have to specify at least one sample property even
		 * though we aren't interested in samples in this case.
		 */
		DRM_I915_PERF_PROP_SAMPLE_OA, true,

		/* OA unit configuration */
		DRM_I915_PERF_PROP_OA_METRICS_SET, test_set->perf_oa_metrics_set,
		DRM_I915_PERF_PROP_OA_FORMAT, test_set->perf_oa_format,

		/* Note: no OA exponent specified in this case */
	};
	struct drm_i915_perf_open_param param = {
		.flags = I915_PERF_FLAG_FD_CLOEXEC,
		.num_properties = sizeof(properties) / 16,
		.properties_ptr = to_user_pointer(properties),
	};
	drm_intel_bufmgr *bufmgr = drm_intel_bufmgr_gem_init(drm_fd, 4096);
	drm_intel_context *context;
	struct intel_batchbuffer *batch;
	drm_intel_bo *bo;
	uint32_t *report32;
	int ret;

	stream_fd = __perf_open(drm_fd, &param, false);

	drm_intel_bufmgr_gem_enable_reuse(bufmgr);

	context = drm_intel_gem_context_create(bufmgr);
	igt_assert(context);

	batch = intel_batchbuffer_alloc(bufmgr, devid);

	bo = drm_intel_bo_alloc(bufmgr, "mi_rpc dest bo", 4096, 64);

	ret = drm_intel_bo_map(bo, true);
	igt_assert_eq(ret, 0);

	memset(bo->virtual, 0x80, 4096);
	drm_intel_bo_unmap(bo);

	emit_report_perf_count(batch,
			       bo, /* dst */
			       0, /* dst offset in bytes */
			       0xdeadbeef); /* report ID */

	intel_batchbuffer_flush_with_context(batch, context);

	ret = drm_intel_bo_map(bo, false /* write enable */);
	igt_assert_eq(ret, 0);

	report32 = bo->virtual;
	dump_report(report32, 64, "mi-rpc");
	igt_assert_eq(report32[0], 0xdeadbeef); /* report ID */
	igt_assert_neq(report32[1], 0); /* timestamp */

	igt_assert_neq(report32[63], 0x80808080); /* end of report */
	igt_assert_eq(report32[64], 0x80808080); /* after 256 byte report */

	drm_intel_bo_unmap(bo);
	drm_intel_bo_unreference(bo);
	intel_batchbuffer_free(batch);
	drm_intel_gem_context_destroy(context);
	drm_intel_bufmgr_destroy(bufmgr);
	__perf_close(stream_fd);
}

static void
emit_stall_timestamp_and_rpc(struct intel_batchbuffer *batch,
			     drm_intel_bo *dst,
			     int timestamp_offset,
			     int report_dst_offset,
			     uint32_t report_id)
{
	uint32_t pipe_ctl_flags = (PIPE_CONTROL_CS_STALL |
				   PIPE_CONTROL_RENDER_TARGET_FLUSH |
				   PIPE_CONTROL_WRITE_TIMESTAMP);

	if (intel_gen(devid) >= 8) {
		BEGIN_BATCH(5, 1);
		OUT_BATCH(GFX_OP_PIPE_CONTROL | (6 - 2));
		OUT_BATCH(pipe_ctl_flags);
		OUT_RELOC(dst, I915_GEM_DOMAIN_INSTRUCTION, I915_GEM_DOMAIN_INSTRUCTION,
			  timestamp_offset);
		OUT_BATCH(0); /* imm lower */
		OUT_BATCH(0); /* imm upper */
		ADVANCE_BATCH();
	} else {
		BEGIN_BATCH(5, 1);
		OUT_BATCH(GFX_OP_PIPE_CONTROL | (5 - 2));
		OUT_BATCH(pipe_ctl_flags);
		OUT_RELOC(dst, I915_GEM_DOMAIN_INSTRUCTION, I915_GEM_DOMAIN_INSTRUCTION,
			  timestamp_offset);
		OUT_BATCH(0); /* imm lower */
		OUT_BATCH(0); /* imm upper */
		ADVANCE_BATCH();
	}

	emit_report_perf_count(batch, dst, report_dst_offset, report_id);
}

/* Tests the INTEL_performance_query use case where an unprivileged process
 * should be able to configure the OA unit for per-context metrics (for a
 * context associated with that process' drm file descriptor) and the counters
 * should only relate to that specific context.
 *
 * Unfortunately only Haswell limits the progression of OA counters for a
 * single context and so this unit test is Haswell specific. For Gen8+ although
 * reports read via i915 perf can be filtered for a single context the counters
 * themselves always progress as global/system-wide counters affected by all
 * contexts.
 */
static void
hsw_test_single_ctx_counters(void)
{
	uint64_t properties[] = {
		DRM_I915_PERF_PROP_CTX_HANDLE, UINT64_MAX, /* updated below */

		/* Note: we have to specify at least one sample property even
		 * though we aren't interested in samples in this case
		 */
		DRM_I915_PERF_PROP_SAMPLE_OA, true,

		/* OA unit configuration */
		DRM_I915_PERF_PROP_OA_METRICS_SET, test_set->perf_oa_metrics_set,
		DRM_I915_PERF_PROP_OA_FORMAT, test_set->perf_oa_format,

		/* Note: no OA exponent specified in this case */
	};
	struct drm_i915_perf_open_param param = {
		.flags = I915_PERF_FLAG_FD_CLOEXEC,
		.num_properties = sizeof(properties) / 16,
		.properties_ptr = to_user_pointer(properties),
	};

	/* should be default, but just to be sure... */
	write_u64_file("/proc/sys/dev/i915/perf_stream_paranoid", 1);

	igt_fork(child, 1) {
		drm_intel_bufmgr *bufmgr;
		drm_intel_context *context0, *context1;
		struct intel_batchbuffer *batch;
		struct igt_buf src[3], dst[3];
		drm_intel_bo *bo;
		uint32_t *report0_32, *report1_32;
		uint64_t timestamp0_64, timestamp1_64;
		uint32_t delta_ts64, delta_oa32;
		uint64_t delta_ts64_ns, delta_oa32_ns;
		uint32_t delta_delta;
		int n_samples_written;
		int width = 800;
		int height = 600;
		uint32_t ctx_id = 0xffffffff; /* invalid id */
		int ret;

		igt_drop_root();

		bufmgr = drm_intel_bufmgr_gem_init(drm_fd, 4096);
		drm_intel_bufmgr_gem_enable_reuse(bufmgr);

		for (int i = 0; i < ARRAY_SIZE(src); i++) {
			scratch_buf_init(bufmgr, &src[i], width, height, 0xff0000ff);
			scratch_buf_init(bufmgr, &dst[i], width, height, 0x00ff00ff);
		}

		batch = intel_batchbuffer_alloc(bufmgr, devid);

		context0 = drm_intel_gem_context_create(bufmgr);
		igt_assert(context0);

		context1 = drm_intel_gem_context_create(bufmgr);
		igt_assert(context1);

		igt_debug("submitting warm up render_copy\n");

		/* Submit some early, unmeasured, work to the context we want
		 * to measure to try and catch issues with i915-perf
		 * initializing the HW context ID for filtering.
		 *
		 * We do this because i915-perf single context filtering had
		 * previously only relied on a hook into context pinning to
		 * initialize the HW context ID, instead of also trying to
		 * determine the HW ID while opening the stream, in case it
		 * has already been pinned.
		 *
		 * This wasn't noticed by the previous unit test because we
		 * were opening the stream while the context hadn't been
		 * touched or pinned yet and so it worked out correctly to wait
		 * for the pinning hook.
		 *
		 * Now a buggy version of i915-perf will fail to measure
		 * anything for context0 once this initial render_copy() ends
		 * up pinning the context since there won't ever be a pinning
		 * hook callback.
		 */
		render_copy(batch,
			    context0,
			    &src[0], 0, 0, width, height,
			    &dst[0], 0, 0);

		ret = drm_intel_gem_context_get_id(context0, &ctx_id);
		igt_assert_eq(ret, 0);
		igt_assert_neq(ctx_id, 0xffffffff);
		properties[1] = ctx_id;

		intel_batchbuffer_flush_with_context(batch, context0);

		scratch_buf_memset(src[0].bo, width, height, 0xff0000ff);
		scratch_buf_memset(dst[0].bo, width, height, 0x00ff00ff);

		igt_debug("opening i915-perf stream\n");
		stream_fd = __perf_open(drm_fd, &param, false);

		bo = drm_intel_bo_alloc(bufmgr, "mi_rpc dest bo", 4096, 64);

		ret = drm_intel_bo_map(bo, true /* write enable */);
		igt_assert_eq(ret, 0);

		memset(bo->virtual, 0x80, 4096);
		drm_intel_bo_unmap(bo);

		emit_stall_timestamp_and_rpc(batch,
					     bo,
					     512 /* timestamp offset */,
					     0, /* report dst offset */
					     0xdeadbeef); /* report id */

		/* Explicitly flush here (even though the render_copy() call
		 * will itself flush before/after the copy) to clarify that
		 * that the PIPE_CONTROL + MI_RPC commands will be in a
		 * separate batch from the copy.
		 */
		intel_batchbuffer_flush_with_context(batch, context0);

		render_copy(batch,
			    context0,
			    &src[0], 0, 0, width, height,
			    &dst[0], 0, 0);

		/* Another redundant flush to clarify batch bo is free to reuse */
		intel_batchbuffer_flush_with_context(batch, context0);

		/* submit two copies on the other context to avoid a false
		 * positive in case the driver somehow ended up filtering for
		 * context1
		 */
		render_copy(batch,
			    context1,
			    &src[1], 0, 0, width, height,
			    &dst[1], 0, 0);

		render_copy(batch,
			    context1,
			    &src[2], 0, 0, width, height,
			    &dst[2], 0, 0);

		/* And another */
		intel_batchbuffer_flush_with_context(batch, context1);

		emit_stall_timestamp_and_rpc(batch,
					     bo,
					     520 /* timestamp offset */,
					     256, /* report dst offset */
					     0xbeefbeef); /* report id */

		intel_batchbuffer_flush_with_context(batch, context0);

		ret = drm_intel_bo_map(bo, false /* write enable */);
		igt_assert_eq(ret, 0);

		report0_32 = bo->virtual;
		igt_assert_eq(report0_32[0], 0xdeadbeef); /* report ID */
		igt_assert_neq(report0_32[1], 0); /* timestamp */

		report1_32 = report0_32 + 64;
		igt_assert_eq(report1_32[0], 0xbeefbeef); /* report ID */
		igt_assert_neq(report1_32[1], 0); /* timestamp */

		print_reports(report0_32, report1_32,
			      lookup_format(test_set->perf_oa_format));

		/* A40 == N samples written to all render targets */
		n_samples_written = report1_32[43] - report0_32[43];

		igt_debug("n samples written = %d\n", n_samples_written);
		igt_assert_eq(n_samples_written, width * height);

		igt_debug("timestamp32 0 = %u\n", report0_32[1]);
		igt_debug("timestamp32 1 = %u\n", report1_32[1]);

		timestamp0_64 = *(uint64_t *)(((uint8_t *)bo->virtual) + 512);
		timestamp1_64 = *(uint64_t *)(((uint8_t *)bo->virtual) + 520);

		igt_debug("timestamp64 0 = %"PRIu64"\n", timestamp0_64);
		igt_debug("timestamp64 1 = %"PRIu64"\n", timestamp1_64);

		delta_ts64 = timestamp1_64 - timestamp0_64;
		delta_oa32 = report1_32[1] - report0_32[1];

		/* sanity check that we can pass the delta to timebase_scale */
		igt_assert(delta_ts64 < UINT32_MAX);
		delta_oa32_ns = timebase_scale(delta_oa32);
		delta_ts64_ns = timebase_scale(delta_ts64);

		igt_debug("ts32 delta = %u, = %uns\n",
			  delta_oa32, (unsigned)delta_oa32_ns);
		igt_debug("ts64 delta = %u, = %uns\n",
			  delta_ts64, (unsigned)delta_ts64_ns);

		/* The delta as calculated via the PIPE_CONTROL timestamp or
		 * the OA report timestamps should be almost identical but
		 * allow a 320 nanoseconds margin.
		 */
		delta_delta = delta_ts64_ns > delta_oa32_ns ?
			(delta_ts64_ns - delta_oa32_ns) :
			(delta_oa32_ns - delta_ts64_ns);
		igt_assert(delta_delta <= 320);

		for (int i = 0; i < ARRAY_SIZE(src); i++) {
			drm_intel_bo_unreference(src[i].bo);
			drm_intel_bo_unreference(dst[i].bo);
		}

		drm_intel_bo_unmap(bo);
		drm_intel_bo_unreference(bo);
		intel_batchbuffer_free(batch);
		drm_intel_gem_context_destroy(context0);
		drm_intel_gem_context_destroy(context1);
		drm_intel_bufmgr_destroy(bufmgr);
		__perf_close(stream_fd);
	}

	igt_waitchildren();
}

/* Tests the INTEL_performance_query use case where an unprivileged process
 * should be able to configure the OA unit for per-context metrics (for a
 * context associated with that process' drm file descriptor) and the counters
 * should only relate to that specific context.
 *
 * For Gen8+ although reports read via i915 perf can be filtered for a single
 * context the counters themselves always progress as global/system-wide
 * counters affected by all contexts. To support the INTEL_performance_query
 * use case on Gen8+ it's necessary to combine OABUFFER and
 * MI_REPORT_PERF_COUNT reports so that counter normalisation can take into
 * account context-switch reports and factor out any counter progression not
 * associated with the current context.
 */
static void
gen8_test_single_ctx_render_target_writes_a_counter(void)
{
	int oa_exponent = max_oa_exponent_for_period_lte(1000000);
	uint64_t properties[] = {
		DRM_I915_PERF_PROP_CTX_HANDLE, UINT64_MAX, /* updated below */

		/* Note: we have to specify at least one sample property even
		 * though we aren't interested in samples in this case
		 */
		DRM_I915_PERF_PROP_SAMPLE_OA, true,

		/* OA unit configuration */
		DRM_I915_PERF_PROP_OA_METRICS_SET, test_set->perf_oa_metrics_set,
		DRM_I915_PERF_PROP_OA_FORMAT, test_set->perf_oa_format,
		DRM_I915_PERF_PROP_OA_EXPONENT, oa_exponent,

		/* Note: no OA exponent specified in this case */
	};
	struct drm_i915_perf_open_param param = {
		.flags = I915_PERF_FLAG_FD_CLOEXEC,
		.num_properties = ARRAY_SIZE(properties) / 2,
		.properties_ptr = to_user_pointer(properties),
	};
	size_t format_size = get_oa_format(test_set->perf_oa_format).size;
	size_t sample_size = (sizeof(struct drm_i915_perf_record_header) +
			      format_size);
	int max_reports = MAX_OA_BUF_SIZE / format_size;
	int buf_size = sample_size * max_reports * 1.5;
	int child_ret;
	uint8_t *buf = malloc(buf_size);
	ssize_t len;
	struct igt_helper_process child = {};

	/* should be default, but just to be sure... */
	write_u64_file("/proc/sys/dev/i915/perf_stream_paranoid", 1);

	do {

		igt_fork_helper(&child) {
			struct drm_i915_perf_record_header *header;
			drm_intel_bufmgr *bufmgr;
			drm_intel_context *context0, *context1;
			struct intel_batchbuffer *batch;
			struct igt_buf src[3], dst[3];
			drm_intel_bo *bo;
			uint32_t *report0_32, *report1_32;
			uint32_t *prev, *lprev = NULL;
			uint64_t timestamp0_64, timestamp1_64;
			uint32_t delta_ts64, delta_oa32;
			uint64_t delta_ts64_ns, delta_oa32_ns;
			int width = 800;
			int height = 600;
			uint32_t ctx_id = 0xffffffff; /* invalid handle */
			uint32_t ctx1_id = 0xffffffff;  /* invalid handle */
			uint32_t current_ctx_id = 0xffffffff;
			uint32_t n_invalid_ctx = 0;
			int delta_delta;
			int ret;
			struct accumulator accumulator = {
				.format = test_set->perf_oa_format
			};

			bufmgr = drm_intel_bufmgr_gem_init(drm_fd, 4096);
			drm_intel_bufmgr_gem_enable_reuse(bufmgr);

			for (int i = 0; i < ARRAY_SIZE(src); i++) {
				scratch_buf_init(bufmgr, &src[i], width, height, 0xff0000ff);
				scratch_buf_init(bufmgr, &dst[i], width, height, 0x00ff00ff);
			}

			batch = intel_batchbuffer_alloc(bufmgr, devid);

			context0 = drm_intel_gem_context_create(bufmgr);
			igt_assert(context0);

			context1 = drm_intel_gem_context_create(bufmgr);
			igt_assert(context1);

			igt_debug("submitting warm up render_copy\n");

			/* Submit some early, unmeasured, work to the context we want
			 * to measure to try and catch issues with i915-perf
			 * initializing the HW context ID for filtering.
			 *
			 * We do this because i915-perf single context filtering had
			 * previously only relied on a hook into context pinning to
			 * initialize the HW context ID, instead of also trying to
			 * determine the HW ID while opening the stream, in case it
			 * has already been pinned.
			 *
			 * This wasn't noticed by the previous unit test because we
			 * were opening the stream while the context hadn't been
			 * touched or pinned yet and so it worked out correctly to wait
			 * for the pinning hook.
			 *
			 * Now a buggy version of i915-perf will fail to measure
			 * anything for context0 once this initial render_copy() ends
			 * up pinning the context since there won't ever be a pinning
			 * hook callback.
			 */
			render_copy(batch,
				    context0,
				    &src[0], 0, 0, width, height,
				    &dst[0], 0, 0);

			ret = drm_intel_gem_context_get_id(context0, &ctx_id);
			igt_assert_eq(ret, 0);
			igt_assert_neq(ctx_id, 0xffffffff);
			properties[1] = ctx_id;

			scratch_buf_memset(src[0].bo, width, height, 0xff0000ff);
			scratch_buf_memset(dst[0].bo, width, height, 0x00ff00ff);

			igt_debug("opening i915-perf stream\n");
			stream_fd = __perf_open(drm_fd, &param, false);

			bo = drm_intel_bo_alloc(bufmgr, "mi_rpc dest bo", 4096, 64);

			ret = drm_intel_bo_map(bo, true /* write enable */);
			igt_assert_eq(ret, 0);

			memset(bo->virtual, 0x80, 4096);
			drm_intel_bo_unmap(bo);

			emit_stall_timestamp_and_rpc(batch,
						     bo,
						     512 /* timestamp offset */,
						     0, /* report dst offset */
						     0xdeadbeef); /* report id */

			/* Explicitly flush here (even though the render_copy() call
			 * will itself flush before/after the copy) to clarify that
			 * that the PIPE_CONTROL + MI_RPC commands will be in a
			 * separate batch from the copy.
			 */
			intel_batchbuffer_flush_with_context(batch, context0);

			render_copy(batch,
				    context0,
				    &src[0], 0, 0, width, height,
				    &dst[0], 0, 0);

			/* Another redundant flush to clarify batch bo is free to reuse */
			intel_batchbuffer_flush_with_context(batch, context0);

			/* submit two copies on the other context to avoid a false
			 * positive in case the driver somehow ended up filtering for
			 * context1
			 */
			render_copy(batch,
				    context1,
				    &src[1], 0, 0, width, height,
				    &dst[1], 0, 0);

			ret = drm_intel_gem_context_get_id(context1, &ctx1_id);
			igt_assert_eq(ret, 0);
			igt_assert_neq(ctx1_id, 0xffffffff);

			render_copy(batch,
				    context1,
				    &src[2], 0, 0, width, height,
				    &dst[2], 0, 0);

			/* And another */
			intel_batchbuffer_flush_with_context(batch, context1);

			emit_stall_timestamp_and_rpc(batch,
						     bo,
						     520 /* timestamp offset */,
						     256, /* report dst offset */
						     0xbeefbeef); /* report id */

			intel_batchbuffer_flush_with_context(batch, context1);

			ret = drm_intel_bo_map(bo, false /* write enable */);
			igt_assert_eq(ret, 0);

			report0_32 = bo->virtual;
			igt_assert_eq(report0_32[0], 0xdeadbeef); /* report ID */
			igt_assert_neq(report0_32[1], 0); /* timestamp */
			prev = report0_32;
			ctx_id = prev[2];
			igt_debug("MI_RPC(start) CTX ID: %u\n", ctx_id);
			dump_report(report0_32, 64, "report0_32");

			report1_32 = report0_32 + 64; /* 64 uint32_t = 256bytes offset */
			igt_assert_eq(report1_32[0], 0xbeefbeef); /* report ID */
			igt_assert_neq(report1_32[1], 0); /* timestamp */
			ctx1_id = report1_32[2];
			dump_report(report1_32, 64, "report1_32");

			memset(accumulator.deltas, 0, sizeof(accumulator.deltas));
			accumulate_reports(&accumulator, report0_32, report1_32);
			igt_debug("total: A0 = %"PRIu64", A21 = %"PRIu64", A26 = %"PRIu64"\n",
				  accumulator.deltas[2 + 0], /* skip timestamp + clock cycles */
				  accumulator.deltas[2 + 21],
				  accumulator.deltas[2 + 26]);

			igt_debug("oa_timestamp32 0 = %u\n", report0_32[1]);
			igt_debug("oa_timestamp32 1 = %u\n", report1_32[1]);
			igt_debug("ctx_id 0 = %u\n", report0_32[2]);
			igt_debug("ctx_id 1 = %u\n", report1_32[2]);

			timestamp0_64 = *(uint64_t *)(((uint8_t *)bo->virtual) + 512);
			timestamp1_64 = *(uint64_t *)(((uint8_t *)bo->virtual) + 520);

			igt_debug("ts_timestamp64 0 = %"PRIu64"\n", timestamp0_64);
			igt_debug("ts_timestamp64 1 = %"PRIu64"\n", timestamp1_64);

			delta_ts64 = timestamp1_64 - timestamp0_64;
			delta_oa32 = report1_32[1] - report0_32[1];

			/* sanity check that we can pass the delta to timebase_scale */
			igt_assert(delta_ts64 < UINT32_MAX);
			delta_oa32_ns = timebase_scale(delta_oa32);
			delta_ts64_ns = timebase_scale(delta_ts64);

			igt_debug("oa32 delta = %u, = %uns\n",
				  delta_oa32, (unsigned)delta_oa32_ns);
			igt_debug("ts64 delta = %u, = %uns\n",
				  delta_ts64, (unsigned)delta_ts64_ns);

			/* The delta as calculated via the PIPE_CONTROL timestamp or
			 * the OA report timestamps should be almost identical but
			 * allow a 2 microsecond margin.
			 */
			delta_delta = delta_ts64_ns - delta_oa32_ns;
			if (abs(delta_delta) > 2000) {
				igt_debug("Too slow %d; skipping\n",
					  delta_delta);
				ret = EAGAIN;
				goto again;
			}

			len = i915_read_reports_until_timestamp(test_set->perf_oa_format,
								buf, buf_size,
								report0_32[1],
								report1_32[1]);

			igt_assert(len > 0);
			igt_debug("read %d bytes\n", (int)len);

			memset(accumulator.deltas, 0, sizeof(accumulator.deltas));

			for (size_t offset = 0; offset < len; offset += header->size) {
				uint32_t *report;
				uint32_t reason;
				const char *skip_reason = NULL, *report_reason = NULL;
				struct accumulator laccumulator = {
					.format = test_set->perf_oa_format
				};


				header = (void *)(buf + offset);

				igt_assert_eq(header->pad, 0); /* Reserved */

				/* Currently the only test that should ever expect to
				 * see a _BUFFER_LOST error is the buffer_fill test,
				 * otherwise something bad has probably happened...
				 */
				igt_assert_neq(header->type, DRM_I915_PERF_RECORD_OA_BUFFER_LOST);

				/* At high sampling frequencies the OA HW might not be
				 * able to cope with all write requests and will notify
				 * us that a report was lost.
				 *
				 * XXX: we should maybe restart the test in this case?
				 */
				if (header->type == DRM_I915_PERF_RECORD_OA_REPORT_LOST) {
					igt_debug("OA trigger collision / report lost\n");
					ret = EAGAIN;
					goto again;
				}

				/* Currently the only other record type expected is a
				 * _SAMPLE. Notably this test will need updating if
				 * i915-perf is extended in the future with additional
				 * record types.
				 */
				igt_assert_eq(header->type, DRM_I915_PERF_RECORD_SAMPLE);

				igt_assert_eq(header->size, sample_size);

				report = (void *)(header + 1);
				dump_report(report, 64, "OA report");

				/* Don't expect zero for timestamps */
				igt_assert_neq(report[1], 0);

				igt_debug("report %p:\n", report);

				/* Discard reports not contained in between the
				 * timestamps we're looking at. */
				{
					uint32_t time_delta = report[1] - report0_32[1];

					if (timebase_scale(time_delta) > 1000000000) {
						skip_reason = "prior first mi-rpc";
					}
				}

				{
					uint32_t time_delta = report[1] - report1_32[1];

					if (timebase_scale(time_delta) <= 1000000000) {
						igt_debug("    comes after last MI_RPC (%u)\n",
							  report1_32[1]);
						report = report1_32;
					}
				}

				/* Print out deltas for a few significant
				 * counters for each report. */
				if (lprev) {
					memset(laccumulator.deltas, 0, sizeof(laccumulator.deltas));
					accumulate_reports(&laccumulator, lprev, report);
					igt_debug("    deltas: A0=%"PRIu64" A21=%"PRIu64", A26=%"PRIu64"\n",
						  laccumulator.deltas[2 + 0], /* skip timestamp + clock cycles */
						  laccumulator.deltas[2 + 21],
						  laccumulator.deltas[2 + 26]);
				}
				lprev = report;

				/* Print out reason for the report. */
				reason = ((report[0] >> OAREPORT_REASON_SHIFT) &
					  OAREPORT_REASON_MASK);

				if (reason & OAREPORT_REASON_CTX_SWITCH) {
					report_reason = "ctx-load";
				} else if (reason & OAREPORT_REASON_TIMER) {
					report_reason = "timer";
				} else if (reason & OAREPORT_REASON_INTERNAL ||
					   reason & OAREPORT_REASON_GO ||
					   reason & OAREPORT_REASON_CLK_RATIO) {
					report_reason = "internal/go/clk-ratio";
				} else {
					report_reason = "end-mi-rpc";
				}
				igt_debug("    ctx_id=%u/%x reason=%s oa_timestamp32=%u\n",
					  report[2], report[2], report_reason, report[1]);

				/* Should we skip this report?
				 *
				 *   Only if the current context id of
				 *   the stream is not the one we want
				 *   to measure.
				 */
				if (current_ctx_id != ctx_id) {
					skip_reason = "not our context";
				}

				if (n_invalid_ctx > 1) {
					skip_reason = "too many invalid context events";
				}

				if (!skip_reason) {
					accumulate_reports(&accumulator, prev, report);
					igt_debug(" -> Accumulated deltas A0=%"PRIu64" A21=%"PRIu64", A26=%"PRIu64"\n",
						  accumulator.deltas[2 + 0], /* skip timestamp + clock cycles */
						  accumulator.deltas[2 + 21],
						  accumulator.deltas[2 + 26]);
				} else {
					igt_debug(" -> Skipping: %s\n", skip_reason);
				}


				/* Finally update current-ctx_id, only possible
				 * with a valid context id. */
				if (oa_report_ctx_is_valid(report)) {
					current_ctx_id = report[2];
					n_invalid_ctx = 0;
				} else {
					n_invalid_ctx++;
				}

				prev = report;

				if (report == report1_32) {
					igt_debug("Breaking on end of report\n");
					print_reports(report0_32, report1_32,
						      lookup_format(test_set->perf_oa_format));
					break;
				}
			}

			igt_debug("n samples written = %"PRIu64"/%"PRIu64" (%ix%i)\n",
				  accumulator.deltas[2 + 21],/* skip timestamp + clock cycles */
				  accumulator.deltas[2 + 26],
				  width, height);
			accumulator_print(&accumulator, "filtered");

			ret = drm_intel_bo_map(src[0].bo, false /* write enable */);
			igt_assert_eq(ret, 0);
			ret = drm_intel_bo_map(dst[0].bo, false /* write enable */);
			igt_assert_eq(ret, 0);

			ret = memcmp(src[0].bo->virtual, dst[0].bo->virtual, 4 * width * height);
			drm_intel_bo_unmap(src[0].bo);
			drm_intel_bo_unmap(dst[0].bo);

again:
			for (int i = 0; i < ARRAY_SIZE(src); i++) {
				drm_intel_bo_unreference(src[i].bo);
				drm_intel_bo_unreference(dst[i].bo);
			}

			drm_intel_bo_unmap(bo);
			drm_intel_bo_unreference(bo);
			intel_batchbuffer_free(batch);
			drm_intel_gem_context_destroy(context0);
			drm_intel_gem_context_destroy(context1);
			drm_intel_bufmgr_destroy(bufmgr);
			__perf_close(stream_fd);
			gem_quiescent_gpu(drm_fd);

			if (ret != 0) {
				accumulator_print(&accumulator, "total");
				/* This needs to be investigated... From time
				 * to time, the work we kick off doesn't seem
				 * to happen. WTH?? */
				exit(EAGAIN);
			}

			igt_assert_eq(accumulator.deltas[2 + 26], width * height);
		}

		child_ret = igt_wait_helper(&child);

		igt_assert(WEXITSTATUS(child_ret) == EAGAIN ||
			   WEXITSTATUS(child_ret) == 0);

	} while (WEXITSTATUS(child_ret) == EAGAIN);
}

static void gen12_single_ctx_helper(void)
{
	uint64_t properties[] = {
		/* Have a random value here for the context id, but initialize
		 * it once you figure out the context ID for the work to be
		 * measured
		 */
		DRM_I915_PERF_PROP_CTX_HANDLE, UINT64_MAX,

		/* OA unit configuration:
		 * DRM_I915_PERF_PROP_SAMPLE_OA is no longer required for Gen12
		 * because the OAR unit increments counters only for the
		 * relevant context. No other parameters are needed since we do
		 * not rely on the OA buffer anymore to normalize the counter
		 * values.
		 */
		DRM_I915_PERF_PROP_OA_METRICS_SET, test_set->perf_oa_metrics_set,
		DRM_I915_PERF_PROP_OA_FORMAT, test_set->perf_oa_format,
	};
	struct drm_i915_perf_open_param param = {
		.flags = I915_PERF_FLAG_FD_CLOEXEC,
		.num_properties = ARRAY_SIZE(properties) / 2,
		.properties_ptr = to_user_pointer(properties),
	};
	drm_intel_bufmgr *bufmgr;
	drm_intel_context *context0, *context1;
	struct intel_batchbuffer *batch;
	struct igt_buf src[3], dst[3];
	drm_intel_bo *bo;
	uint32_t *report0_32, *report1_32, *report2_32, *report3_32;
	uint64_t timestamp0_64, timestamp1_64;
	uint32_t delta_ts64, delta_oa32;
	uint64_t delta_ts64_ns, delta_oa32_ns;
	uint32_t delta_delta;
	int width = 800;
	int height = 600;
#define INVALID_CTX_ID 0xffffffff
	uint32_t ctx0_id = INVALID_CTX_ID;
	uint32_t ctx1_id = INVALID_CTX_ID;
	int ret;
	struct accumulator accumulator = {
		.format = test_set->perf_oa_format
	};

	bufmgr = drm_intel_bufmgr_gem_init(drm_fd, 4096);
	drm_intel_bufmgr_gem_enable_reuse(bufmgr);

	for (int i = 0; i < ARRAY_SIZE(src); i++) {
		scratch_buf_init(bufmgr, &src[i], width, height, 0xff0000ff);
		scratch_buf_init(bufmgr, &dst[i], width, height, 0x00ff00ff);
	}

	batch = intel_batchbuffer_alloc(bufmgr, devid);

	context0 = drm_intel_gem_context_create(bufmgr);
	igt_assert(context0);

	context1 = drm_intel_gem_context_create(bufmgr);
	igt_assert(context1);

	igt_debug("submitting warm up render_copy\n");

	/* Submit some early, unmeasured, work to the context we want
	 * to measure to try and catch issues with i915-perf
	 * initializing the HW context ID for filtering.
	 *
	 * We do this because i915-perf single context filtering had
	 * previously only relied on a hook into context pinning to
	 * initialize the HW context ID, instead of also trying to
	 * determine the HW ID while opening the stream, in case it
	 * has already been pinned.
	 *
	 * This wasn't noticed by the previous unit test because we
	 * were opening the stream while the context hadn't been
	 * touched or pinned yet and so it worked out correctly to wait
	 * for the pinning hook.
	 *
	 * Now a buggy version of i915-perf will fail to measure
	 * anything for context0 once this initial render_copy() ends
	 * up pinning the context since there won't ever be a pinning
	 * hook callback.
	 */
	render_copy(batch, context0,
		    &src[0], 0, 0, width, height,
		    &dst[0], 0, 0);

	/* Initialize the context parameter to the perf open ioctl here */
	ret = drm_intel_gem_context_get_id(context0, &ctx0_id);
	igt_assert_eq(ret, 0);
	igt_assert_neq(ctx0_id, 0xffffffff);
	properties[1] = ctx0_id;

	igt_debug("opening i915-perf stream\n");
	stream_fd = __perf_open(drm_fd, &param, false);

	bo = drm_intel_bo_alloc(bufmgr, "mi_rpc dest bo", 4096, 64);

	/* Set write domain to cpu briefly to fill the buffer with 80s */
	ret = drm_intel_bo_map(bo, true);
	igt_assert_eq(ret, 0);
	memset(bo->virtual, 0x80, 2048);
	memset(bo->virtual + 2048, 0, 2048);
	drm_intel_bo_unmap(bo);

	/* Submit an mi-rpc to context0 before measurable work */
#define BO_TIMESTAMP_OFFSET0 1024
#define BO_REPORT_OFFSET0 0
#define BO_REPORT_ID0 0xdeadbeef
	emit_stall_timestamp_and_rpc(batch,
				     bo,
				     BO_TIMESTAMP_OFFSET0,
				     BO_REPORT_OFFSET0,
				     BO_REPORT_ID0);
	intel_batchbuffer_flush_with_context(batch, context0);

	/* This is the work/context that is measured for counter increments */
	render_copy(batch, context0,
		    &src[0], 0, 0, width, height,
		    &dst[0], 0, 0);
	intel_batchbuffer_flush_with_context(batch, context0);

	/* Submit an mi-rpc to context1 before work
	 *
	 * On gen12, this measurement should just yield counters that are
	 * all zeroes, since the counters will only increment for the
	 * context passed to perf open ioctl
	 */
#define BO_TIMESTAMP_OFFSET2 1040
#define BO_REPORT_OFFSET2 512
#define BO_REPORT_ID2 0x00c0ffee
	emit_stall_timestamp_and_rpc(batch,
				     bo,
				     BO_TIMESTAMP_OFFSET2,
				     BO_REPORT_OFFSET2,
				     BO_REPORT_ID2);
	intel_batchbuffer_flush_with_context(batch, context1);

	/* Submit two copies on the other context to avoid a false
	 * positive in case the driver somehow ended up filtering for
	 * context1
	 */
	render_copy(batch, context1,
		    &src[1], 0, 0, width, height,
		    &dst[1], 0, 0);
	ret = drm_intel_gem_context_get_id(context1, &ctx1_id);
	igt_assert_eq(ret, 0);
	igt_assert_neq(ctx1_id, 0xffffffff);

	render_copy(batch, context1,
		    &src[2], 0, 0, width, height,
		    &dst[2], 0, 0);
	intel_batchbuffer_flush_with_context(batch, context1);

	/* Submit an mi-rpc to context1 after all work */
#define BO_TIMESTAMP_OFFSET3 1048
#define BO_REPORT_OFFSET3 768
#define BO_REPORT_ID3 0x01c0ffee
	emit_stall_timestamp_and_rpc(batch,
				     bo,
				     BO_TIMESTAMP_OFFSET3,
				     BO_REPORT_OFFSET3,
				     BO_REPORT_ID3);
	intel_batchbuffer_flush_with_context(batch, context1);

	/* Submit an mi-rpc to context0 after all measurable work */
#define BO_TIMESTAMP_OFFSET1 1032
#define BO_REPORT_OFFSET1 256
#define BO_REPORT_ID1 0xbeefbeef
	emit_stall_timestamp_and_rpc(batch,
				     bo,
				     BO_TIMESTAMP_OFFSET1,
				     BO_REPORT_OFFSET1,
				     BO_REPORT_ID1);
	intel_batchbuffer_flush_with_context(batch, context0);

	/* Set write domain to none */
	ret = drm_intel_bo_map(bo, false);
	igt_assert_eq(ret, 0);

	/* Sanity check reports
	 * reportX_32[0]: report id passed with mi-rpc
	 * reportX_32[1]: timestamp
	 * reportX_32[2]: context id
	 *
	 * report0_32: start of measurable work
	 * report1_32: end of measurable work
	 * report2_32: start of other work
	 * report3_32: end of other work
	 */
	report0_32 = bo->virtual;
	igt_assert_eq(report0_32[0], 0xdeadbeef);
	igt_assert_neq(report0_32[1], 0);
	ctx0_id = report0_32[2];
	igt_debug("MI_RPC(start) CTX ID: %u\n", ctx0_id);
	dump_report(report0_32, 64, "report0_32");

	report1_32 = report0_32 + 64;
	igt_assert_eq(report1_32[0], 0xbeefbeef);
	igt_assert_neq(report1_32[1], 0);
	ctx1_id = report1_32[2];
	dump_report(report1_32, 64, "report1_32");

	/* Verify that counters in context1 are all zeroes */
	report2_32 = report0_32 + 128;
	igt_assert_eq(report2_32[0], 0x00c0ffee);
	igt_assert_neq(report2_32[1], 0);
	dump_report(report2_32, 64, "report2_32");
	igt_assert_eq(0, memcmp(&report2_32[4],
				bo->virtual + 2048,
				240));

	report3_32 = report0_32 + 192;
	igt_assert_eq(report3_32[0], 0x01c0ffee);
	igt_assert_neq(report3_32[1], 0);
	dump_report(report3_32, 64, "report3_32");
	igt_assert_eq(0, memcmp(&report3_32[4],
				bo->virtual + 2048,
				240));

	/* Accumulate deltas for counters - A0, A21 and A26 */
	memset(accumulator.deltas, 0, sizeof(accumulator.deltas));
	accumulate_reports(&accumulator, report0_32, report1_32);
	igt_debug("total: A0 = %"PRIu64", A21 = %"PRIu64", A26 = %"PRIu64"\n",
			accumulator.deltas[2 + 0],
			accumulator.deltas[2 + 21],
			accumulator.deltas[2 + 26]);

	igt_debug("oa_timestamp32 0 = %u\n", report0_32[1]);
	igt_debug("oa_timestamp32 1 = %u\n", report1_32[1]);
	igt_debug("ctx_id 0 = %u\n", report0_32[2]);
	igt_debug("ctx_id 1 = %u\n", report1_32[2]);

	/* The delta as calculated via the PIPE_CONTROL timestamp or
	 * the OA report timestamps should be almost identical but
	 * allow a 500 nanoseconds margin.
	 */
	timestamp0_64 = *(uint64_t *)(((uint8_t *)bo->virtual) + BO_TIMESTAMP_OFFSET0);
	timestamp1_64 = *(uint64_t *)(((uint8_t *)bo->virtual) + BO_TIMESTAMP_OFFSET1);

	igt_debug("ts_timestamp64 0 = %"PRIu64"\n", timestamp0_64);
	igt_debug("ts_timestamp64 1 = %"PRIu64"\n", timestamp1_64);

	delta_ts64 = timestamp1_64 - timestamp0_64;
	delta_oa32 = report1_32[1] - report0_32[1];

	/* Sanity check that we can pass the delta to timebase_scale */
	igt_assert(delta_ts64 < UINT32_MAX);
	delta_oa32_ns = timebase_scale(delta_oa32);
	delta_ts64_ns = timebase_scale(delta_ts64);

	igt_debug("oa32 delta = %u, = %uns\n",
			delta_oa32, (unsigned)delta_oa32_ns);
	igt_debug("ts64 delta = %u, = %uns\n",
			delta_ts64, (unsigned)delta_ts64_ns);

	delta_delta = delta_ts64_ns > delta_oa32_ns ?
		      (delta_ts64_ns - delta_oa32_ns) :
		      (delta_oa32_ns - delta_ts64_ns);
	if (delta_delta > 500) {
		igt_debug("delta_delta exceeds margin, skipping..\n");
		exit(EAGAIN);
	}

	igt_debug("n samples written = %"PRIu64"/%"PRIu64" (%ix%i)\n",
		  accumulator.deltas[2 + 21],
		  accumulator.deltas[2 + 26],
		  width, height);
	accumulator_print(&accumulator, "filtered");

	/* Verify that the work actually happened by comparing the src
	 * and dst buffers
	 */
	ret = drm_intel_bo_map(src[0].bo, false);
	igt_assert_eq(ret, 0);
	ret = drm_intel_bo_map(dst[0].bo, false);
	igt_assert_eq(ret, 0);

	ret = memcmp(src[0].bo->virtual, dst[0].bo->virtual, 4 * width * height);
	if (ret != 0) {
		accumulator_print(&accumulator, "total");
		exit(EAGAIN);
	}

	drm_intel_bo_unmap(src[0].bo);
	drm_intel_bo_unmap(dst[0].bo);

	/* Check that this test passed. The test measures the number of 2x2
	 * samples written to the render target using the counter A26. For
	 * OAR, this counter will only have increments relevant to this specific
	 * context. The value equals the width * height of the rendered work.
	 */
	igt_assert_eq(accumulator.deltas[2 + 26], width * height);

	/* Clean up */
	for (int i = 0; i < ARRAY_SIZE(src); i++) {
		drm_intel_bo_unreference(src[i].bo);
		drm_intel_bo_unreference(dst[i].bo);
	}

	drm_intel_bo_unmap(bo);
	drm_intel_bo_unreference(bo);
	intel_batchbuffer_free(batch);
	drm_intel_gem_context_destroy(context0);
	drm_intel_gem_context_destroy(context1);
	drm_intel_bufmgr_destroy(bufmgr);
	__perf_close(stream_fd);
}

static void
gen12_test_single_ctx_render_target_writes_a_counter(void)
{
	int child_ret;
	struct igt_helper_process child = {};

	/* Ensure perf_stream_paranoid is set to 1 by default */
	write_u64_file("/proc/sys/dev/i915/perf_stream_paranoid", 1);

	do {
		igt_fork_helper(&child) {
			/* A local device for local resources. */
			drm_fd = gem_reopen_driver(drm_fd);

			igt_drop_root();
			gen12_single_ctx_helper();

			close(drm_fd);
		}
		child_ret = igt_wait_helper(&child);
		igt_assert(WEXITSTATUS(child_ret) == EAGAIN ||
			   WEXITSTATUS(child_ret) == 0);
	} while (WEXITSTATUS(child_ret) == EAGAIN);
}

static unsigned long rc6_residency_ms(void)
{
	return sysfs_read("power/rc6_residency_ms");
}

static void
test_rc6_disable(void)
{
	uint64_t properties[] = {
		/* Include OA reports in samples */
		DRM_I915_PERF_PROP_SAMPLE_OA, true,

		/* OA unit configuration */
		DRM_I915_PERF_PROP_OA_METRICS_SET, test_set->perf_oa_metrics_set,
		DRM_I915_PERF_PROP_OA_FORMAT, test_set->perf_oa_format,
		DRM_I915_PERF_PROP_OA_EXPONENT, oa_exp_1_millisec,
	};
	struct drm_i915_perf_open_param param = {
		.flags = I915_PERF_FLAG_FD_CLOEXEC,
		.num_properties = sizeof(properties) / 16,
		.properties_ptr = to_user_pointer(properties),
	};
	unsigned long rc6_start, rc6_end, rc6_enabled;

	rc6_enabled = 0;
	igt_sysfs_scanf(sysfs, "power/rc6_enable", "%lu", &rc6_enabled);
	igt_require(rc6_enabled);

	/* Verify rc6 is functional by measuring residency while idle */
	gem_quiescent_gpu(drm_fd);
	rc6_start = rc6_residency_ms();
	usleep(50000);
	rc6_end = rc6_residency_ms();
	igt_require(rc6_end != rc6_start);

	/* While OA is active, we keep rc6 disabled so we don't lose metrics */
	stream_fd = __perf_open(drm_fd, &param, false);

	rc6_start = rc6_residency_ms();
	usleep(50000);
	rc6_end = rc6_residency_ms();
	igt_assert_eq(rc6_end - rc6_start, 0);

	__perf_close(stream_fd);
	gem_quiescent_gpu(drm_fd);

	/* But once OA is closed, we expect the device to sleep again */
	rc6_start = rc6_residency_ms();
	usleep(50000);
	rc6_end = rc6_residency_ms();
	igt_assert_neq(rc6_end - rc6_start, 0);
}

static void
test_stress_open_close(void)
{
	load_helper_init();
	load_helper_run(HIGH);

	igt_until_timeout(2) {
		int oa_exponent = 5; /* 5 micro seconds */
		uint64_t properties[] = {
			/* XXX: even without periodic sampling we have to
			 * specify at least one sample layout property...
			 */
			DRM_I915_PERF_PROP_SAMPLE_OA, true,

			/* OA unit configuration */
			DRM_I915_PERF_PROP_OA_METRICS_SET, test_set->perf_oa_metrics_set,
			DRM_I915_PERF_PROP_OA_FORMAT, test_set->perf_oa_format,
			DRM_I915_PERF_PROP_OA_EXPONENT, oa_exponent,
		};
		struct drm_i915_perf_open_param param = {
			.flags = I915_PERF_FLAG_FD_CLOEXEC |
			         I915_PERF_FLAG_DISABLED, /* XXX: open disabled */
			.num_properties = NUM_PROPERTIES(properties),
			.properties_ptr = to_user_pointer(properties),
		};

		stream_fd = __perf_open(drm_fd, &param, false);
		__perf_close(stream_fd);
	}

	load_helper_stop();
	load_helper_fini();
}

static uint64_t mask_minus_one(uint64_t mask)
{
	unsigned int i;

	for (i = 0; i < (sizeof(mask) * 8 - 1); i++) {
		if ((1ULL << i) & mask)
			return mask & ~(1ULL << i);
	}

	igt_assert(0);
	return 0;
}

static uint64_t mask_plus_one(uint64_t mask)
{
	unsigned int i;

	for (i = 0; i < (sizeof(mask) * 8 - 1); i++) {
		if (((1ULL << i) & mask) == 0)
			return mask | (1ULL << i);
	}

	igt_assert(0);
	return 0;
}

static void print_sseu_config(struct drm_i915_gem_context_param_sseu *sseu)
{
	igt_debug("   engine class/instance=%hu:%hu\n",
		  sseu->engine.engine_class,
		  sseu->engine.engine_instance);
	igt_debug("   slice_mask=0x%llx\n", sseu->slice_mask);
	igt_debug("   subslice_mask=0x%llx\n", sseu->subslice_mask);
	igt_debug("   eu min/max=%hu/%hu\n",
		  sseu->min_eus_per_subslice,
		  sseu->max_eus_per_subslice);
}

static struct drm_i915_gem_context_param_sseu
make_valid_reduced_sseu_config(struct drm_i915_gem_context_param_sseu default_sseu)
{
	struct drm_i915_gem_context_param_sseu sseu = default_sseu;

	if (intel_gen(devid) == 11) {
		/*
		 * On Gen11 there are restrictions on what subslices
		 * can be disabled, notably we're not able to enable
		 * more than half the subslice. So disable half
		 * subslices only.
		 */
		for (int i = 0; i < DIV_ROUND_UP(__builtin_popcount(default_sseu.subslice_mask), 2); i++)
			sseu.subslice_mask = mask_minus_one(sseu.subslice_mask);
	} else {
		sseu.subslice_mask = mask_minus_one(sseu.subslice_mask);
	}

	return sseu;
}

static void
test_global_sseu_config_invalid(void)
{
	struct drm_i915_gem_context_param_sseu default_sseu;
	struct drm_i915_gem_context_param_sseu sseu_param;
	struct drm_i915_gem_context_param ctx_gp = {
		.param = I915_CONTEXT_PARAM_SSEU,
		.size = sizeof(default_sseu),
		.value = to_user_pointer(&default_sseu),
	};
	uint64_t properties[] = {
		/* XXX: even without periodic sampling we have to
		 * specify at least one sample layout property...
		 */
		DRM_I915_PERF_PROP_SAMPLE_OA, true,

		/* OA unit configuration */
		DRM_I915_PERF_PROP_OA_METRICS_SET, test_set->perf_oa_metrics_set,
		DRM_I915_PERF_PROP_OA_FORMAT, test_set->perf_oa_format,
		DRM_I915_PERF_PROP_OA_EXPONENT, oa_exp_1_millisec,
		DRM_I915_PERF_PROP_GLOBAL_SSEU, to_user_pointer(&sseu_param),
	};
	struct drm_i915_perf_open_param param = {
		.flags = I915_PERF_FLAG_FD_CLOEXEC |
		I915_PERF_FLAG_DISABLED, /* XXX: open disabled */
		.num_properties = NUM_PROPERTIES(properties),
			.properties_ptr = to_user_pointer(properties),
	};

	memset(&default_sseu, 0, sizeof(default_sseu));
	igt_require(__gem_context_get_param(drm_fd, &ctx_gp) == 0);

	igt_debug("Default context sseu:\n");
	print_sseu_config(&default_sseu);

	/* Invalid engine class */
	sseu_param = default_sseu;
	sseu_param.engine.engine_class = -1;
	do_ioctl_err(drm_fd, DRM_IOCTL_I915_PERF_OPEN, &param, EINVAL);

	/* Invalid engine instance */
	sseu_param = default_sseu;
	sseu_param.engine.engine_instance = -1;
	do_ioctl_err(drm_fd, DRM_IOCTL_I915_PERF_OPEN, &param, EINVAL);

	/* Invalid slice mask */
	sseu_param = default_sseu;
	sseu_param.slice_mask = 0;
	do_ioctl_err(drm_fd, DRM_IOCTL_I915_PERF_OPEN, &param, EINVAL);

	sseu_param = default_sseu;
	sseu_param.slice_mask = mask_plus_one(sseu_param.slice_mask);
	do_ioctl_err(drm_fd, DRM_IOCTL_I915_PERF_OPEN, &param, EINVAL);

	/* Invalid subslice mask */
	sseu_param = default_sseu;
	sseu_param.subslice_mask = 0;
	do_ioctl_err(drm_fd, DRM_IOCTL_I915_PERF_OPEN, &param, EINVAL);

	sseu_param = default_sseu;
	sseu_param.subslice_mask = mask_plus_one(sseu_param.subslice_mask);
	do_ioctl_err(drm_fd, DRM_IOCTL_I915_PERF_OPEN, &param, EINVAL);

	/* Privileged operation */
	if (__builtin_popcount(default_sseu.subslice_mask) > 1) {
		igt_fork(child, 1) {
			igt_drop_root();

			sseu_param = make_valid_reduced_sseu_config(default_sseu);
			do_ioctl_err(drm_fd, DRM_IOCTL_I915_PERF_OPEN, &param, EACCES);
		}
		igt_waitchildren();
	}
}

static void
test_global_sseu_config(void)
{
	struct drm_i915_gem_context_param_sseu default_sseu;
	struct drm_i915_gem_context_param_sseu sseu_param;
	struct drm_i915_gem_context_param ctx_gp = {
		.param = I915_CONTEXT_PARAM_SSEU,
		.size = sizeof(default_sseu),
		.value = to_user_pointer(&default_sseu),
	};
	uint64_t properties[] = {
		/* XXX: even without periodic sampling we have to
		 * specify at least one sample layout property...
		 */
		DRM_I915_PERF_PROP_SAMPLE_OA, true,

		/* OA unit configuration */
		DRM_I915_PERF_PROP_OA_METRICS_SET, test_set->perf_oa_metrics_set,
		DRM_I915_PERF_PROP_OA_FORMAT, test_set->perf_oa_format,
		DRM_I915_PERF_PROP_OA_EXPONENT, oa_exp_1_millisec,
		DRM_I915_PERF_PROP_GLOBAL_SSEU, to_user_pointer(&sseu_param),
	};
	struct drm_i915_perf_open_param param = {
		.flags = I915_PERF_FLAG_FD_CLOEXEC |
		I915_PERF_FLAG_DISABLED, /* XXX: open disabled */
		.num_properties = NUM_PROPERTIES(properties),
			.properties_ptr = to_user_pointer(properties),
	};

	memset(&default_sseu, 0, sizeof(default_sseu));
	igt_require(__gem_context_get_param(drm_fd, &ctx_gp) == 0);

	igt_debug("Default context sseu:\n");
	print_sseu_config(&default_sseu);

	igt_require(__builtin_popcount(default_sseu.subslice_mask) > 1);

	write_u64_file("/proc/sys/dev/i915/perf_stream_paranoid", 0);

	sseu_param = make_valid_reduced_sseu_config(default_sseu);
	igt_debug("Selected context sseu:\n");
	print_sseu_config(&sseu_param);

	igt_fork(child, 1) {
		igt_drop_root();

		stream_fd = __perf_open(drm_fd, &param, false);
		__perf_close(stream_fd);
	}

	igt_waitchildren();

	write_u64_file("/proc/sys/dev/i915/perf_stream_paranoid", 1);

	stream_fd = __perf_open(drm_fd, &param, false);
	__perf_close(stream_fd);
}

static int __i915_perf_add_config(int fd, struct drm_i915_perf_oa_config *config)
{
	int ret = igt_ioctl(fd, DRM_IOCTL_I915_PERF_ADD_CONFIG, config);
	if (ret < 0)
		ret = -errno;
	return ret;
}

static int i915_perf_add_config(int fd, struct drm_i915_perf_oa_config *config)
{
	int config_id = __i915_perf_add_config(fd, config);

	igt_debug("config_id=%i\n", config_id);
	igt_assert(config_id > 0);

	return config_id;
}

static void i915_perf_remove_config(int fd, uint64_t config_id)
{
	igt_assert_eq(igt_ioctl(fd, DRM_IOCTL_I915_PERF_REMOVE_CONFIG,
				&config_id), 0);
}

static bool has_i915_perf_userspace_config(int fd)
{
	uint64_t config = 0;
	int ret = igt_ioctl(fd, DRM_IOCTL_I915_PERF_REMOVE_CONFIG, &config);
	igt_assert_eq(ret, -1);

	igt_debug("errno=%i\n", errno);

	return errno != EINVAL;
}

static void
test_invalid_create_userspace_config(void)
{
	struct drm_i915_perf_oa_config config;
	const char *uuid = "01234567-0123-0123-0123-0123456789ab";
	const char *invalid_uuid = "blablabla-wrong";
	uint32_t mux_regs[] = { 0x9888 /* NOA_WRITE */, 0x0 };
	uint32_t invalid_mux_regs[] = { 0x12345678 /* invalid register */, 0x0 };

	igt_require(has_i915_perf_userspace_config(drm_fd));

	memset(&config, 0, sizeof(config));

	/* invalid uuid */
	strncpy(config.uuid, invalid_uuid, sizeof(config.uuid));
	config.n_mux_regs = 1;
	config.mux_regs_ptr = to_user_pointer(mux_regs);
	config.n_boolean_regs = 0;
	config.n_flex_regs = 0;

	igt_assert_eq(__i915_perf_add_config(drm_fd, &config), -EINVAL);

	/* invalid mux_regs */
	memcpy(config.uuid, uuid, sizeof(config.uuid));
	config.n_mux_regs = 1;
	config.mux_regs_ptr = to_user_pointer(invalid_mux_regs);
	config.n_boolean_regs = 0;
	config.n_flex_regs = 0;

	igt_assert_eq(__i915_perf_add_config(drm_fd, &config), -EINVAL);

	/* empty config */
	memcpy(config.uuid, uuid, sizeof(config.uuid));
	config.n_mux_regs = 0;
	config.mux_regs_ptr = to_user_pointer(mux_regs);
	config.n_boolean_regs = 0;
	config.n_flex_regs = 0;

	igt_assert_eq(__i915_perf_add_config(drm_fd, &config), -EINVAL);

	/* empty config with null pointers */
	memcpy(config.uuid, uuid, sizeof(config.uuid));
	config.n_mux_regs = 1;
	config.mux_regs_ptr = to_user_pointer(NULL);
	config.n_boolean_regs = 2;
	config.boolean_regs_ptr = to_user_pointer(NULL);
	config.n_flex_regs = 3;
	config.flex_regs_ptr = to_user_pointer(NULL);

	igt_assert_eq(__i915_perf_add_config(drm_fd, &config), -EINVAL);

	/* invalid pointers */
	memcpy(config.uuid, uuid, sizeof(config.uuid));
	config.n_mux_regs = 42;
	config.mux_regs_ptr = to_user_pointer((void *) 0xDEADBEEF);
	config.n_boolean_regs = 0;
	config.n_flex_regs = 0;

	igt_assert_eq(__i915_perf_add_config(drm_fd, &config), -EFAULT);
}

static void
test_invalid_remove_userspace_config(void)
{
	struct drm_i915_perf_oa_config config;
	const char *uuid = "01234567-0123-0123-0123-0123456789ab";
	uint32_t mux_regs[] = { 0x9888 /* NOA_WRITE */, 0x0 };
	uint64_t config_id, wrong_config_id = 999999999;
	char path[512];

	igt_require(has_i915_perf_userspace_config(drm_fd));

	snprintf(path, sizeof(path), "metrics/%s/id", uuid);

	/* Destroy previous configuration if present */
	if (try_sysfs_read_u64(path, &config_id))
		i915_perf_remove_config(drm_fd, config_id);

	memset(&config, 0, sizeof(config));

	memcpy(config.uuid, uuid, sizeof(config.uuid));

	config.n_mux_regs = 1;
	config.mux_regs_ptr = to_user_pointer(mux_regs);
	config.n_boolean_regs = 0;
	config.n_flex_regs = 0;

	config_id = i915_perf_add_config(drm_fd, &config);

	/* Removing configs without permissions should fail. */
	igt_fork(child, 1) {
		igt_drop_root();

		do_ioctl_err(drm_fd, DRM_IOCTL_I915_PERF_REMOVE_CONFIG, &config_id, EACCES);
	}
	igt_waitchildren();

	/* Removing invalid config ID should fail. */
	do_ioctl_err(drm_fd, DRM_IOCTL_I915_PERF_REMOVE_CONFIG, &wrong_config_id, ENOENT);

	i915_perf_remove_config(drm_fd, config_id);
}

static void
test_create_destroy_userspace_config(void)
{
	struct drm_i915_perf_oa_config config;
	const char *uuid = "01234567-0123-0123-0123-0123456789ab";
	uint32_t mux_regs[] = { 0x9888 /* NOA_WRITE */, 0x0 };
	uint32_t flex_regs[100];
	int i;
	uint64_t config_id;
	uint64_t properties[] = {
		DRM_I915_PERF_PROP_OA_METRICS_SET, 0, /* Filled later */

		/* OA unit configuration */
		DRM_I915_PERF_PROP_SAMPLE_OA, true,
		DRM_I915_PERF_PROP_OA_FORMAT, test_set->perf_oa_format,
		DRM_I915_PERF_PROP_OA_EXPONENT, oa_exp_1_millisec,
		DRM_I915_PERF_PROP_OA_METRICS_SET
	};
	struct drm_i915_perf_open_param param = {
		.flags = I915_PERF_FLAG_FD_CLOEXEC |
		I915_PERF_FLAG_FD_NONBLOCK |
		I915_PERF_FLAG_DISABLED,
		.num_properties = ARRAY_SIZE(properties) / 2,
		.properties_ptr = to_user_pointer(properties),
	};
	char path[512];

	igt_require(has_i915_perf_userspace_config(drm_fd));

	snprintf(path, sizeof(path), "metrics/%s/id", uuid);

	/* Destroy previous configuration if present */
	if (try_sysfs_read_u64(path, &config_id))
		i915_perf_remove_config(drm_fd, config_id);

	memset(&config, 0, sizeof(config));
	memcpy(config.uuid, uuid, sizeof(config.uuid));

	config.n_mux_regs = 1;
	config.mux_regs_ptr = to_user_pointer(mux_regs);

	/* Flex EU counters are only available on gen8+ */
	if (intel_gen(devid) >= 8) {
		for (i = 0; i < ARRAY_SIZE(flex_regs) / 2; i++) {
			flex_regs[i * 2] = 0xe458; /* EU_PERF_CNTL0 */
			flex_regs[i * 2 + 1] = 0x0;
		}
		config.flex_regs_ptr = to_user_pointer(flex_regs);
		config.n_flex_regs = ARRAY_SIZE(flex_regs) / 2;
	}

	config.n_boolean_regs = 0;

	/* Creating configs without permissions shouldn't work. */
	igt_fork(child, 1) {
		igt_drop_root();

		igt_assert_eq(__i915_perf_add_config(drm_fd, &config), -EACCES);
	}
	igt_waitchildren();

	/* Create a new config */
	config_id = i915_perf_add_config(drm_fd, &config);

	/* Verify that adding the another config with the same uuid fails. */
	igt_assert_eq(__i915_perf_add_config(drm_fd, &config), -EADDRINUSE);

	/* Try to use the new config */
	properties[1] = config_id;
	stream_fd = __perf_open(drm_fd, &param, false);

	/* Verify that destroying the config doesn't yield any error. */
	i915_perf_remove_config(drm_fd, config_id);

	/* Read the config to verify shouldn't raise any issue. */
	config_id = i915_perf_add_config(drm_fd, &config);

	__perf_close(stream_fd);

	i915_perf_remove_config(drm_fd, config_id);
}

/* Registers required by userspace. This list should be maintained by
 * the OA configs developers and agreed upon with kernel developers as
 * some of the registers have bits used by the kernel (for workarounds
 * for instance) and other bits that need to be set by the OA configs.
 */
static void
test_whitelisted_registers_userspace_config(void)
{
	struct drm_i915_perf_oa_config config;
	const char *uuid = "01234567-0123-0123-0123-0123456789ab";
	uint32_t mux_regs[200];
	uint32_t b_counters_regs[200];
	uint32_t flex_regs[200];
	uint32_t i;
	uint32_t oa_start_trig1, oa_start_trig8;
	uint32_t oa_report_trig1, oa_report_trig8;
	uint64_t config_id;
	char path[512];
	int ret;
	const uint32_t flex[] = {
		0xe458,
		0xe558,
		0xe658,
		0xe758,
		0xe45c,
		0xe55c,
		0xe65c
	};

	igt_require(has_i915_perf_userspace_config(drm_fd));

	snprintf(path, sizeof(path), "metrics/%s/id", uuid);

	if (try_sysfs_read_u64(path, &config_id))
		i915_perf_remove_config(drm_fd, config_id);

	memset(&config, 0, sizeof(config));
	memcpy(config.uuid, uuid, sizeof(config.uuid));

	if (intel_gen(devid) >= 12) {
		oa_start_trig1 = 0xd900;
		oa_start_trig8 = 0xd91c;
		oa_report_trig1 = 0xd920;
		oa_report_trig8 = 0xd93c;
	} else {
		oa_start_trig1 = 0x2710;
		oa_start_trig8 = 0x272c;
		oa_report_trig1 = 0x2740;
		oa_report_trig8 = 0x275c;
	}

	/* OASTARTTRIG[1-8] */
	for (i = oa_start_trig1; i <= oa_start_trig8; i += 4) {
		b_counters_regs[config.n_boolean_regs * 2] = i;
		b_counters_regs[config.n_boolean_regs * 2 + 1] = 0;
		config.n_boolean_regs++;
	}
	/* OAREPORTTRIG[1-8] */
	for (i = oa_report_trig1; i <= oa_report_trig8; i += 4) {
		b_counters_regs[config.n_boolean_regs * 2] = i;
		b_counters_regs[config.n_boolean_regs * 2 + 1] = 0;
		config.n_boolean_regs++;
	}
	config.boolean_regs_ptr = (uintptr_t) b_counters_regs;

	if (intel_gen(devid) >= 8) {
		/* Flex EU registers, only from Gen8+. */
		for (i = 0; i < ARRAY_SIZE(flex); i++) {
			flex_regs[config.n_flex_regs * 2] = flex[i];
			flex_regs[config.n_flex_regs * 2 + 1] = 0;
			config.n_flex_regs++;
		}
		config.flex_regs_ptr = (uintptr_t) flex_regs;
	}

	/* Mux registers (too many of them, just checking bounds) */
	i = 0;

	/* NOA_WRITE */
	mux_regs[i++] = 0x9888;
	mux_regs[i++] = 0;

	if (IS_HASWELL(devid)) {
		/* Haswell specific. undocumented... */
		mux_regs[i++] = 0x9ec0;
		mux_regs[i++] = 0;

		mux_regs[i++] = 0x25100;
		mux_regs[i++] = 0;
		mux_regs[i++] = 0x2ff90;
		mux_regs[i++] = 0;
	}

	if (intel_gen(devid) >= 8 && !IS_CHERRYVIEW(devid)) {
		/* NOA_CONFIG */
		mux_regs[i++] = 0xD04;
		mux_regs[i++] = 0;
		mux_regs[i++] = 0xD2C;
		mux_regs[i++] = 0;
		/* WAIT_FOR_RC6_EXIT */
		mux_regs[i++] = 0x20CC;
		mux_regs[i++] = 0;
	}

	if (IS_CHERRYVIEW(devid)) {
		/* Cherryview specific. undocumented... */
		mux_regs[i++] = 0x182300;
		mux_regs[i++] = 0;
		mux_regs[i++] = 0x1823A4;
		mux_regs[i++] = 0;
	}

	if (intel_gen(devid) <= 11) {
		/* HALF_SLICE_CHICKEN2 (shared with kernel workaround) */
		mux_regs[i++] = 0xE180;
		mux_regs[i++] = 0;
		/* PERFCNT[12] */
		mux_regs[i++] = 0x91B8;
		mux_regs[i++] = 0;
		/* PERFMATRIX */
		mux_regs[i++] = 0x91C8;
		mux_regs[i++] = 0;
	}

	config.mux_regs_ptr = (uintptr_t) mux_regs;
	config.n_mux_regs = i / 2;

	/* Create a new config */
	ret = igt_ioctl(drm_fd, DRM_IOCTL_I915_PERF_ADD_CONFIG, &config);
	igt_assert(ret > 0); /* Config 0 should be used by the kernel */
	config_id = ret;

	i915_perf_remove_config(drm_fd, config_id);
}

static unsigned
read_i915_module_ref(void)
{
	FILE *fp = fopen("/proc/modules", "r");
	char *line = NULL;
	size_t line_buf_size = 0;
	int len = 0;
	unsigned ref_count;

	igt_assert(fp);

	while ((len = getline(&line, &line_buf_size, fp)) > 0) {
		if (strncmp(line, "i915 ", 5) == 0) {
			unsigned long mem;
			int ret = sscanf(line + 5, "%lu %u", &mem, &ref_count);
			igt_assert(ret == 2);
			goto done;
		}
	}

	igt_assert(!"reached");

done:
	free(line);
	fclose(fp);
	return ref_count;
}

/* check that an open i915 perf stream holds a reference on the drm i915 module
 * including in the corner case where the original drm fd has been closed.
 */
static void
test_i915_ref_count(void)
{
	uint64_t properties[] = {
		/* Include OA reports in samples */
		DRM_I915_PERF_PROP_SAMPLE_OA, true,

		/* OA unit configuration */
		DRM_I915_PERF_PROP_OA_METRICS_SET, 0 /* updated below */,
		DRM_I915_PERF_PROP_OA_FORMAT, 0, /* update below */
		DRM_I915_PERF_PROP_OA_EXPONENT, 0, /* update below */
	};
	struct drm_i915_perf_open_param param = {
		.flags = I915_PERF_FLAG_FD_CLOEXEC,
		.num_properties = sizeof(properties) / 16,
		.properties_ptr = to_user_pointer(properties),
	};
	unsigned baseline, ref_count0, ref_count1;
	uint32_t oa_report0[64];
	uint32_t oa_report1[64];

	/* This should be the first test before the first fixture so no drm_fd
	 * should have been opened so far...
	 */
	igt_assert_eq(drm_fd, -1);

	baseline = read_i915_module_ref();
	igt_debug("baseline ref count (drm fd closed) = %u\n", baseline);

	drm_fd = __drm_open_driver(DRIVER_INTEL);
	devid = intel_get_drm_devid(drm_fd);
	sysfs = igt_sysfs_open(drm_fd);

	/* Note: these global variables are only initialized after calling
	 * init_sys_info()...
	 */
	igt_require(init_sys_info());
	properties[3] = test_set->perf_oa_metrics_set;
	properties[5] = test_set->perf_oa_format;
	properties[7] = oa_exp_1_millisec;

	ref_count0 = read_i915_module_ref();
	igt_debug("initial ref count with drm_fd open = %u\n", ref_count0);
	igt_assert(ref_count0 > baseline);

	stream_fd = __perf_open(drm_fd, &param, false);
	ref_count1 = read_i915_module_ref();
	igt_debug("ref count after opening i915 perf stream = %u\n", ref_count1);
	igt_assert(ref_count1 > ref_count0);

	close(drm_fd);
	close(sysfs);
	drm_fd = -1;
	sysfs = -1;
	ref_count0 = read_i915_module_ref();
	igt_debug("ref count after closing drm fd = %u\n", ref_count0);

	igt_assert(ref_count0 > baseline);

	read_2_oa_reports(test_set->perf_oa_format,
			  oa_exp_1_millisec,
			  oa_report0,
			  oa_report1,
			  false); /* not just timer reports */

	__perf_close(stream_fd);
	ref_count0 = read_i915_module_ref();
	igt_debug("ref count after closing i915 perf stream fd = %u\n", ref_count0);
	igt_assert_eq(ref_count0, baseline);
}

static void
test_sysctl_defaults(void)
{
	int paranoid = read_u64_file("/proc/sys/dev/i915/perf_stream_paranoid");
	int max_freq = read_u64_file("/proc/sys/dev/i915/oa_max_sample_rate");

	igt_assert_eq(paranoid, 1);
	igt_assert_eq(max_freq, 100000);
}

static int i915_perf_revision(int fd)
{
	drm_i915_getparam_t gp;
	int value = 1, ret;

	gp.param = I915_PARAM_PERF_REVISION;
	gp.value = &value;
	ret = igt_ioctl(drm_fd, DRM_IOCTL_I915_GETPARAM, &gp);
	if (ret == -1) {
		/* If the param is missing, consider version 1. */
		igt_assert_eq(errno, EINVAL);
		return 1;
	}

	return value;
}

igt_main
{
	igt_fixture {
		struct stat sb;

		igt_require(stat("/proc/sys/dev/i915/perf_stream_paranoid", &sb)
			    == 0);
		igt_require(stat("/proc/sys/dev/i915/oa_max_sample_rate", &sb)
			    == 0);
	}

	igt_subtest("i915-ref-count")
		test_i915_ref_count();

	igt_subtest("sysctl-defaults")
		test_sysctl_defaults();

	igt_fixture {
		/* We expect that the ref count test before these fixtures
		 * should have closed drm_fd...
		 */
		igt_assert_eq(drm_fd, -1);

		/* Avoid the normal exithandler, our perf-fd interferes */
		drm_fd = __drm_open_driver(DRIVER_INTEL);
		igt_require_gem(drm_fd);

		devid = intel_get_drm_devid(drm_fd);
		sysfs = igt_sysfs_open(drm_fd);

		igt_require(init_sys_info());

		write_u64_file("/proc/sys/dev/i915/perf_stream_paranoid", 1);
		write_u64_file("/proc/sys/dev/i915/oa_max_sample_rate", 100000);

		gt_max_freq_mhz = sysfs_read("gt_boost_freq_mhz");

		render_copy = igt_get_render_copyfunc(devid);
		igt_require_f(render_copy, "no render-copy function\n");
	}

	igt_subtest("non-system-wide-paranoid")
		test_system_wide_paranoid();

	igt_subtest("invalid-open-flags")
		test_invalid_open_flags();

	igt_subtest("invalid-oa-metric-set-id")
		test_invalid_oa_metric_set_id();

	igt_subtest("invalid-oa-format-id")
		test_invalid_oa_format_id();

	igt_subtest("missing-sample-flags")
		test_missing_sample_flags();

	igt_subtest("oa-formats")
		test_oa_formats();

	igt_subtest("invalid-oa-exponent")
		test_invalid_oa_exponent();
	igt_subtest("low-oa-exponent-permissions")
		test_low_oa_exponent_permissions();
	igt_subtest("oa-exponents")
		test_oa_exponents();

	igt_subtest("per-context-mode-unprivileged") {
		igt_require(IS_HASWELL(devid));
		test_per_context_mode_unprivileged();
	}

	igt_subtest("buffer-fill")
		test_buffer_fill();

	igt_subtest("disabled-read-error")
		test_disabled_read_error();
	igt_subtest("non-sampling-read-error")
		test_non_sampling_read_error();

	igt_subtest("enable-disable")
		test_enable_disable();

	igt_describe("Test blocking read with default hrtimer frequency");
	igt_subtest("blocking") {
		test_blocking(40 * 1000 * 1000 /* 40ms oa period */,
			      false /* set_kernel_hrtimer */,
			      5 * 1000 * 1000 /* default 5ms/200Hz hrtimer */);
	}

	igt_describe("Test blocking read with different hrtimer frequencies");
	igt_subtest("blocking-parameterized") {
		igt_require(i915_perf_revision(drm_fd) >= 5);

		test_blocking(10 * 1000 * 1000 /* 10ms oa period */,
			      true /* set_kernel_hrtimer */,
			      40 * 1000 * 1000 /* default 40ms hrtimer */);
		test_blocking(500 * 1000 /* 500us oa period */,
			      true /* set_kernel_hrtimer */,
			      2 * 1000 * 1000 /* default 2ms hrtimer */);
	}

	igt_describe("Test polled read with default hrtimer frequency");
	igt_subtest("polling") {
		test_polling(40 * 1000 * 1000 /* 40ms oa period */,
			     false /* set_kernel_hrtimer */,
			     5 * 1000 * 1000 /* default 5ms/200Hz hrtimer */);
	}

	igt_describe("Test polled read with different hrtimer frequencies");
	igt_subtest("polling-parameterized") {
		igt_require(i915_perf_revision(drm_fd) >= 5);

		test_polling(10 * 1000 * 1000 /* 10ms oa period */,
			     true /* set_kernel_hrtimer */,
			     40 * 1000 * 1000 /* default 40ms hrtimer */);
		test_polling(500 * 1000 /* 500us oa period */,
			     true /* set_kernel_hrtimer */,
			     2 * 1000 * 1000 /* default 2ms hrtimer */);
	}

	igt_describe("Test polled read with buffer size smaller than available data");
	igt_subtest("polling-small-buf")
		test_polling_small_buf();

	igt_subtest("short-reads")
		test_short_reads();

	igt_subtest("mi-rpc") {
		igt_require(intel_gen(devid) < 12);
		test_mi_rpc();
	}

	igt_subtest("unprivileged-single-ctx-counters") {
		igt_require(IS_HASWELL(devid));
		hsw_test_single_ctx_counters();
	}

	igt_subtest("gen8-unprivileged-single-ctx-counters") {
		/* For Gen8+ the OA unit can no longer be made to clock gate
		 * for a specific context. Additionally the partial-replacement
		 * functionality to HW filter timer reports for a specific
		 * context (SKL+) can't stop multiple applications viewing
		 * system-wide data via MI_REPORT_PERF_COUNT commands.
		 *
		 * For gen12 implement a separate test that uses only OAR
		 */
		igt_require(intel_gen(devid) >= 8 && intel_gen(devid) < 12);
		gen8_test_single_ctx_render_target_writes_a_counter();
	}

	igt_subtest_group {
		igt_fixture igt_require(intel_gen(devid) >= 12);

		igt_describe("Test MI REPORT PERF COUNT for Gen 12");
		igt_subtest("gen12-mi-rpc")
			gen12_test_mi_rpc();

		igt_describe("Test OA TLB invalidate");
		igt_subtest("gen12-oa-tlb-invalidate")
			gen12_test_oa_tlb_invalidate();

		igt_describe("Measure performance for a specific context using OAR in Gen 12");
		igt_subtest("gen12-unprivileged-single-ctx-counters")
			gen12_test_single_ctx_render_target_writes_a_counter();
	}

	igt_subtest("rc6-disable")
		test_rc6_disable();

	igt_describe("Stress tests opening & closing the i915-perf stream in a busy loop");
	igt_subtest("stress-open-close")
		test_stress_open_close();

	igt_subtest_group {
		igt_fixture {
			igt_require(i915_perf_revision(drm_fd) >= 4);
		}

		igt_describe("Verify invalid SSEU opening parameters");
		igt_subtest("global-sseu-config-invalid")
			test_global_sseu_config_invalid();

		igt_describe("Verify specifying SSEU opening parameters");
		igt_subtest("global-sseu-config")
			test_global_sseu_config();
	}

	igt_subtest("invalid-create-userspace-config")
		test_invalid_create_userspace_config();

	igt_subtest("invalid-remove-userspace-config")
		test_invalid_remove_userspace_config();

	igt_subtest("create-destroy-userspace-config")
		test_create_destroy_userspace_config();

	igt_subtest("whitelisted-registers-userspace-config")
		test_whitelisted_registers_userspace_config();

	igt_fixture {
		/* leave sysctl options in their default state... */
		write_u64_file("/proc/sys/dev/i915/oa_max_sample_rate", 100000);
		write_u64_file("/proc/sys/dev/i915/perf_stream_paranoid", 1);

		if (intel_perf)
			intel_perf_free(intel_perf);

		close(drm_fd);
	}
}
