/*
 * Copyright © 2007-2018 Intel Corporation
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
 *
 * Authors:
 *    Eric Anholt <eric@anholt.net>
 *    Eugeni Dodonov <eugeni.dodonov@intel.com>
 */

#include <stdio.h>
#include <sys/types.h>
#include <dirent.h>
#include <stdint.h>
#include <assert.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <inttypes.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <math.h>
#include <locale.h>

#include "igt_perf.h"

struct pmu_pair {
	uint64_t cur;
	uint64_t prev;
};

struct pmu_counter {
	bool present;
	uint64_t config;
	unsigned int idx;
	struct pmu_pair val;
};

struct engine {
	const char *name;
	const char *display_name;

	unsigned int class;
	unsigned int instance;

	unsigned int num_counters;

	struct pmu_counter busy;
	struct pmu_counter wait;
	struct pmu_counter sema;
};

struct engines {
	unsigned int num_engines;
	unsigned int num_counters;
	DIR *root;
	int fd;
	struct pmu_pair ts;

	int rapl_fd;
	double rapl_scale;
	const char *rapl_unit;

	int imc_fd;
	double imc_reads_scale;
	const char *imc_reads_unit;
	double imc_writes_scale;
	const char *imc_writes_unit;

	struct pmu_counter freq_req;
	struct pmu_counter freq_act;
	struct pmu_counter irq;
	struct pmu_counter rc6;
	struct pmu_counter rapl;
	struct pmu_counter imc_reads;
	struct pmu_counter imc_writes;

	struct engine engine;
};

static uint64_t
get_pmu_config(int dirfd, const char *name, const char *counter)
{
	char buf[128], *p;
	int fd, ret;

	ret = snprintf(buf, sizeof(buf), "%s-%s", name, counter);
	if (ret < 0 || ret == sizeof(buf))
		return -1;

	fd = openat(dirfd, buf, O_RDONLY);
	if (fd < 0)
		return -1;

	ret = read(fd, buf, sizeof(buf));
	close(fd);
	if (ret <= 0)
		return -1;

	p = index(buf, '0');
	if (!p)
		return -1;

	return strtoul(p, NULL, 0);
}

#define engine_ptr(engines, n) (&engines->engine + (n))

static const char *class_display_name(unsigned int class)
{
	switch (class) {
	case I915_ENGINE_CLASS_RENDER:
		return "Render/3D";
	case I915_ENGINE_CLASS_COPY:
		return "Blitter";
	case I915_ENGINE_CLASS_VIDEO:
		return "Video";
	case I915_ENGINE_CLASS_VIDEO_ENHANCE:
		return "VideoEnhance";
	default:
		return "[unknown]";
	}
}

static int engine_cmp(const void *__a, const void *__b)
{
	const struct engine *a = (struct engine *)__a;
	const struct engine *b = (struct engine *)__b;

	if (a->class != b->class)
		return a->class - b->class;
	else
		return a->instance - b->instance;
}

static struct engines *discover_engines(void)
{
	const char *sysfs_root = "/sys/devices/i915/events";
	struct engines *engines;
	struct dirent *dent;
	int ret = 0;
	DIR *d;

	engines = malloc(sizeof(struct engines));
	if (!engines)
		return NULL;

	memset(engines, 0, sizeof(*engines));

	engines->num_engines = 0;

	d = opendir(sysfs_root);
	if (!d)
		return NULL;

	while ((dent = readdir(d)) != NULL) {
		const char *endswith = "-busy";
		const unsigned int endlen = strlen(endswith);
		struct engine *engine =
				engine_ptr(engines, engines->num_engines);
		char buf[256];

		if (dent->d_type != DT_REG)
			continue;

		if (strlen(dent->d_name) >= sizeof(buf)) {
			ret = ENAMETOOLONG;
			break;
		}

		strcpy(buf, dent->d_name);

		/* xxxN-busy */
		if (strlen(buf) < (endlen + 4))
			continue;
		if (strcmp(&buf[strlen(buf) - endlen], endswith))
			continue;

		memset(engine, 0, sizeof(*engine));

		buf[strlen(buf) - endlen] = 0;
		engine->name = strdup(buf);
		if (!engine->name) {
			ret = errno;
			break;
		}

		engine->busy.config = get_pmu_config(dirfd(d), engine->name,
						     "busy");
		if (engine->busy.config == -1) {
			ret = ENOENT;
			break;
		}

		engine->class = (engine->busy.config &
				 (__I915_PMU_OTHER(0) - 1)) >>
				I915_PMU_CLASS_SHIFT;

		engine->instance = (engine->busy.config >>
				    I915_PMU_SAMPLE_BITS) &
				    ((1 << I915_PMU_SAMPLE_INSTANCE_BITS) - 1);

		ret = snprintf(buf, sizeof(buf), "%s/%u",
			       class_display_name(engine->class),
			       engine->instance);
		if (ret < 0 || ret == sizeof(buf)) {
			ret = ENOBUFS;
			break;
		}
		ret = 0;

		engine->display_name = strdup(buf);
		if (!engine->display_name) {
			ret = errno;
			break;
		}

		engines->num_engines++;
		engines = realloc(engines, sizeof(struct engines) +
				  engines->num_engines * sizeof(struct engine));
		if (!engines) {
			ret = errno;
			break;
		}
	}

