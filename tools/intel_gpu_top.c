/*
 * Copyright © 2007-2021 Intel Corporation
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

#include "igt_device_scan.h"

#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <termios.h>

#include "igt_perf.h"

#define ARRAY_SIZE(arr) (sizeof(arr)/sizeof(arr[0]))

struct pmu_pair {
	uint64_t cur;
	uint64_t prev;
};

struct pmu_counter {
	uint64_t type;
	uint64_t config;
	unsigned int idx;
	struct pmu_pair val;
	double scale;
	const char *units;
	bool present;
};

struct engine {
	const char *name;
	char *display_name;
	char *short_name;

	unsigned int class;
	unsigned int instance;

	unsigned int num_counters;

	struct pmu_counter busy;
	struct pmu_counter wait;
	struct pmu_counter sema;
};

struct engine_class {
	unsigned int class;
	const char *name;
	unsigned int num_engines;
};

struct engines {
	unsigned int num_engines;
	unsigned int num_classes;
	struct engine_class *class;
	unsigned int num_counters;
	DIR *root;
	int fd;
	struct pmu_pair ts;

	int rapl_fd;
	struct pmu_counter r_gpu, r_pkg;
	unsigned int num_rapl;

	int imc_fd;
	struct pmu_counter imc_reads;
	struct pmu_counter imc_writes;
	unsigned int num_imc;

	struct pmu_counter freq_req;
	struct pmu_counter freq_act;
	struct pmu_counter irq;
	struct pmu_counter rc6;

	bool discrete;
	char *device;

	/* Do not edit below this line.
	 * This structure is reallocated every time a new engine is
	 * found and size is increased by sizeof (engine).
	 */

	struct engine engine;

};

__attribute__((format(scanf,3,4)))
static int igt_sysfs_scanf(int dir, const char *attr, const char *fmt, ...)
{
	FILE *file;
	int fd;
	int ret = -1;

	fd = openat(dir, attr, O_RDONLY);
	if (fd < 0)
		return -1;

	file = fdopen(fd, "r");
	if (file) {
		va_list ap;

		va_start(ap, fmt);
		ret = vfscanf(file, fmt, ap);
		va_end(ap);

		fclose(file);
	} else {
		close(fd);
	}

	return ret;
}

static int pmu_parse(struct pmu_counter *pmu, const char *path, const char *str)
{
	locale_t locale, oldlocale;
	bool result = true;
	char buf[128] = {};
	int dir;

	dir = open(path, O_RDONLY);
	if (dir < 0)
		return -errno;

	/* Replace user environment with plain C to match kernel format */
	locale = newlocale(LC_ALL, "C", 0);
	oldlocale = uselocale(locale);

	result &= igt_sysfs_scanf(dir, "type", "%"PRIu64, &pmu->type) == 1;

	snprintf(buf, sizeof(buf) - 1, "events/%s", str);
	result &= igt_sysfs_scanf(dir, buf, "event=%"PRIx64, &pmu->config) == 1;

	snprintf(buf, sizeof(buf) - 1, "events/%s.scale", str);
	result &= igt_sysfs_scanf(dir, buf, "%lf", &pmu->scale) == 1;

	snprintf(buf, sizeof(buf) - 1, "events/%s.unit", str);
	result &= igt_sysfs_scanf(dir, buf, "%127s", buf) == 1;
	pmu->units = strdup(buf);

	uselocale(oldlocale);
	freelocale(locale);

	close(dir);

	if (!result)
		return -EINVAL;

	if (isnan(pmu->scale) || !pmu->scale)
		return -ERANGE;

	return 0;
}

static int rapl_parse(struct pmu_counter *pmu, const char *str)
{
	const char *expected_units = "Joules";
	int err;

	err = pmu_parse(pmu, "/sys/devices/power", str);
	if (err < 0)
		return err;

	if (!pmu->units || strcmp(pmu->units, expected_units)) {
		fprintf(stderr,
			"Unexpected units for RAPL %s: found '%s', expected '%s'\n",
			str, pmu->units, expected_units);
	}

	return 0;
}

static void
rapl_open(struct pmu_counter *pmu,
	  const char *domain,
	  struct engines *engines)
{
	int fd;

	if (rapl_parse(pmu, domain) < 0)
		return;

	fd = igt_perf_open_group(pmu->type, pmu->config, engines->rapl_fd);
	if (fd < 0)
		return;

	if (engines->rapl_fd == -1)
		engines->rapl_fd = fd;

	pmu->idx = engines->num_rapl++;
	pmu->present = true;
}

static void gpu_power_open(struct pmu_counter *pmu,
			   struct engines *engines)
{
	rapl_open(pmu, "energy-gpu", engines);
}

static void pkg_power_open(struct pmu_counter *pmu,
			   struct engines *engines)
{
	rapl_open(pmu, "energy-pkg", engines);
}

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