	if (ret) {
		free(engines);
		errno = ret;

		return NULL;
	}

	qsort(engine_ptr(engines, 0), engines->num_engines,
	      sizeof(struct engine), engine_cmp);

	engines->root = d;

	return engines;
}

static int
filename_to_buf(const char *filename, char *buf, unsigned int bufsize)
{
	int fd, err;
	ssize_t ret;

	fd = open(filename, O_RDONLY);
	if (fd < 0)
		return -1;

	ret = read(fd, buf, bufsize - 1);
	err = errno;
	close(fd);
	if (ret < 1) {
		errno = ret < 0 ? err : ENOMSG;

		return -1;
	}

	if (ret > 1 && buf[ret - 1] == '\n')
		buf[ret - 1] = '\0';
	else
		buf[ret] = '\0';

	return 0;
}

static uint64_t filename_to_u64(const char *filename, int base)
{
	char buf[64], *b;

	if (filename_to_buf(filename, buf, sizeof(buf)))
		return 0;

	/*
	 * Handle both single integer and key=value formats by skipping
	 * leading non-digits.
	 */
	b = buf;
	while (*b && !isdigit(*b))
		b++;

	return strtoull(b, NULL, base);
}

static double filename_to_double(const char *filename)
{
	char *oldlocale;
	char buf[80];
	double v;

	if (filename_to_buf(filename, buf, sizeof(buf)))
		return 0;

	oldlocale = setlocale(LC_ALL, "C");
	v = strtod(buf, NULL);
	setlocale(LC_ALL, oldlocale);

	return v;
}

#define RAPL_ROOT "/sys/devices/power/"
#define RAPL_EVENT "/sys/devices/power/events/"

static uint64_t rapl_type_id(void)
{
	return filename_to_u64(RAPL_ROOT "type", 10);
}

static uint64_t rapl_gpu_power(void)
{
	return filename_to_u64(RAPL_EVENT "energy-gpu", 0);
}

static double rapl_gpu_power_scale(void)
{
	return filename_to_double(RAPL_EVENT "energy-gpu.scale");
}

static const char *rapl_gpu_power_unit(void)
{
	char buf[32];

	if (filename_to_buf(RAPL_EVENT "energy-gpu.unit",
			    buf, sizeof(buf)) == 0)
		if (!strcmp(buf, "Joules"))
			return strdup("Watts");
		else
			return strdup(buf);
	else
		return NULL;
}

#define IMC_ROOT "/sys/devices/uncore_imc/"
#define IMC_EVENT "/sys/devices/uncore_imc/events/"

static uint64_t imc_type_id(void)
{
	return filename_to_u64(IMC_ROOT "type", 10);
}

static uint64_t imc_data_reads(void)
{
	return filename_to_u64(IMC_EVENT "data_reads", 0);
}

static double imc_data_reads_scale(void)
{
	return filename_to_double(IMC_EVENT "data_reads.scale");
}

static const char *imc_data_reads_unit(void)
{
	char buf[32];

	if (filename_to_buf(IMC_EVENT "data_reads.unit", buf, sizeof(buf)) == 0)
		return strdup(buf);
	else
		return NULL;
}

static uint64_t imc_data_writes(void)
{
	return filename_to_u64(IMC_EVENT "data_writes", 0);
}

static double imc_data_writes_scale(void)
{
	return filename_to_double(IMC_EVENT "data_writes.scale");
}

static const char *imc_data_writes_unit(void)
{
	char buf[32];

	if (filename_to_buf(IMC_EVENT "data_writes.unit",
			    buf, sizeof(buf)) == 0)
		return strdup(buf);
	else
		return NULL;
}

#define _open_pmu(cnt, pmu, fd) \
({ \
	int fd__; \
\
	fd__ = perf_i915_open_group((pmu)->config, (fd)); \
	if (fd__ >= 0) { \
		if ((fd) == -1) \
			(fd) = fd__; \
		(pmu)->present = true; \
		(pmu)->idx = (cnt)++; \
	} \
\
	fd__; \
})

#define _open_imc(cnt, pmu, fd) \
({ \
	int fd__; \
\
	fd__ = igt_perf_open_group(imc_type_id(), (pmu)->config, (fd)); \
	if (fd__ >= 0) { \
		if ((fd) == -1) \
			(fd) = fd__; \
		(pmu)->present = true; \
		(pmu)->idx = (cnt)++; \
	} \
\
	fd__; \
})

static int pmu_init(struct engines *engines)
{
	unsigned int i;
	int fd;

	engines->fd = -1;
	engines->num_counters = 0;

	engines->irq.config = I915_PMU_INTERRUPTS;
	fd = _open_pmu(engines->num_counters, &engines->irq, engines->fd);
	if (fd < 0)
		return -1;

	engines->freq_req.config = I915_PMU_REQUESTED_FREQUENCY;
	_open_pmu(engines->num_counters, &engines->freq_req, engines->fd);

	engines->freq_act.config = I915_PMU_ACTUAL_FREQUENCY;
	_open_pmu(engines->num_counters, &engines->freq_act, engines->fd);

	engines->rc6.config = I915_PMU_RC6_RESIDENCY;
	_open_pmu(engines->num_counters, &engines->rc6, engines->fd);

	for (i = 0; i < engines->num_engines; i++) {
		struct engine *engine = engine_ptr(engines, i);
		struct {
			struct pmu_counter *pmu;
			const char *counter;
		} *cnt, counters[] = {
			{ .pmu = &engine->busy, .counter = "busy" },
			{ .pmu = &engine->wait, .counter = "wait" },
			{ .pmu = &engine->sema, .counter = "sema" },
			{ .pmu = NULL, .counter = NULL },
		};

		for (cnt = counters; cnt->pmu; cnt++) {
			if (!cnt->pmu->config)
				cnt->pmu->config =
					get_pmu_config(dirfd(engines->root),
						       engine->name,
						       cnt->counter);
			fd = _open_pmu(engines->num_counters, cnt->pmu,
				       engines->fd);
			if (fd >= 0)
				engine->num_counters++;
		}
	}

	engines->rapl_fd = -1;
	if (rapl_type_id()) {
		engines->rapl_scale = rapl_gpu_power_scale();
		engines->rapl_unit = rapl_gpu_power_unit();
		if (!engines->rapl_unit)
			return -1;

		engines->rapl.config = rapl_gpu_power();
		if (!engines->rapl.config)
			return -1;

		engines->rapl_fd = igt_perf_open(rapl_type_id(),
						 engines->rapl.config);
		if (engines->rapl_fd < 0)
			return -1;

		engines->rapl.present = true;
	}

	engines->imc_fd = -1;
	if (imc_type_id()) {
		unsigned int num = 0;

		engines->imc_reads_scale = imc_data_reads_scale();
		engines->imc_writes_scale = imc_data_writes_scale();

		engines->imc_reads_unit = imc_data_reads_unit();
		if (!engines->imc_reads_unit)
			return -1;

		engines->imc_writes_unit = imc_data_writes_unit();
		if (!engines->imc_writes_unit)
			return -1;

		engines->imc_reads.config = imc_data_reads();
		if (!engines->imc_reads.config)
			return -1;

		engines->imc_writes.config = imc_data_writes();
		if (!engines->imc_writes.config)
			return -1;

		fd = _open_imc(num, &engines->imc_reads, engines->imc_fd);
		if (fd < 0)
			return -1;
		fd = _open_imc(num, &engines->imc_writes, engines->imc_fd);
		if (fd < 0)
			return -1;

		engines->imc_reads.present = true;
		engines->imc_writes.present = true;
	}

	return 0;
}

static uint64_t pmu_read_multi(int fd, unsigned int num, uint64_t *val)
{
	uint64_t buf[2 + num];
	unsigned int i;
	ssize_t len;

	memset(buf, 0, sizeof(buf));

	len = read(fd, buf, sizeof(buf));
	assert(len == sizeof(buf));

	for (i = 0; i < num; i++)
		val[i] = buf[2 + i];

	return buf[1];
}