static const char *class_short_name(unsigned int class)
{
	switch (class) {
	case I915_ENGINE_CLASS_RENDER:
		return "RCS";
	case I915_ENGINE_CLASS_COPY:
		return "BCS";
	case I915_ENGINE_CLASS_VIDEO:
		return "VCS";
	case I915_ENGINE_CLASS_VIDEO_ENHANCE:
		return "VECS";
	default:
		return "UNKN";
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

#define is_igpu_pci(x) (strcmp(x, "0000:00:02.0") == 0)
#define is_igpu(x) (strcmp(x, "i915") == 0)

static struct engines *discover_engines(char *device)
{
	char sysfs_root[PATH_MAX];
	struct engines *engines;
	struct dirent *dent;
	int ret = 0;
	DIR *d;

	snprintf(sysfs_root, sizeof(sysfs_root),
		 "/sys/devices/%s/events", device);

	engines = malloc(sizeof(struct engines));
	if (!engines)
		return NULL;

	memset(engines, 0, sizeof(*engines));

	engines->num_engines = 0;
	engines->device = device;
	engines->discrete = !is_igpu(device);

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

		ret = asprintf(&engine->display_name, "%s/%u",
			       class_display_name(engine->class),
			       engine->instance);
		if (ret <= 0) {
			ret = errno;
			break;
		}

		ret = asprintf(&engine->short_name, "%s/%u",
			       class_short_name(engine->class),
			       engine->instance);
		if (ret <= 0) {
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

		ret = 0;
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

#define _open_pmu(type, cnt, pmu, fd) \
({ \
	int fd__; \
\
	fd__ = igt_perf_open_group((type), (pmu)->config, (fd)); \
	if (fd__ >= 0) { \
		if ((fd) == -1) \
			(fd) = fd__; \
		(pmu)->present = true; \
		(pmu)->idx = (cnt)++; \
	} \
\
	fd__; \
})

static int imc_parse(struct pmu_counter *pmu, const char *str)
{
	return pmu_parse(pmu, "/sys/devices/uncore_imc", str);
}

static void
imc_open(struct pmu_counter *pmu,
	 const char *domain,
	 struct engines *engines)
{
	int fd;

	if (imc_parse(pmu, domain) < 0)
		return;

	fd = igt_perf_open_group(pmu->type, pmu->config, engines->imc_fd);
	if (fd < 0)
		return;

	if (engines->imc_fd == -1)
		engines->imc_fd = fd;

	pmu->idx = engines->num_imc++;
	pmu->present = true;
}

static void imc_writes_open(struct pmu_counter *pmu, struct engines *engines)
{
	imc_open(pmu, "data_writes", engines);
}

static void imc_reads_open(struct pmu_counter *pmu, struct engines *engines)
{
	imc_open(pmu, "data_reads", engines);
}

static int pmu_init(struct engines *engines)
{
	unsigned int i;
	int fd;
	uint64_t type = igt_perf_type_id(engines->device);

	engines->fd = -1;
	engines->num_counters = 0;

	engines->irq.config = I915_PMU_INTERRUPTS;
	fd = _open_pmu(type, engines->num_counters, &engines->irq, engines->fd);
	if (fd < 0)
		return -1;

	engines->freq_req.config = I915_PMU_REQUESTED_FREQUENCY;
	_open_pmu(type, engines->num_counters, &engines->freq_req, engines->fd);

	engines->freq_act.config = I915_PMU_ACTUAL_FREQUENCY;
	_open_pmu(type, engines->num_counters, &engines->freq_act, engines->fd);

	engines->rc6.config = I915_PMU_RC6_RESIDENCY;
	_open_pmu(type, engines->num_counters, &engines->rc6, engines->fd);

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
			fd = _open_pmu(type, engines->num_counters, cnt->pmu,
				       engines->fd);
			if (fd >= 0)
				engine->num_counters++;
		}
	}

	engines->rapl_fd = -1;
	if (!engines->discrete) {
		gpu_power_open(&engines->r_gpu, engines);
		pkg_power_open(&engines->r_pkg, engines);
	}

	engines->imc_fd = -1;
	imc_reads_open(&engines->imc_reads, engines);
	imc_writes_open(&engines->imc_writes, engines);

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

static double pmu_calc(struct pmu_pair *p, double d, double t, double s)
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

	if (engines->num_rapl) {
		pmu_read_multi(engines->rapl_fd, engines->num_rapl, val);
		update_sample(&engines->r_gpu, val);
		update_sample(&engines->r_pkg, val);
	}

	if (engines->num_imc) {
		pmu_read_multi(engines->imc_fd, engines->num_imc, val);
		update_sample(&engines->imc_reads, val);
		update_sample(&engines->imc_writes, val);
	}
}

enum client_status {
	FREE = 0, /* mbz */
	ALIVE,
	PROBE
};

struct clients;

struct client {
	struct clients *clients;

	enum client_status status;
	int sysfs_root;
	int busy_root;
	unsigned int id;
	unsigned int pid;
	char name[24];
	char print_name[24];
	unsigned int samples;
	unsigned long total_runtime;
	unsigned long last_runtime;
	struct engines *engines;
	unsigned long *val;
	uint64_t *last;
};

struct clients {
	unsigned int num_clients;
	unsigned int active_clients;

	unsigned int num_classes;
	struct engine_class *class;

	char sysfs_root[128];

	struct client *client;
};

#define for_each_client(clients, c, tmp) \
	for ((tmp) = (clients)->num_clients, c = (clients)->client; \
	     (tmp > 0); (tmp)--, (c)++)

static struct clients *init_clients(const char *drm_card)
{
	struct clients *clients = malloc(sizeof(*clients));
	const char *slash;
	ssize_t ret;

	memset(clients, 0, sizeof(*clients));

	if (drm_card) {
		slash = rindex(drm_card, '/');
		assert(slash);
	} else {
		slash = "card0";
	}

	ret = snprintf(clients->sysfs_root, sizeof(clients->sysfs_root),
		       "/sys/class/drm/%s/clients/", slash);
	assert(ret > 0 && ret < sizeof(clients->sysfs_root));

	return clients;
}

static int __read_to_buf(int fd, char *buf, unsigned int bufsize)
{
	ssize_t ret;
	int err;

	ret = read(fd, buf, bufsize - 1);
	err = errno;
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

static int
__read_client_field(int root, const char *field, char *buf, unsigned int bufsize)
{
	int fd, ret;

	fd = openat(root, field, O_RDONLY);
	if (fd < 0)
		return -1;

	ret = __read_to_buf(fd, buf, bufsize);

	close(fd);

	return ret;
}

static uint64_t
read_client_busy(struct client *client, unsigned int class)
{
	const char *class_str[] = { "0", "1", "2", "3", "4", "5", "6", "7" };
	char buf[256], *b;
	int ret;

	assert(class < ARRAY_SIZE(class_str));
	if (class >= ARRAY_SIZE(class_str))
		return 0;

	assert(client->sysfs_root >= 0);
	if (client->sysfs_root < 0)
		return 0;

	if (client->busy_root < 0)
		client->busy_root = openat(client->sysfs_root, "busy",
					   O_RDONLY | O_DIRECTORY);

	assert(client->busy_root);
	if (client->busy_root < 0)
		return 0;

	ret = __read_client_field(client->busy_root, class_str[class], buf,
				  sizeof(buf));
	if (ret) {
		close(client->busy_root);
		client->busy_root = -1;
		return 0;
	}

	/*
	 * Handle both single integer and key=value formats by skipping
	 * leading non-digits.
	 */
	b = buf;
	while (*b && !isdigit(*b))
		b++;

	return strtoull(b, NULL, 10);
}

static struct client *
find_client(struct clients *clients, enum client_status status, unsigned int id)
{
	unsigned int start, num;
	struct client *c;

	start = status == FREE ? clients->active_clients : 0; /* Free block at the end. */
	num = clients->num_clients - start;

	for (c = &clients->client[start]; num; c++, num--) {
		if (status != c->status)
			continue;

		if (status == FREE || c->id == id)
			return c;
	}

	return NULL;
}

static void update_client(struct client *c, unsigned int pid, char *name)
{
	uint64_t val[c->clients->num_classes];
	unsigned int i;

	if (c->pid != pid)
		c->pid = pid;

	if (strcmp(c->name, name)) {
		char *p;

		strncpy(c->name, name, sizeof(c->name) - 1);
		strncpy(c->print_name, name, sizeof(c->print_name) - 1);

		p = c->print_name;
		while (*p) {
			if (!isprint(*p))
				*p = '*';
			p++;
		}
	}

	for (i = 0; i < c->clients->num_classes; i++)
		val[i] = read_client_busy(c, c->clients->class[i].class);

	c->last_runtime = 0;
	c->total_runtime = 0;

	for (i = 0; i < c->clients->num_classes; i++) {
		if (val[i] < c->last[i])
			continue; /* It will catch up soon. */

		c->total_runtime += val[i];
		c->val[i] = val[i] - c->last[i];
		c->last_runtime += c->val[i];
		c->last[i] = val[i];
	}

	c->samples++;
	c->status = ALIVE;
}

static void
add_client(struct clients *clients, unsigned int id, unsigned int pid,
	   char *name, int sysfs_root)
{
	struct client *c;

	assert(!find_client(clients, ALIVE, id));

	c = find_client(clients, FREE, 0);
	if (!c) {
		unsigned int idx = clients->num_clients;

		clients->num_clients += (clients->num_clients + 2) / 2;
		clients->client = realloc(clients->client,
					  clients->num_clients * sizeof(*c));
		assert(clients->client);

		c = &clients->client[idx];
		memset(c, 0, (clients->num_clients - idx) * sizeof(*c));
	}

	c->sysfs_root = sysfs_root;
	c->busy_root = -1;
	c->id = id;
	c->clients = clients;
	c->val = calloc(clients->num_classes, sizeof(c->val));
	c->last = calloc(clients->num_classes, sizeof(c->last));
	assert(c->val && c->last);

	update_client(c, pid, name);
}

static void free_client(struct client *c)
{
	if (c->sysfs_root >= 0)
		close(c->sysfs_root);
	if (c->busy_root >= 0)
		close(c->busy_root);
	free(c->val);
	free(c->last);
	memset(c, 0, sizeof(*c));
}

static int
read_client_sysfs(char *buf, int bufsize, const char *sysfs_root,
		  unsigned int id, const char *field, int *client_root)
{
	ssize_t ret;

	if (*client_root < 0) {
		char namebuf[256];

		ret = snprintf(namebuf, sizeof(namebuf), "%s/%u",
			       sysfs_root, id);
		assert(ret > 0 && ret < sizeof(namebuf));
		if (ret <= 0 || ret == sizeof(namebuf))
			return -1;

		*client_root = open(namebuf, O_RDONLY | O_DIRECTORY);
	}

	if (*client_root < 0)
		return -1;

	return __read_client_field(*client_root, field, buf, bufsize);
}

static int client_last_cmp(const void *_a, const void *_b)
{
	const struct client *a = _a;
	const struct client *b = _b;
	long tot_a, tot_b;

	/*
	 * Sort clients in descending order of runtime in the previous sampling
	 * period for active ones, followed by inactive. Tie-breaker is client
	 * id.
	 */

	tot_a = a->status == ALIVE ? a->last_runtime : -1;
	tot_b = b->status == ALIVE ? b->last_runtime : -1;

	tot_b -= tot_a;
	if (tot_b > 0)
		return 1;
	if (tot_b < 0)
		return -1;

	return (int)b->id - a->id;
}

static int client_total_cmp(const void *_a, const void *_b)
{
	const struct client *a = _a;
	const struct client *b = _b;
	long tot_a, tot_b;

	tot_a = a->status == ALIVE ? a->total_runtime : -1;
	tot_b = b->status == ALIVE ? b->total_runtime : -1;

	tot_b -= tot_a;
	if (tot_b > 0)
		return 1;
	if (tot_b < 0)
		return -1;

	return (int)b->id - a->id;
}

static int client_id_cmp(const void *_a, const void *_b)
{
	const struct client *a = _a;
	const struct client *b = _b;
	int id_a, id_b;

	id_a = a->status == ALIVE ? a->id : -1;
	id_b = b->status == ALIVE ? b->id : -1;

	id_b -= id_a;
	if (id_b > 0)
		return 1;
	if (id_b < 0)
		return -1;

	return (int)b->id - a->id;
}

static int (*client_cmp)(const void *, const void *) = client_last_cmp;

static void sort_clients(struct clients *clients)
{
	unsigned int active, free;
	struct client *c;
	int tmp;

	if (!clients)
		return;

	qsort(clients->client, clients->num_clients, sizeof(*clients->client),
	      client_cmp);

	/* Trim excessive array space. */
	active = 0;
	for_each_client(clients, c, tmp) {
		if (c->status != ALIVE)
			break; /* Active clients are first in the array. */
		active++;
	}

	clients->active_clients = active;

	free = clients->num_clients - active;
	if (free > clients->num_clients / 2) {
		active = clients->num_clients - free / 2;
		if (active != clients->num_clients) {
			clients->num_clients = active;
			clients->client = realloc(clients->client,
						  clients->num_clients *
						  sizeof(*c));
		}
	}
}

static void scan_clients(struct clients *clients)
{
	struct dirent *dent;
	struct client *c;
	unsigned int id;
	int tmp;
	DIR *d;

	if (!clients)
		return;

	for_each_client(clients, c, tmp) {
		assert(c->status != PROBE);
		if (c->status == ALIVE)
			c->status = PROBE;
		else
			break; /* Free block at the end of array. */
	}

	d = opendir(clients->sysfs_root);
	if (!d)
		return;

	while ((dent = readdir(d)) != NULL) {
		char name[24], pid[24];
		int ret, root = -1, *pr;

		if (dent->d_type != DT_DIR)
			continue;
		if (!isdigit(dent->d_name[0]))
			continue;

		id = atoi(dent->d_name);

		c = find_client(clients, PROBE, id);

		if (c)
			pr = &c->sysfs_root;
		else
			pr = &root;

		ret = read_client_sysfs(name, sizeof(name), clients->sysfs_root,
					id, "name", pr);
		ret |= read_client_sysfs(pid, sizeof(pid), clients->sysfs_root,
					id, "pid", pr);
		if (!ret) {
			if (!c)
				add_client(clients, id, atoi(pid), name, root);
			else
				update_client(c, atoi(pid), name);
		} else if (c) {
			c->status = PROBE; /* Will be deleted below. */
		}
	}

	closedir(d);

	for_each_client(clients, c, tmp) {
		if (c->status == PROBE)
			free_client(c);
		else if (c->status == FREE)
			break;
	}

	sort_clients(clients);
}

static const char *bars[] = { " ", "▏", "▎", "▍", "▌", "▋", "▊", "▉", "█" };

static void n_spaces(const unsigned int n)
{
	unsigned int i;

	for (i = 0; i < n; i++)
		putchar(' ');
}

static void
print_percentage_bar(double percent, int max_len, bool numeric)
{
	int bar_len, i, len = max_len - 2;
	const int w = 8;

	assert(max_len > 0);

	bar_len = ceil(percent * len / 100.0);
	if (bar_len > len)
		bar_len = len;
	bar_len *= w;

	putchar('|');

	for (i = bar_len; i >= w; i -= w)
		printf("%s", bars[w]);
	if (i)
		printf("%s", bars[i]);

	len -= (bar_len + (w - 1)) / w;
	n_spaces(len);

	putchar('|');

	if (numeric) {
		/*
		 * TODO: Finer grained reverse control to better preserve
		 * bar under numerical percentage.
		 */
		printf("\033[%uD\033[7m", max_len - 1);
		i = printf("%3.f%%", percent);
		printf("\033[%uC\033[0m", max_len - i - 1);
	}
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
		"\t[-h]            Show this help text.\n"
		"\t[-J]            Output JSON formatted data.\n"
		"\t[-l]            List plain text data.\n"
		"\t[-o <file|->]   Output to specified file or '-' for standard out.\n"
		"\t[-s <ms>]       Refresh period in milliseconds (default %ums).\n"
		"\t[-L]            List all cards.\n"
		"\t[-d <device>]   Device filter, please check manual page for more details.\n"
		"\n",
		appname, DEFAULT_PERIOD_MS);
	igt_device_print_filter_types();
}

static enum {
	INTERACTIVE,
	STDOUT,
	JSON
} output_mode;

struct cnt_item {
	struct pmu_counter *pmu;
	unsigned int fmt_width;
	unsigned int fmt_precision;
	double d;
	double t;
	double s;
	const char *name;
	const char *unit;

	/* Internal fields. */
	char buf[16];
};

struct cnt_group {
	const char *name;
	const char *display_name;
	struct cnt_item *items;
};

static unsigned int json_indent_level;

static const char *json_indent[] = {
	"",
	"\t",
	"\t\t",
	"\t\t\t",
	"\t\t\t\t",
	"\t\t\t\t\t",
};

static unsigned int json_prev_struct_members;
static unsigned int json_struct_members;

FILE *out;

static void
json_open_struct(const char *name)
{
	assert(json_indent_level < ARRAY_SIZE(json_indent));

	json_prev_struct_members = json_struct_members;
	json_struct_members = 0;

	if (name)
		fprintf(out, "%s%s\"%s\": {\n",
			json_prev_struct_members ? ",\n" : "",
			json_indent[json_indent_level],
			name);
	else
		fprintf(out, "%s\n%s{\n",
			json_prev_struct_members ? "," : "",
			json_indent[json_indent_level]);

	json_indent_level++;
}

static void
json_close_struct(void)
{
	assert(json_indent_level > 0);

	fprintf(out, "\n%s}", json_indent[--json_indent_level]);

	if (json_indent_level == 0)
		fflush(stdout);
}

static void
__json_add_member(const char *key, const char *val)
{
	assert(json_indent_level < ARRAY_SIZE(json_indent));

	fprintf(out, "%s%s\"%s\": \"%s\"",
		json_struct_members ? ",\n" : "",
		json_indent[json_indent_level], key, val);

	json_struct_members++;
}

static unsigned int
json_add_member(const struct cnt_group *parent, struct cnt_item *item,
		unsigned int headers)
{
	assert(json_indent_level < ARRAY_SIZE(json_indent));

	fprintf(out, "%s%s\"%s\": ",
		json_struct_members ? ",\n" : "",
		json_indent[json_indent_level], item->name);

	json_struct_members++;

	if (!strcmp(item->name, "unit"))
		fprintf(out, "\"%s\"", item->unit);
	else
		fprintf(out, "%f",
			pmu_calc(&item->pmu->val, item->d, item->t, item->s));

	return 1;
}

static unsigned int stdout_level;

#define STDOUT_HEADER_REPEAT 20
static unsigned int stdout_lines = STDOUT_HEADER_REPEAT;

static void
stdout_open_struct(const char *name)
{
	stdout_level++;
	assert(stdout_level > 0);
}

static void
stdout_close_struct(void)
{
	assert(stdout_level > 0);
	if (--stdout_level == 0) {
		stdout_lines++;
		fputs("\n", out);
		fflush(out);
	}
}

static unsigned int
stdout_add_member(const struct cnt_group *parent, struct cnt_item *item,
		  unsigned int headers)
{
	unsigned int fmt_tot = item->fmt_width + (item->fmt_precision ? 1 : 0);
	char buf[fmt_tot + 1];
	double val;
	int len;

	if (!item->pmu)
		return 0;
	else if (!item->pmu->present)
		return 0;

	if (headers == 1) {
		unsigned int grp_tot = 0;
		struct cnt_item *it;

		if (item != parent->items)
			return 0;

		for (it = parent->items; it->pmu; it++) {
			if (!it->pmu->present)
				continue;

			grp_tot += 1 + it->fmt_width +
				   (it->fmt_precision ? 1 : 0);
		}

		fprintf(out, "%*s ", grp_tot - 1, parent->display_name);
		return 0;
	} else if (headers == 2) {
		fprintf(out, "%*s ", fmt_tot, item->unit ?: item->name);
		return 0;
	}

	val = pmu_calc(&item->pmu->val, item->d, item->t, item->s);

	len = snprintf(buf, sizeof(buf), "%*.*f",
		       fmt_tot, item->fmt_precision, val);
	if (len < 0 || len == sizeof(buf))
		fill_str(buf, sizeof(buf), 'X', fmt_tot);

	len = fprintf(out, "%s ", buf);

	return len > 0 ? len : 0;
}

static void
term_open_struct(const char *name)
{
}

static void
term_close_struct(void)
{
}

static unsigned int
term_add_member(const struct cnt_group *parent, struct cnt_item *item,
		unsigned int headers)
{
	unsigned int fmt_tot = item->fmt_width + (item->fmt_precision ? 1 : 0);
	double val;
	int len;

	if (!item->pmu)
		return 0;

	assert(fmt_tot <= sizeof(item->buf));

	if (!item->pmu->present) {
		fill_str(item->buf, sizeof(item->buf), '-', fmt_tot);
		return 1;
	}

	val = pmu_calc(&item->pmu->val, item->d, item->t, item->s);
	len = snprintf(item->buf, sizeof(item->buf),
		       "%*.*f",
		       fmt_tot, item->fmt_precision, val);

	if (len < 0 || len == sizeof(item->buf))
		fill_str(item->buf, sizeof(item->buf), 'X', fmt_tot);

	return 1;
}

struct print_operations {
	void (*open_struct)(const char *name);
	void (*close_struct)(void);
	unsigned int (*add_member)(const struct cnt_group *parent,
				   struct cnt_item *item,
				   unsigned int headers);
	bool (*print_group)(struct cnt_group *group, unsigned int headers);
};

static const struct print_operations *pops;

static unsigned int
present_in_group(const struct cnt_group *grp)
{
	unsigned int present = 0;
	struct cnt_item *item;

	for (item = grp->items; item->name; item++) {
		if (item->pmu && item->pmu->present)
			present++;
	}

	return present;
}

static bool
print_group(struct cnt_group *grp, unsigned int headers)
{
	unsigned int consumed = 0;
	struct cnt_item *item;

	if (!present_in_group(grp))
		return false;

	pops->open_struct(grp->name);

	for (item = grp->items; item->name; item++)
		consumed += pops->add_member(grp, item, headers);

	pops->close_struct();

	return consumed;
}

static bool
term_print_group(struct cnt_group *grp, unsigned int headers)
{
	unsigned int consumed = 0;
	struct cnt_item *item;

	pops->open_struct(grp->name);

	for (item = grp->items; item->name; item++)
		consumed += pops->add_member(grp, item, headers);

	pops->close_struct();

	return consumed;
}

static const struct print_operations json_pops = {
	.open_struct = json_open_struct,
	.close_struct = json_close_struct,
	.add_member = json_add_member,
	.print_group = print_group,
};

static const struct print_operations stdout_pops = {
	.open_struct = stdout_open_struct,
	.close_struct = stdout_close_struct,
	.add_member = stdout_add_member,
	.print_group = print_group,
};

static const struct print_operations term_pops = {
	.open_struct = term_open_struct,
	.close_struct = term_close_struct,
	.add_member = term_add_member,
	.print_group = term_print_group,
};

static bool print_groups(struct cnt_group **groups)
{
	unsigned int headers = stdout_lines % STDOUT_HEADER_REPEAT + 1;
	bool print_data = true;

	if (output_mode == STDOUT && (headers == 1 || headers == 2)) {
		for (struct cnt_group **grp = groups; *grp; grp++)
			print_data = pops->print_group(*grp, headers);
	}

	for (struct cnt_group **grp = groups; print_data && *grp; grp++)
		pops->print_group(*grp, false);

	return print_data;
}

static int
print_header(const struct igt_device_card *card,
	     const char *codename,
	     struct engines *engines, double t,
	     int lines, int con_w, int con_h, bool *consumed)
{
	struct pmu_counter fake_pmu = {
		.present = true,
		.val.cur = 1,
	};
	struct cnt_item period_items[] = {
		{ &fake_pmu, 0, 0, 1.0, 1.0, t * 1e3, "duration" },
		{ NULL, 0, 0, 0.0, 0.0, 0.0, "unit", "ms" },
		{ },
	};
	struct cnt_group period_group = {
		.name = "period",
		.items = period_items,
	};
	struct cnt_item freq_items[] = {
		{ &engines->freq_req, 4, 0, 1.0, t, 1, "requested", "req" },
		{ &engines->freq_act, 4, 0, 1.0, t, 1, "actual", "act" },
		{ NULL, 0, 0, 0.0, 0.0, 0.0, "unit", "MHz" },
		{ },
	};
	struct cnt_group freq_group = {
		.name = "frequency",
		.display_name = "Freq MHz",
		.items = freq_items,
	};
	struct cnt_item irq_items[] = {
		{ &engines->irq, 8, 0, 1.0, t, 1, "count", "/s" },
		{ NULL, 0, 0, 0.0, 0.0, 0.0, "unit", "irq/s" },
		{ },
	};
	struct cnt_group irq_group = {
		.name = "interrupts",
		.display_name = "IRQ",
		.items = irq_items,
	};
	struct cnt_item rc6_items[] = {
		{ &engines->rc6, 3, 0, 1e9, t, 100, "value", "%" },
		{ NULL, 0, 0, 0.0, 0.0, 0.0, "unit", "%" },
		{ },
	};
	struct cnt_group rc6_group = {
		.name = "rc6",
		.display_name = "RC6",
		.items = rc6_items,
	};
	struct cnt_item power_items[] = {
		{ &engines->r_gpu, 4, 2, 1.0, t, engines->r_gpu.scale, "GPU", "gpu" },
		{ &engines->r_pkg, 4, 2, 1.0, t, engines->r_pkg.scale, "Package", "pkg" },
		{ NULL, 0, 0, 0.0, 0.0, 0.0, "unit", "W" },
		{ },
	};
	struct cnt_group power_group = {
		.name = "power",
		.display_name = "Power W",
		.items = power_items,
	};
	struct cnt_group *groups[] = {
		&period_group,
		&freq_group,
		&irq_group,
		&rc6_group,
		&power_group,
		NULL
	};

	if (output_mode != JSON)
		memmove(&groups[0], &groups[1],
			sizeof(groups) - sizeof(groups[0]));

	*consumed = print_groups(groups);

	if (output_mode == INTERACTIVE) {
		printf("\033[H\033[J");

		if (lines++ < con_h)
			printf("intel-gpu-top: %s @ %s\n",
			       codename, card->card);
		if (lines++ < con_h) {
			printf("%s/%s MHz; %s%% RC6; ",
			       freq_items[1].buf, freq_items[0].buf,
			       rc6_items[0].buf);
			if (engines->r_gpu.present) {
				printf("%s/%s W; ",
				       power_items[0].buf,
				       power_items[1].buf);
			}
			printf("%s irqs/s\n", irq_items[0].buf);
		}
	}

	return lines;
}

static int
print_imc(struct engines *engines, double t, int lines, int con_w, int con_h)
{
	struct cnt_item imc_items[] = {
		{ &engines->imc_reads, 6, 0, 1.0, t, engines->imc_reads.scale,
		  "reads", "rd" },
		{ &engines->imc_writes, 6, 0, 1.0, t, engines->imc_writes.scale,
		  "writes", "wr" },
		{ NULL, 0, 0, 0.0, 0.0, 0.0, "unit" },
		{ },
	};
	struct cnt_group imc_group = {
		.name = "imc-bandwidth",
		.items = imc_items,
	};
	struct cnt_group *groups[] = {
		&imc_group,
		NULL
	};
	int ret;

	if (!engines->num_imc)
		return lines;

	ret = asprintf((char **)&imc_group.display_name, "IMC %s/s",
			engines->imc_reads.units);
	assert(ret >= 0);

	ret = asprintf((char **)&imc_items[2].unit, "%s/s",
			engines->imc_reads.units);
	assert(ret >= 0);

	print_groups(groups);

	free((void *)imc_group.display_name);
	free((void *)imc_items[2].unit);

	if (output_mode == INTERACTIVE) {
		if (lines++ < con_h)
			printf("      IMC reads:   %s %s/s\n",
			       imc_items[0].buf, engines->imc_reads.units);

		if (lines++ < con_h)
			printf("     IMC writes:   %s %s/s\n",
			       imc_items[1].buf, engines->imc_writes.units);

		if (lines++ < con_h)
			printf("\n");
	}

	return lines;
}

static bool class_view;

static int
print_engines_header(struct engines *engines, double t,
		     int lines, int con_w, int con_h)
{
	for (unsigned int i = 0;
	     i < engines->num_engines && lines < con_h;
	     i++) {
		struct engine *engine = engine_ptr(engines, i);

		if (!engine->num_counters)
			continue;

		pops->open_struct("engines");

		if (output_mode == INTERACTIVE) {
			const char *b = " MI_SEMA MI_WAIT";
			const char *a;

			if (class_view)
				a = "         ENGINES     BUSY  ";
			else
				a = "          ENGINE     BUSY  ";

			printf("\033[7m%s%*s%s\033[0m\n",
			       a, (int)(con_w - 1 - strlen(a) - strlen(b)),
			       " ", b);

			lines++;
		}

		break;
	}

	return lines;
}

static int
print_engine(struct engines *engines, unsigned int i, double t,
	     int lines, int con_w, int con_h)
{
	struct engine *engine = engine_ptr(engines, i);
	struct cnt_item engine_items[] = {
		{ &engine->busy, 6, 2, 1e9, t, 100, "busy", "%" },
		{ &engine->sema, 3, 0, 1e9, t, 100, "sema", "se" },
		{ &engine->wait, 3, 0, 1e9, t, 100, "wait", "wa" },
		{ NULL, 0, 0, 0.0, 0.0, 0.0, "unit", "%" },
		{ },
	};
	struct cnt_group engine_group = {
		.name = engine->display_name,
		.display_name = engine->short_name,
		.items = engine_items,
	};
	struct cnt_group *groups[] = {
		&engine_group,
		NULL
	};

	if (!engine->num_counters)
		return lines;

	print_groups(groups);

	if (output_mode == INTERACTIVE) {
		unsigned int max_w = con_w - 1;
		unsigned int len;
		char buf[128];
		double val;

		len = snprintf(buf, sizeof(buf), "    %s%%    %s%%",
			       engine_items[1].buf, engine_items[2].buf);

		len += printf("%16s %s%% ",
			      engine->display_name, engine_items[0].buf);

		val = pmu_calc(&engine->busy.val, 1e9, t, 100);
		print_percentage_bar(val, max_w > len ? max_w - len : 0, false);

		printf("%s\n", buf);

		lines++;
	}

	return lines;
}

static int
print_engines_footer(struct engines *engines, double t,
		     int lines, int con_w, int con_h)
{
	pops->close_struct();

	if (output_mode == INTERACTIVE) {
		if (lines++ < con_h)
			printf("\n");
	}

	return lines;
}

static int class_cmp(const void *_a, const void *_b)
{
	const struct engine_class *a = _a;
	const struct engine_class *b = _b;

	return a->class - b->class;
}

static void init_engine_classes(struct engines *engines)
{
	struct engine_class *classes;
	unsigned int i, num;
	int max = -1;

	if (engines->num_classes)
		return;

	for (i = 0; i < engines->num_engines; i++) {
		struct engine *engine = engine_ptr(engines, i);

		if ((int)engine->class > max)
			max = engine->class;
	}
	assert(max >= 0);

	num = max + 1;

	classes = calloc(num, sizeof(*classes));
	assert(classes);

	for (i = 0; i < engines->num_engines; i++) {
		struct engine *engine = engine_ptr(engines, i);

		classes[engine->class].num_engines++;
	}

	for (i = 0; i < num; i++) {
		classes[i].class = i;
		classes[i].name = class_display_name(i);
	}

	qsort(classes, num, sizeof(*classes), class_cmp);

	engines->num_classes = num;
	engines->class = classes;
}

static void __pmu_sum(struct pmu_pair *dst, struct pmu_pair *src)
{
	dst->prev += src->prev;
	dst->cur += src->cur;
}

static void __pmu_normalize(struct pmu_pair *val, unsigned int n)
{
	val->prev /= n;
	val->cur /= n;
}

static struct engines *init_class_engines(struct engines *engines)
{
	unsigned int num_present;
	struct engines *classes;
	unsigned int i, j, k;

	init_engine_classes(engines);

	num_present = 0; /* Classes with engines. */
	for (i = 0; i < engines->num_classes; i++) {
		if (engines->class[i].num_engines)
			num_present++;
	}

	classes = calloc(1, sizeof(struct engines) +
			    num_present * sizeof(struct engine));
	assert(classes);

	classes->num_engines = num_present;
	classes->num_classes = engines->num_classes;
	classes->class = engines->class;

	j = 0;
	for (i = 0; i < engines->num_classes; i++) {
		struct engine *engine = engine_ptr(classes, j);

		/* Skip classes with no engines. */
		if (!engines->class[i].num_engines)
			continue;

		assert(j < num_present);

		engine->class = i;
		engine->instance = -1;

		engine->display_name = strdup(class_display_name(i));
		assert(engine->display_name);
		engine->short_name = strdup(class_short_name(i));
		assert(engine->short_name);

		/*
		 * Copy over pmu metadata from one real engine of the same
		 * class.
		 */
		for (k = 0; k < engines->num_engines; k++) {
			struct engine *e = engine_ptr(engines, k);

			if (e->class == i) {
				engine->num_counters = e->num_counters;
				engine->busy = e->busy;
				engine->sema = e->sema;
				engine->wait = e->wait;
				break;
			}
		}

		j++; /* Next "class engine" to populate. */
	}

	return classes;
}

static struct engines *update_class_engines(struct engines *engines)
{
	static struct engines *classes;
	unsigned int i, j;

	if (!classes)
		classes = init_class_engines(engines);

	for (i = 0; i < classes->num_engines; i++) {
		struct engine *engine = engine_ptr(classes, i);
		unsigned int num_engines =
			classes->class[engine->class].num_engines;

		assert(num_engines);

		memset(&engine->busy.val, 0, sizeof(engine->busy.val));
		memset(&engine->sema.val, 0, sizeof(engine->sema.val));
		memset(&engine->wait.val, 0, sizeof(engine->wait.val));

		for (j = 0; j < engines->num_engines; j++) {
			struct engine *e = engine_ptr(engines, j);

			if (e->class == engine->class) {
				__pmu_sum(&engine->busy.val, &e->busy.val);
				__pmu_sum(&engine->sema.val, &e->sema.val);
				__pmu_sum(&engine->wait.val, &e->wait.val);
			}
		}

		__pmu_normalize(&engine->busy.val, num_engines);
		__pmu_normalize(&engine->sema.val, num_engines);
		__pmu_normalize(&engine->wait.val, num_engines);
	}

	return classes;
}

static int
print_engines(struct engines *engines, double t, int lines, int w, int h)
{
	struct engines *show;

	if (class_view)
		show = update_class_engines(engines);
	else
		show = engines;

	lines = print_engines_header(show, t, lines, w,  h);

	for (unsigned int i = 0; i < show->num_engines && lines < h; i++)
		lines = print_engine(show, i, t, lines, w, h);

	lines = print_engines_footer(show, t, lines, w, h);

	return lines;
}

static int
print_clients_header(struct clients *clients, int lines,
		     int con_w, int con_h, int *class_w)
{
	if (output_mode == INTERACTIVE) {
		const char *pidname = "   PID              NAME ";
		unsigned int num_active = 0;
		int len = strlen(pidname);

		if (lines++ >= con_h)
			return lines;

		printf("\033[7m");
		printf("%s", pidname);

		if (lines++ >= con_h || len >= con_w)
			return lines;

		if (clients->num_classes) {
			unsigned int i;
			int width;

			for (i = 0; i < clients->num_classes; i++) {
				if (clients->class[i].num_engines)
					num_active++;
			}

			*class_w = width = (con_w - len) / num_active;

			for (i = 0; i < clients->num_classes; i++) {
				const char *name = clients->class[i].name;
				int name_len = strlen(name);
				int pad = (width - name_len) / 2;
				int spaces = width - pad - name_len;

				if (!clients->class[i].num_engines)
					continue; /* Assert in the ideal world. */

				if (pad < 0 || spaces < 0)
					continue;

				n_spaces(pad);
				printf("%s", name);
				n_spaces(spaces);
				len += pad + name_len + spaces;
			}
		}

		n_spaces(con_w - len);
		printf("\033[0m\n");
	} else {
		if (clients->num_classes)
			pops->open_struct("clients");
	}

	return lines;
}

static bool numeric_clients;
static bool filter_idle;

static int
print_client(struct client *c, struct engines *engines, double t, int lines,
	     int con_w, int con_h, unsigned int period_us, int *class_w)
{
	struct clients *clients = c->clients;
	unsigned int i;

	if (output_mode == INTERACTIVE) {
		if (filter_idle && !c->total_runtime)
			return lines;

		lines++;

		printf("%6u %17s ", c->pid, c->print_name);

		for (i = 0; c->samples > 1 && i < clients->num_classes; i++) {
			double pct;

			if (!clients->class[i].num_engines)
				continue; /* Assert in the ideal world. */

			pct = (double)c->val[i] / period_us / 1e3 * 100 /
			      clients->class[i].num_engines;

			/*
			 * Guard against possible time-drift between sampling
			 * client data and time we obtained our time-delta from
			 * PMU.
			 */
			if (pct > 100.0)
				pct = 100.0;

			print_percentage_bar(pct, *class_w, numeric_clients);
		}

		putchar('\n');
	} else if (output_mode == JSON) {
		char buf[64];

		snprintf(buf, sizeof(buf), "%u", c->id);
		pops->open_struct(buf);

		__json_add_member("name", c->print_name);

		snprintf(buf, sizeof(buf), "%u", c->pid);
		__json_add_member("pid", buf);

		if (c->samples > 1) {
			pops->open_struct("engine-classes");

			for (i = 0; i < clients->num_classes; i++) {
				double pct;

				snprintf(buf, sizeof(buf), "%s",
					clients->class[i].name);
				pops->open_struct(buf);

				pct = (double)c->val[i] / period_us / 1e3 * 100;
				snprintf(buf, sizeof(buf), "%f", pct);
				__json_add_member("busy", buf);

				__json_add_member("unit", "%");

				pops->close_struct();
			}

			pops->close_struct();
		}

		pops->close_struct();
	}

	return lines;
}

static int
print_clients_footer(struct clients *clients, double t,
		     int lines, int con_w, int con_h)
{
	if (output_mode == INTERACTIVE) {
		if (lines++ < con_h)
			printf("\n");
	} else {
		if (clients->num_classes)
			pops->close_struct();
	}

	return lines;
}

static bool stop_top;

static void sigint_handler(int  sig)
{
	stop_top = true;
}

/* tr_pmu_name()
 *
 * Transliterate pci_slot_id to sysfs device name entry for discrete GPU.
 * Discrete GPU PCI ID   ("xxxx:yy:zz.z")       device = "i915_xxxx_yy_zz.z".
 */
static char *tr_pmu_name(struct igt_device_card *card)
{
	int ret;
	const int bufsize = 18;
	char *buf, *device = NULL;

	assert(card->pci_slot_name[0]);

	device = malloc(bufsize);
	assert(device);

	ret = snprintf(device, bufsize, "i915_%s", card->pci_slot_name);
	assert(ret == (bufsize-1));

	buf = device;
	for (; *buf; buf++)
		if (*buf == ':')
			*buf = '_';

	return device;
}

static void interactive_stdin(void)
{
	struct termios termios = { };
	int ret;

	ret = fcntl(0, F_GETFL, NULL);
	ret |= O_NONBLOCK;
	ret = fcntl(0, F_SETFL, ret);
	assert(ret == 0);

	ret = tcgetattr(0, &termios);
	assert(ret == 0);

	termios.c_lflag &= ~ICANON;
	termios.c_cc[VMIN] = 1;
	termios.c_cc[VTIME] = 0; /* Deciseconds only - we'll use poll. */

	ret = tcsetattr(0, TCSAFLUSH, &termios);
	assert(ret == 0);
}

static void select_client_sort(void)
{
	static unsigned int client_sort;

	switch (++client_sort % 3) {
	case 0:
		client_cmp = client_last_cmp;
		break;
	case 1:
		client_cmp = client_total_cmp;
		break;
	case 2:
		client_cmp = client_id_cmp;
	}
}

static void process_stdin(unsigned int timeout_us)
{
	struct pollfd p = { .fd = 0, .events = POLLIN };
	int ret;

	ret = poll(&p, 1, timeout_us / 1000);
	if (ret <= 0) {
		if (ret < 0)
			stop_top = true;
		return;
	}

	for (;;) {
		char c;

		ret = read(0, &c, 1);
		if (ret <= 0)
			break;

		switch (c) {
		case 'q':
			stop_top = true;
			break;
		case '1':
			class_view ^= true;
			break;
		case 'i':
			filter_idle ^= true;
			break;
		case 'n':
			numeric_clients ^= true;
			break;
		case 's':
			select_client_sort();
			break;
		};
	}
}

int main(int argc, char **argv)
{
	unsigned int period_us = DEFAULT_PERIOD_MS * 1000;
	struct clients *clients = NULL;
	int con_w = -1, con_h = -1;
	char *output_path = NULL;
	struct engines *engines;
	int ret = 0, ch;
	bool list_device = false;
	char *pmu_device, *opt_device = NULL;
	struct igt_device_card card;
	char *codename = NULL;

	/* Parse options */
	while ((ch = getopt(argc, argv, "o:s:d:JLlh")) != -1) {
		switch (ch) {
		case 'o':
			output_path = optarg;
			break;
		case 's':
			period_us = atoi(optarg) * 1000;
			break;
		case 'd':
			opt_device = strdup(optarg);
			break;
		case 'J':
			output_mode = JSON;
			break;
		case 'L':
			list_device = true;
			break;
		case 'l':
			output_mode = STDOUT;
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

	if (output_mode == INTERACTIVE && (output_path || isatty(1) != 1))
		output_mode = STDOUT;

	if (output_path && strcmp(output_path, "-")) {
		out = fopen(output_path, "w");

		if (!out) {
			fprintf(stderr, "Failed to open output file - '%s'!\n",
				strerror(errno));
			exit(1);
		}
	} else {
		out = stdout;
	}

	if (output_mode != INTERACTIVE) {
		sighandler_t sig = signal(SIGINT, sigint_handler);

		if (sig == SIG_ERR)
			fprintf(stderr, "Failed to install signal handler!\n");
	}

	switch (output_mode) {
	case INTERACTIVE:
		pops = &term_pops;
		interactive_stdin();
		class_view = true;
		break;
	case STDOUT:
		pops = &stdout_pops;
		break;
	case JSON:
		pops = &json_pops;
		break;
	default:
		assert(0);
		break;
	};

	igt_devices_scan(false);

	if (list_device) {
		struct igt_devices_print_format fmt = {
			.type = IGT_PRINT_USER,
			.option = IGT_PRINT_PCI,
		};

		igt_devices_print(&fmt);
		goto exit;
	}

	if (opt_device != NULL) {
		ret = igt_device_card_match_pci(opt_device, &card);
		if (!ret)
			fprintf(stderr, "Requested device %s not found!\n", opt_device);
		free(opt_device);
	} else {
		ret = igt_device_find_first_i915_discrete_card(&card);
		if (!ret)
			ret = igt_device_find_integrated_card(&card);
		if (!ret)
			fprintf(stderr, "No device filter specified and no discrete/integrated i915 devices found\n");
	}

	if (!ret) {
		ret = EXIT_FAILURE;
		goto exit;
	}

	if (card.pci_slot_name[0] && !is_igpu_pci(card.pci_slot_name))
		pmu_device = tr_pmu_name(&card);
	else
		pmu_device = strdup("i915");

	engines = discover_engines(pmu_device);
	if (!engines) {
		fprintf(stderr,
			"Failed to detect engines! (%s)\n(Kernel 4.16 or newer is required for i915 PMU support.)\n",
			strerror(errno));
		ret = EXIT_FAILURE;
		goto err;
	}

	ret = pmu_init(engines);
	if (ret) {
		fprintf(stderr,
			"Failed to initialize PMU! (%s)\n", strerror(errno));
		ret = EXIT_FAILURE;
		goto err;
	}

	ret = EXIT_SUCCESS;

	clients = init_clients(card.pci_slot_name[0] ? card.card : NULL);
	init_engine_classes(engines);
	clients->num_classes = engines->num_classes;
	clients->class = engines->class;

	pmu_sample(engines);
	scan_clients(clients);
	codename = igt_device_get_pretty_name(&card, false);

	while (!stop_top) {
		bool consumed = false;
		int j, lines = 0;
		struct winsize ws;
		struct client *c;
		double t;

		/* Update terminal size. */
		if (output_mode != INTERACTIVE) {
			con_w = con_h = INT_MAX;
		} else if (ioctl(0, TIOCGWINSZ, &ws) != -1) {
			con_w = ws.ws_col;
			con_h = ws.ws_row;
			if (con_w == 0 && con_h == 0) {
				/* Serial console. */
				con_w = 80;
				con_h = 24;
			}
		}

		pmu_sample(engines);
		t = (double)(engines->ts.cur - engines->ts.prev) / 1e9;

		scan_clients(clients);

		if (stop_top)
			break;

		while (!consumed) {
			pops->open_struct(NULL);

			lines = print_header(&card, codename, engines,
					     t, lines, con_w, con_h,
					     &consumed);

			lines = print_imc(engines, t, lines, con_w, con_h);

			lines = print_engines(engines, t, lines, con_w, con_h);

			if (clients) {
				int class_w;

				lines = print_clients_header(clients, lines,
							     con_w, con_h,
							     &class_w);

				for_each_client(clients, c, j) {
					assert(c->status != PROBE);
					if (c->status != ALIVE)
						break; /* Active clients are first in the array. */

					if (lines >= con_h)
						break;

					lines = print_client(c, engines, t,
							     lines, con_w,
							     con_h, period_us,
							     &class_w);
				}

				lines = print_clients_footer(clients, t, lines,
							     con_w, con_h);
			}

			pops->close_struct();
		}

		if (stop_top)
			break;

		if (output_mode == INTERACTIVE)
			process_stdin(period_us);
		else
			usleep(period_us);
	}

	free(codename);
err:
	free(engines);
	free(pmu_device);
exit:
	igt_devices_free();
	return ret;
}