static double __pmu_calc(struct pmu_pair *p, double d, double t, double s)
{
	double v;

	v = p->cur - p->prev;
	v /= d;
	v /= t;
	v *= s;

	if (s == 100.0 && v > 100.0)
		v = 100.0;

	return v;
}

static void fill_str(char *buf, unsigned int bufsz, char c, unsigned int num)
{
	unsigned int i;

	for (i = 0; i < num && i < (bufsz - 1); i++)
		*buf++ = c;

	*buf = 0;
}

static void pmu_calc(struct pmu_counter *cnt,
		     char *buf, unsigned int bufsz,
		     unsigned int width, unsigned width_dec,
		     double d, double t, double s)
{
	double val;
	int len;

	assert(bufsz >= (width + width_dec + 1));

	if (!cnt->present) {
		fill_str(buf, bufsz, '-', width + width_dec);
		return;
	}

	val = __pmu_calc(&cnt->val, d, t, s);

	len = snprintf(buf, bufsz, "%*.*f", width + width_dec, width_dec, val);
	if (len < 0 || len == bufsz) {
		fill_str(buf, bufsz, 'X', width + width_dec);
		return;
	}
}

static uint64_t __pmu_read_single(int fd, uint64_t *ts)
{
	uint64_t data[2] = { };
	ssize_t len;

	len = read(fd, data, sizeof(data));
	assert(len == sizeof(data));

	if (ts)
		*ts = data[1];

	return data[0];
}

static uint64_t pmu_read_single(int fd)
{
	return __pmu_read_single(fd, NULL);
}

static void __update_sample(struct pmu_counter *counter, uint64_t val)
{
	counter->val.prev = counter->val.cur;
	counter->val.cur = val;
}

static void update_sample(struct pmu_counter *counter, uint64_t *val)
{
	if (counter->present)
		__update_sample(counter, val[counter->idx]);
}

static void pmu_sample(struct engines *engines)
{
	const int num_val = engines->num_counters;
	uint64_t val[2 + num_val];
	unsigned int i;

	engines->ts.prev = engines->ts.cur;

	if (engines->rapl_fd >= 0)
		__update_sample(&engines->rapl,
				pmu_read_single(engines->rapl_fd));

	if (engines->imc_fd >= 0) {
		pmu_read_multi(engines->imc_fd, 2, val);
		update_sample(&engines->imc_reads, val);
		update_sample(&engines->imc_writes, val);
	}

	engines->ts.cur = pmu_read_multi(engines->fd, num_val, val);

	update_sample(&engines->freq_req, val);
	update_sample(&engines->freq_act, val);
	update_sample(&engines->irq, val);
	update_sample(&engines->rc6, val);

	for (i = 0; i < engines->num_engines; i++) {
		struct engine *engine = engine_ptr(engines, i);

		update_sample(&engine->busy, val);
		update_sample(&engine->sema, val);
		update_sample(&engine->wait, val);
	}
}

static const char *bars[] = { " ", "▏", "▎", "▍", "▌", "▋", "▊", "▉", "█" };

static void
print_percentage_bar(double percent, int max_len)
{
	int bar_len = percent * (8 * (max_len - 2)) / 100.0;
	int i;

	putchar('|');

	for (i = bar_len; i >= 8; i -= 8)
		printf("%s", bars[8]);
	if (i)
		printf("%s", bars[i]);

	for (i = 0; i < (max_len - 2 - (bar_len + 7) / 8); i++)
		putchar(' ');

	putchar('|');
}

#define DEFAULT_PERIOD_MS (1000)

static void
usage(const char *appname)
{
	printf("intel_gpu_top - Display a top-like summary of Intel GPU usage\n"
		"\n"
		"Usage: %s [parameters]\n"
		"\n"
		"\tThe following parameters are optional:\n\n"
		"\t[-s <ms>]       Refresh period in milliseconds (default %ums).\n"
		"\t[-h]            Show this help text.\n"
		"\n",
		appname, DEFAULT_PERIOD_MS);
}

int main(int argc, char **argv)
{
	unsigned int period_us = DEFAULT_PERIOD_MS * 1000;
	int con_w = -1, con_h = -1;
	struct engines *engines;
	unsigned int i;
	int ret, ch;

	/* Parse options */
	while ((ch = getopt(argc, argv, "s:h")) != -1) {
		switch (ch) {
		case 's':
			period_us = atoi(optarg) * 1000;
			break;
		case 'h':
			usage(argv[0]);
			exit(0);
		default:
			fprintf(stderr, "Invalid option %c!\n", (char)optopt);
			usage(argv[0]);
			exit(1);
		}
	}

	engines = discover_engines();
	if (!engines) {
		fprintf(stderr,
			"Failed to detect engines! (%s)\n(Kernel 4.16 or newer is required for i915 PMU support.)\n",
			strerror(errno));
		return 1;
	}

	ret = pmu_init(engines);
	if (ret) {
		fprintf(stderr,
			"Failed to initialize PMU! (%s)\n", strerror(errno));
		return 1;
	}

	pmu_sample(engines);

	for (;;) {
		double t;
#define BUFSZ 16
		char freq[BUFSZ];
		char fact[BUFSZ];
		char irq[BUFSZ];
		char rc6[BUFSZ];
		char power[BUFSZ];
		char reads[BUFSZ];
		char writes[BUFSZ];
		struct winsize ws;
		int lines = 0;

		/* Update terminal size. */
		if (ioctl(0, TIOCGWINSZ, &ws) != -1) {
			con_w = ws.ws_col;
			con_h = ws.ws_row;
		}

		pmu_sample(engines);
		t = (double)(engines->ts.cur - engines->ts.prev) / 1e9;

		printf("\033[H\033[J");

		pmu_calc(&engines->freq_req, freq, BUFSZ, 4, 0, 1.0, t, 1);
		pmu_calc(&engines->freq_act, fact, BUFSZ, 4, 0, 1.0, t, 1);
		pmu_calc(&engines->irq, irq, BUFSZ, 8, 0, 1.0, t, 1);
		pmu_calc(&engines->rc6, rc6, BUFSZ, 3, 0, 1e9, t, 100);
		pmu_calc(&engines->rapl, power, BUFSZ, 4, 2, 1.0, t,
			 engines->rapl_scale);
		pmu_calc(&engines->imc_reads, reads, BUFSZ, 6, 0, 1.0, t,
			 engines->imc_reads_scale);
		pmu_calc(&engines->imc_writes, writes, BUFSZ, 6, 0, 1.0, t,
			 engines->imc_writes_scale);

		if (lines++ < con_h)
			printf("intel-gpu-top - %s/%s MHz;  %s%% RC6; %s %s; %s irqs/s\n",
			       fact, freq, rc6, power, engines->rapl_unit, irq);

		if (lines++ < con_h)
			printf("\n");

		if (engines->imc_fd) {
			if (lines++ < con_h)
				printf("      IMC reads:   %s %s/s\n",
				       reads, engines->imc_reads_unit);

			if (lines++ < con_h)
				printf("     IMC writes:   %s %s/s\n",
				       writes, engines->imc_writes_unit);

			if (++lines < con_h)
				printf("\n");
		}

		for (i = 0; i < engines->num_engines; i++) {
			struct engine *engine = engine_ptr(engines, i);

			if (engine->num_counters && lines < con_h) {
				const char *a = "          ENGINE      BUSY ";
				const char *b = " MI_SEMA MI_WAIT";

				printf("\033[7m%s%*s%s\033[0m\n",
				       a,
				       (int)(con_w - 1 - strlen(a) - strlen(b)),
				       " ", b);
				lines++;
				break;
			}
		}

		for (i = 0; i < engines->num_engines && lines < con_h; i++) {
			struct engine *engine = engine_ptr(engines, i);
			unsigned int max_w = con_w - 1;
			unsigned int len;
			char sema[BUFSZ];
			char wait[BUFSZ];
			char busy[BUFSZ];
			char buf[128];
			double val;

			if (!engine->num_counters)
				continue;

			pmu_calc(&engine->sema, sema, BUFSZ, 3, 0, 1e9, t, 100);
			pmu_calc(&engine->wait, wait, BUFSZ, 3, 0, 1e9, t, 100);
			len = snprintf(buf, sizeof(buf), "    %s%%    %s%%",
				       sema, wait);

			pmu_calc(&engine->busy, busy, BUFSZ, 6, 2, 1e9, t,
				 100);
			len += printf("%16s %s%% ", engine->display_name, busy);

			val = __pmu_calc(&engine->busy.val, 1e9, t, 100);
			print_percentage_bar(val, max_w - len);

			printf("%s\n", buf);

			lines++;
		}

		if (lines++ < con_h)
			printf("\n");

		usleep(period_us);
	}

	return 0;
}
