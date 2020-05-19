/*
 * Copyright © 2017 Intel Corporation
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

#include <unistd.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <inttypes.h>
#include <errno.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <assert.h>
#include <limits.h>
#include <pthread.h>

#include "intel_chipset.h"
#include "intel_reg.h"
#include "drm.h"
#include "ioctl_wrappers.h"
#include "drmtest.h"

#include "intel_io.h"
#include "igt_aux.h"
#include "igt_rand.h"
#include "igt_perf.h"
#include "sw_sync.h"
#include "i915/gem_mman.h"

#include "i915/gem_engine_topology.h"

enum intel_engine_id {
	DEFAULT,
	RCS,
	BCS,
	VCS,
	VCS1,
	VCS2,
	VECS,
	NUM_ENGINES
};

struct duration {
	unsigned int min, max;
};

enum w_type
{
	BATCH,
	SYNC,
	DELAY,
	PERIOD,
	THROTTLE,
	QD_THROTTLE,
	SW_FENCE,
	SW_FENCE_SIGNAL,
	CTX_PRIORITY,
	PREEMPTION,
	ENGINE_MAP,
	LOAD_BALANCE,
	BOND,
	TERMINATE,
	SSEU,
	WORKINGSET,
};

struct dep_entry {
	int target;
	bool write;
	int working_set; /* -1 = step dependecy, >= 0 working set id */
};

struct deps
{
	int nr;
	bool submit_fence;
	struct dep_entry *list;
};

struct w_arg {
	char *filename;
	char *desc;
	int prio;
	bool sseu;
};

struct bond {
	uint64_t mask;
	enum intel_engine_id master;
};

struct work_buffer_size {
	unsigned long size;
	unsigned long min;
	unsigned long max;
};

struct working_set {
	int id;
	bool shared;
	unsigned int nr;
	uint32_t *handles;
	struct work_buffer_size *sizes;
};

struct workload;

struct w_step
{
	struct workload *wrk;

	/* Workload step metadata */
	enum w_type type;
	unsigned int context;
	unsigned int engine;
	struct duration duration;
	bool unbound_duration;
	struct deps data_deps;
	struct deps fence_deps;
	int emit_fence;
	union {
		int sync;
		int delay;
		int period;
		int target;
		int throttle;
		int fence_signal;
		int priority;
		struct {
			unsigned int engine_map_count;
			enum intel_engine_id *engine_map;
		};
		bool load_balance;
		struct {
			uint64_t bond_mask;
			enum intel_engine_id bond_master;
		};
		int sseu;
		struct working_set working_set;
	};

	/* Implementation details */
	unsigned int idx;
	struct igt_list_head rq_link;
	unsigned int request;
	unsigned int preempt_us;

	struct drm_i915_gem_execbuffer2 eb;
	struct drm_i915_gem_exec_object2 *obj;
	struct drm_i915_gem_relocation_entry reloc[1];
	unsigned long bb_sz;
	uint32_t bb_handle;
	uint32_t *recursive_bb_start;
};

struct ctx {
	uint32_t id;
	int priority;
	unsigned int engine_map_count;
	enum intel_engine_id *engine_map;
	unsigned int bond_count;
	struct bond *bonds;
	bool load_balance;
	uint64_t sseu;
};

struct workload
{
	unsigned int id;

	unsigned int nr_steps;
	struct w_step *steps;
	int prio;
	bool sseu;

	pthread_t thread;
	bool run;
	bool background;
	unsigned int repeat;
	unsigned int flags;
	bool print_stats;

	uint32_t bb_prng;
	uint32_t bo_prng;

	struct timespec repeat_start;

	unsigned int nr_ctxs;
	struct ctx *ctx_list;

	struct working_set **working_sets; /* array indexed by set id */
	int max_working_set_id;

	int sync_timeline;
	uint32_t sync_seqno;

	struct igt_list_head requests[NUM_ENGINES];
	unsigned int nrequest[NUM_ENGINES];
};

static const unsigned int nop_calibration_us = 1000;
static bool has_nop_calibration = false;
static bool sequential = true;

static unsigned int master_prng;

static int verbose = 1;
static int fd;
static struct drm_i915_gem_context_param_sseu device_sseu = {
	.slice_mask = -1 /* Force read on first use. */
};

#define SYNCEDCLIENTS	(1<<1)
#define DEPSYNC		(1<<2)
#define SSEU		(1<<3)

static const char *ring_str_map[NUM_ENGINES] = {
	[DEFAULT] = "DEFAULT",
	[RCS] = "RCS",
	[BCS] = "BCS",
	[VCS] = "VCS",
	[VCS1] = "VCS1",
	[VCS2] = "VCS2",
	[VECS] = "VECS",
};

/* stores calibrations for particular engines */
static unsigned long engine_calib_map[NUM_ENGINES];

static enum intel_engine_id
ci_to_engine_id(int class, int instance)
{
	static const struct {
		int class;
		int instance;
		unsigned int id;
	} map[] = {
		{ I915_ENGINE_CLASS_RENDER, 0, RCS },
		{ I915_ENGINE_CLASS_COPY, 0, BCS },
		{ I915_ENGINE_CLASS_VIDEO, 0, VCS1 },
		{ I915_ENGINE_CLASS_VIDEO, 1, VCS2 },
		{ I915_ENGINE_CLASS_VIDEO, 2, VCS2 }, /* FIXME/ICL */
		{ I915_ENGINE_CLASS_VIDEO_ENHANCE, 0, VECS },
	};

	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(map); i++) {
		if (class == map[i].class && instance == map[i].instance)
			return map[i].id;
	}
	return -1;
}

static void
apply_unset_calibrations(unsigned long raw_number)
{
	for (int i = 0; i < NUM_ENGINES; i++)
		engine_calib_map[i] += engine_calib_map[i] ? 0 : raw_number;
}

static void
print_engine_calibrations(void)
{
	bool first_entry = true;

	printf("Nop calibration for %uus delay is: ", nop_calibration_us);
	for (int i = 0; i < NUM_ENGINES; i++) {
		/* skip DEFAULT and VCS engines */
		if (i != DEFAULT && i != VCS) {
			if (first_entry) {
				printf("%s=%lu", ring_str_map[i], engine_calib_map[i]);
				first_entry = false;
			} else {
				printf(",%s=%lu", ring_str_map[i], engine_calib_map[i]);
			}
		}
	}
	printf("\n");
}

static void add_dep(struct deps *deps, struct dep_entry entry)
{
	deps->list = realloc(deps->list, sizeof(*deps->list) * (deps->nr + 1));
	igt_assert(deps->list);

	deps->list[deps->nr++] = entry;
}

static int
parse_working_set_deps(struct workload *wrk,
		       struct deps *deps,
		       struct dep_entry _entry,
		       char *str)
{
	/*
	 * 1 - target handle index in the specified working set.
	 * 2-4 - range
	 */
	struct dep_entry entry = _entry;
	char *s;

	s = index(str, '-');
	if (s) {
		int from, to;

		from = atoi(str);
		if (from < 0)
			return -1;

		to = atoi(++s);
		if (to <= 0)
			return -1;

		if (to <= from)
			return -1;

		for (entry.target = from; entry.target <= to; entry.target++)
			add_dep(deps, entry);
	} else {
		entry.target = atoi(str);
		if (entry.target < 0)
			return -1;

		add_dep(deps, entry);
	}

	return 0;
}

static int
parse_dependency(unsigned int nr_steps, struct w_step *w, char *str)
{
	struct dep_entry entry = { .working_set = -1 };
	bool submit_fence = false;
	char *s;

	switch (str[0]) {
	case '-':
		if (str[1] < '0' || str[1] > '9')
			return -1;

		entry.target = atoi(str);
		if (entry.target > 0 || ((int)nr_steps + entry.target) < 0)
			return -1;

		add_dep(&w->data_deps, entry);

		break;
	case 's':
		submit_fence = true;
		/* Fall-through. */
	case 'f':
		/* Multiple fences not yet supported. */
		igt_assert_eq(w->fence_deps.nr, 0);

		entry.target = atoi(++str);
		if (entry.target > 0 || ((int)nr_steps + entry.target) < 0)
			return -1;

		add_dep(&w->fence_deps, entry);

		w->fence_deps.submit_fence = submit_fence;
		break;
	case 'w':
		entry.write = true;
		/* Fall-through. */
	case 'r':
		/*
		 * [rw]N-<str>
		 * r1-<str> or w2-<str>, where N is working set id.
		 */
		s = index(++str, '-');
		if (!s)
			return -1;

		entry.working_set = atoi(str);
		if (entry.working_set < 0)
			return -1;

		if (parse_working_set_deps(w->wrk, &w->data_deps, entry, ++s))
			return -1;

		break;
	default:
		return -1;
	};

	return 0;
}

static int
parse_dependencies(unsigned int nr_steps, struct w_step *w, char *_desc)
{
	char *desc = strdup(_desc);
	char *token, *tctx = NULL, *tstart = desc;
	int ret = 0;

	/*
	 * Skip when no dependencies to avoid having to detect
	 * non-sensical "0/0/..." below.
	 */
	if (!strcmp(_desc, "0"))
		goto out;

	igt_assert(desc);
	igt_assert(!w->data_deps.nr && w->data_deps.nr == w->fence_deps.nr);
	igt_assert(!w->data_deps.list &&
		   w->data_deps.list == w->fence_deps.list);

	while ((token = strtok_r(tstart, "/", &tctx)) != NULL) {
		tstart = NULL;

		ret = parse_dependency(nr_steps, w, token);
		if (ret)
			break;
	}

out:
	free(desc);

	return ret;
}

static void __attribute__((format(printf, 1, 2)))
wsim_err(const char *fmt, ...)
{
	va_list ap;

	if (!verbose)
		return;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

#define check_arg(cond, fmt, ...) \
{ \
	if (cond) { \
		wsim_err(fmt, __VA_ARGS__); \
		return NULL; \
	} \
}

static int str_to_engine(const char *str)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(ring_str_map); i++) {
		if (!strcasecmp(str, ring_str_map[i]))
			return i;
	}

	return -1;
}

static bool __engines_queried;
static unsigned int __num_engines;
static struct i915_engine_class_instance *__engines;

static int
__i915_query(int i915, struct drm_i915_query *q)
{
	if (igt_ioctl(i915, DRM_IOCTL_I915_QUERY, q))
		return -errno;
	return 0;
}

static int
__i915_query_items(int i915, struct drm_i915_query_item *items, uint32_t n_items)
{
	struct drm_i915_query q = {
		.num_items = n_items,
		.items_ptr = to_user_pointer(items),
	};
	return __i915_query(i915, &q);
}

static void
i915_query_items(int i915, struct drm_i915_query_item *items, uint32_t n_items)
{
	igt_assert_eq(__i915_query_items(i915, items, n_items), 0);
}

static bool has_engine_query(int i915)
{
	struct drm_i915_query_item item = {
		.query_id = DRM_I915_QUERY_ENGINE_INFO,
	};

	return __i915_query_items(i915, &item, 1) == 0 && item.length > 0;
}

static void query_engines(void)
{
	struct i915_engine_class_instance *engines;
	unsigned int num;

	if (__engines_queried)
		return;

	__engines_queried = true;

	if (!has_engine_query(fd)) {
		unsigned int num_bsd = gem_has_bsd(fd) + gem_has_bsd2(fd);
		unsigned int i = 0;

		igt_assert(num_bsd);

		num = 1 + num_bsd;

		if (gem_has_blt(fd))
			num++;

		if (gem_has_vebox(fd))
			num++;

		engines = calloc(num,
				 sizeof(struct i915_engine_class_instance));
		igt_assert(engines);

		engines[i].engine_class = I915_ENGINE_CLASS_RENDER;
		engines[i].engine_instance = 0;
		i++;

		if (gem_has_blt(fd)) {
			engines[i].engine_class = I915_ENGINE_CLASS_COPY;
			engines[i].engine_instance = 0;
			i++;
		}

		if (gem_has_bsd(fd)) {
			engines[i].engine_class = I915_ENGINE_CLASS_VIDEO;
			engines[i].engine_instance = 0;
			i++;
		}

		if (gem_has_bsd2(fd)) {
			engines[i].engine_class = I915_ENGINE_CLASS_VIDEO;
			engines[i].engine_instance = 1;
			i++;
		}

		if (gem_has_vebox(fd)) {
			engines[i].engine_class =
				I915_ENGINE_CLASS_VIDEO_ENHANCE;
			engines[i].engine_instance = 0;
			i++;
		}
	} else {
		struct drm_i915_query_engine_info *engine_info;
		struct drm_i915_query_item item = {
			.query_id = DRM_I915_QUERY_ENGINE_INFO,
		};
		const unsigned int sz = 4096;
		unsigned int i;

		engine_info = malloc(sz);
		igt_assert(engine_info);
		memset(engine_info, 0, sz);

		item.data_ptr = to_user_pointer(engine_info);
		item.length = sz;

		i915_query_items(fd, &item, 1);
		igt_assert(item.length > 0);
		igt_assert(item.length <= sz);

		num = engine_info->num_engines;

		engines = calloc(num,
				 sizeof(struct i915_engine_class_instance));
		igt_assert(engines);

		for (i = 0; i < num; i++) {
			struct drm_i915_engine_info *engine =
				(struct drm_i915_engine_info *)&engine_info->engines[i];

			engines[i] = engine->engine;
		}
	}

	__engines = engines;
	__num_engines = num;
}

static unsigned int num_engines_in_class(enum intel_engine_id class)
{
	unsigned int i, count = 0;

	igt_assert(class == VCS);

	query_engines();

	for (i = 0; i < __num_engines; i++) {
		if (__engines[i].engine_class == I915_ENGINE_CLASS_VIDEO)
			count++;
	}

	igt_assert(count);
	return count;
}

static void
fill_engines_id_class(enum intel_engine_id *list,
		      enum intel_engine_id class)
{
	enum intel_engine_id engine = VCS1;
	unsigned int i, j = 0;

	igt_assert(class == VCS);
	igt_assert(num_engines_in_class(VCS) <= 2);

	query_engines();

	for (i = 0; i < __num_engines; i++) {
		if (__engines[i].engine_class != I915_ENGINE_CLASS_VIDEO)
			continue;

		list[j++] = engine++;
	}
}

static unsigned int
find_physical_instance(enum intel_engine_id class, unsigned int logical)
{
	unsigned int i, j = 0;

	igt_assert(class == VCS);

	for (i = 0; i < __num_engines; i++) {
		if (__engines[i].engine_class != I915_ENGINE_CLASS_VIDEO)
			continue;

		/* Map logical to physical instances. */
		if (logical == j++)
			return __engines[i].engine_instance;
	}

	igt_assert(0);
	return 0;
}

static struct i915_engine_class_instance
get_engine(enum intel_engine_id engine)
{
	struct i915_engine_class_instance ci;

	query_engines();

	switch (engine) {
	case RCS:
		ci.engine_class = I915_ENGINE_CLASS_RENDER;
		ci.engine_instance = 0;
		break;
	case BCS:
		ci.engine_class = I915_ENGINE_CLASS_COPY;
		ci.engine_instance = 0;
		break;
	case VCS1:
	case VCS2:
		ci.engine_class = I915_ENGINE_CLASS_VIDEO;
		ci.engine_instance = find_physical_instance(VCS, engine - VCS1);
		break;
	case VECS:
		ci.engine_class = I915_ENGINE_CLASS_VIDEO_ENHANCE;
		ci.engine_instance = 0;
		break;
	default:
		igt_assert(0);
	};

	return ci;
}

static int parse_engine_map(struct w_step *step, const char *_str)
{
	char *token, *tctx = NULL, *tstart = (char *)_str;

	while ((token = strtok_r(tstart, "|", &tctx))) {
		enum intel_engine_id engine;
		unsigned int add;

		tstart = NULL;

		if (!strcmp(token, "DEFAULT"))
			return -1;

		engine = str_to_engine(token);
		if ((int)engine < 0)
			return -1;

		if (engine != VCS && engine != VCS1 && engine != VCS2 &&
		    engine != RCS)
			return -1; /* TODO */

		add = engine == VCS ? num_engines_in_class(VCS) : 1;
		step->engine_map_count += add;
		step->engine_map = realloc(step->engine_map,
					   step->engine_map_count *
					   sizeof(step->engine_map[0]));

		if (engine != VCS)
			step->engine_map[step->engine_map_count - add] = engine;
		else
			fill_engines_id_class(&step->engine_map[step->engine_map_count - add], VCS);
	}

	return 0;
}

static unsigned long parse_size(char *str)
{
	const unsigned int len = strlen(str);
	unsigned int mult = 1;
	long val;

	/* "1234567890[gGmMkK]" */

	if (len == 0)
		return 0;

	switch (str[len - 1]) {
	case 'g':
	case 'G':
		mult *= 1024;
		/* Fall-throuogh. */
	case 'm':
	case 'M':
		mult *= 1024;
		/* Fall-throuogh. */
	case 'k':
	case 'K':
		mult *= 1024;

		str[len - 1] = 0;
		/* Fall-throuogh. */

	case '0' ... '9':
		break;
	default:
		return 0; /* Unrecognized non-digit. */
	}

	val = atol(str);
	if (val <= 0)
		return 0;

	return val * mult;
}

static int add_buffers(struct working_set *set, char *str)
{
	/*
	 * 4096
	 * 4k
	 * 4m
	 * 4g
	 * 10n4k - 10 4k batches
	 * 4096-16k - random size in range
	 */
	struct work_buffer_size *sizes;
	unsigned long min_sz, max_sz;
	char *n, *max = NULL;
	int add, i;

	n = index(str, 'n');
	if (n) {
		*n = 0;
		add = atoi(str);
		if (add <= 0)
			return -1;
		str = ++n;
	} else {
		add = 1;
	}

	n = index(str, '-');
	if (n) {
		*n = 0;
		max = ++n;
	}

	min_sz = parse_size(str);
	if (!min_sz)
		return -1;

	if (max) {
		max_sz = parse_size(max);
		if (!max_sz)
			return -1;
	} else {
		max_sz = min_sz;
	}

	sizes = realloc(set->sizes, (set->nr + add) * sizeof(*sizes));
	if (!sizes)
		return -1;

	for (i = 0; i < add; i++) {
		struct work_buffer_size *sz = &sizes[set->nr + i];
		sz->min = min_sz;
		sz->max = max_sz;
		sz->size = 0;
	}

	set->nr += add;
	set->sizes = sizes;

	return 0;
}

static int parse_working_set(struct working_set *set, char *str)
{
	char *token, *tctx = NULL, *tstart = str;

	while ((token = strtok_r(tstart, "/", &tctx))) {
		tstart = NULL;

		if (add_buffers(set, token))
			return -1;
	}

	return 0;
}

static uint64_t engine_list_mask(const char *_str)
{
	uint64_t mask = 0;

	char *token, *tctx = NULL, *tstart = (char *)_str;

	while ((token = strtok_r(tstart, "|", &tctx))) {
		enum intel_engine_id engine = str_to_engine(token);

		if ((int)engine < 0 || engine == DEFAULT || engine == VCS)
			return 0;

		mask |= 1 << engine;

		tstart = NULL;
	}

	return mask;
}

static void allocate_working_set(struct workload *wrk, struct working_set *set);

#define int_field(_STEP_, _FIELD_, _COND_, _ERR_) \
	if ((field = strtok_r(fstart, ".", &fctx))) { \
		tmp = atoi(field); \
		check_arg(_COND_, _ERR_, nr_steps); \
		step.type = _STEP_; \
		step._FIELD_ = tmp; \
		goto add_step; \
	} \

static struct workload *
parse_workload(struct w_arg *arg, unsigned int flags, struct workload *app_w)
{
	struct workload *wrk;
	unsigned int nr_steps = 0;
	char *desc = strdup(arg->desc);
	char *_token, *token, *tctx = NULL, *tstart = desc;
	char *field, *fctx = NULL, *fstart;
	struct w_step step, *w, *steps = NULL;
	unsigned int valid;
	int i, j, tmp;

	igt_assert(desc);

	while ((_token = strtok_r(tstart, ",", &tctx))) {
		tstart = NULL;
		token = strdup(_token);
		igt_assert(token);
		fstart = token;
		valid = 0;
		memset(&step, 0, sizeof(step));

		if ((field = strtok_r(fstart, ".", &fctx))) {
			fstart = NULL;

			if (!strcmp(field, "d")) {
				int_field(DELAY, delay, tmp <= 0,
					  "Invalid delay at step %u!\n");
			} else if (!strcmp(field, "p")) {
				int_field(PERIOD, period, tmp <= 0,
					  "Invalid period at step %u!\n");
			} else if (!strcmp(field, "P")) {
				unsigned int nr = 0;
				while ((field = strtok_r(fstart, ".", &fctx))) {
					tmp = atoi(field);
					check_arg(nr == 0 && tmp <= 0,
						  "Invalid context at step %u!\n",
						  nr_steps);
					check_arg(nr > 1,
						  "Invalid priority format at step %u!\n",
						  nr_steps);

					if (nr == 0)
						step.context = tmp;
					else
						step.priority = tmp;

					nr++;
				}

				step.type = CTX_PRIORITY;
				goto add_step;
			} else if (!strcmp(field, "s")) {
				int_field(SYNC, target,
					  tmp >= 0 || ((int)nr_steps + tmp) < 0,
					  "Invalid sync target at step %u!\n");
			} else if (!strcmp(field, "S")) {
				unsigned int nr = 0;
				while ((field = strtok_r(fstart, ".", &fctx))) {
					tmp = atoi(field);
					check_arg(tmp <= 0 && nr == 0,
						  "Invalid context at step %u!\n",
						  nr_steps);
					check_arg(nr > 1,
						  "Invalid SSEU format at step %u!\n",
						  nr_steps);

					if (nr == 0)
						step.context = tmp;
					else if (nr == 1)
						step.sseu = tmp;

					nr++;
				}

				step.type = SSEU;
				goto add_step;
			} else if (!strcmp(field, "t")) {
				int_field(THROTTLE, throttle,
					  tmp < 0,
					  "Invalid throttle at step %u!\n");
			} else if (!strcmp(field, "q")) {
				int_field(QD_THROTTLE, throttle,
					  tmp < 0,
					  "Invalid qd throttle at step %u!\n");
			} else if (!strcmp(field, "a")) {
				int_field(SW_FENCE_SIGNAL, target,
					  tmp >= 0,
					  "Invalid sw fence signal at step %u!\n");
			} else if (!strcmp(field, "f")) {
				step.type = SW_FENCE;
				goto add_step;
			} else if (!strcmp(field, "M")) {
				unsigned int nr = 0;
				while ((field = strtok_r(fstart, ".", &fctx))) {
					tmp = atoi(field);
					check_arg(nr == 0 && tmp <= 0,
						  "Invalid context at step %u!\n",
						  nr_steps);
					check_arg(nr > 1,
						  "Invalid engine map format at step %u!\n",
						  nr_steps);

					if (nr == 0) {
						step.context = tmp;
					} else {
						tmp = parse_engine_map(&step,
								       field);
						check_arg(tmp < 0,
							  "Invalid engine map list at step %u!\n",
							  nr_steps);
					}

					nr++;
				}

				step.type = ENGINE_MAP;
				goto add_step;
			} else if (!strcmp(field, "T")) {
				int_field(TERMINATE, target,
					  tmp >= 0 || ((int)nr_steps + tmp) < 0,
					  "Invalid terminate target at step %u!\n");
			} else if (!strcmp(field, "X")) {
				unsigned int nr = 0;
				while ((field = strtok_r(fstart, ".", &fctx))) {
					tmp = atoi(field);
					check_arg(nr == 0 && tmp <= 0,
						  "Invalid context at step %u!\n",
						  nr_steps);
					check_arg(nr == 1 && tmp < 0,
						  "Invalid preemption period at step %u!\n",
						  nr_steps);
					check_arg(nr > 1,
						  "Invalid preemption format at step %u!\n",
						  nr_steps);

					if (nr == 0)
						step.context = tmp;
					else
						step.period = tmp;

					nr++;
				}

				step.type = PREEMPTION;
				goto add_step;
			} else if (!strcmp(field, "B")) {
				unsigned int nr = 0;
				while ((field = strtok_r(fstart, ".", &fctx))) {
					tmp = atoi(field);
					check_arg(nr == 0 && tmp <= 0,
						  "Invalid context at step %u!\n",
						  nr_steps);
					check_arg(nr > 0,
						  "Invalid load balance format at step %u!\n",
						  nr_steps);

					step.context = tmp;
					step.load_balance = true;

					nr++;
				}

				step.type = LOAD_BALANCE;
				goto add_step;
			} else if (!strcmp(field, "b")) {
				unsigned int nr = 0;
				while ((field = strtok_r(fstart, ".", &fctx))) {
					check_arg(nr > 2,
						  "Invalid bond format at step %u!\n",
						  nr_steps);

					if (nr == 0) {
						tmp = atoi(field);
						step.context = tmp;
						check_arg(tmp <= 0,
							  "Invalid context at step %u!\n",
							  nr_steps);
					} else if (nr == 1) {
						step.bond_mask = engine_list_mask(field);
						check_arg(step.bond_mask == 0,
							"Invalid siblings list at step %u!\n",
							nr_steps);
					} else if (nr == 2) {
						tmp = str_to_engine(field);
						check_arg(tmp <= 0 ||
							  tmp == VCS ||
							  tmp == DEFAULT,
							  "Invalid master engine at step %u!\n",
							  nr_steps);
						step.bond_master = tmp;
					}

					nr++;
				}

				step.type = BOND;
				goto add_step;
			} else if (!strcmp(field, "w") || !strcmp(field, "W")) {
				unsigned int nr = 0;

				step.working_set.shared = field[0] == 'W';

				while ((field = strtok_r(fstart, ".", &fctx))) {
					tmp = atoi(field);
					if (nr == 0) {
						step.working_set.id = tmp;
					} else {
						tmp = parse_working_set(&step.working_set,
									field);
						check_arg(tmp < 0,
							  "Invalid working set at step %u!\n",
							  nr_steps);
					}

					nr++;
				}

				step.type = WORKINGSET;
				goto add_step;
			}

			if (!field) {
				if (verbose)
					fprintf(stderr,
						"Parse error at step %u!\n",
						nr_steps);
				return NULL;
			}

			tmp = atoi(field);
			check_arg(tmp < 0, "Invalid ctx id at step %u!\n",
				  nr_steps);
			step.context = tmp;

			valid++;
		}

		if ((field = strtok_r(fstart, ".", &fctx))) {
			fstart = NULL;

			i = str_to_engine(field);
			check_arg(i < 0,
				  "Invalid engine id at step %u!\n", nr_steps);

			valid++;

			step.engine = i;
		}

		if ((field = strtok_r(fstart, ".", &fctx))) {
			char *sep = NULL;
			long int tmpl;

			fstart = NULL;

			if (field[0] == '*') {
				check_arg(intel_gen(intel_get_drm_devid(fd)) < 8,
					  "Infinite batch at step %u needs Gen8+!\n",
					  nr_steps);
				step.unbound_duration = true;
			} else {
				tmpl = strtol(field, &sep, 10);
				check_arg(tmpl <= 0 || tmpl == LONG_MIN ||
					  tmpl == LONG_MAX,
					  "Invalid duration at step %u!\n",
					  nr_steps);
				step.duration.min = tmpl;

				if (sep && *sep == '-') {
					tmpl = strtol(sep + 1, NULL, 10);
					check_arg(tmpl <= 0 ||
						tmpl <= step.duration.min ||
						tmpl == LONG_MIN ||
						tmpl == LONG_MAX,
						"Invalid duration range at step %u!\n",
						nr_steps);
					step.duration.max = tmpl;
				} else {
					step.duration.max = step.duration.min;
				}
			}

			valid++;
		}

		if ((field = strtok_r(fstart, ".", &fctx))) {
			fstart = NULL;

			tmp = parse_dependencies(nr_steps, &step, field);
			check_arg(tmp < 0,
				  "Invalid dependency at step %u!\n", nr_steps);

			valid++;
		}

		if ((field = strtok_r(fstart, ".", &fctx))) {
			fstart = NULL;

			check_arg(strlen(field) != 1 ||
				  (field[0] != '0' && field[0] != '1'),
				  "Invalid wait boolean at step %u!\n",
				  nr_steps);
			step.sync = field[0] - '0';

			valid++;
		}

		check_arg(valid != 5, "Invalid record at step %u!\n", nr_steps);

		step.type = BATCH;

add_step:
		step.idx = nr_steps++;
		step.request = -1;
		steps = realloc(steps, sizeof(step) * nr_steps);
		igt_assert(steps);

		memcpy(&steps[nr_steps - 1], &step, sizeof(step));

		free(token);
	}

	if (app_w) {
		steps = realloc(steps, sizeof(step) *
				(nr_steps + app_w->nr_steps));
		igt_assert(steps);

		memcpy(&steps[nr_steps], app_w->steps,
		       sizeof(step) * app_w->nr_steps);

		for (i = 0; i < app_w->nr_steps; i++)
			steps[nr_steps + i].idx += nr_steps;

		nr_steps += app_w->nr_steps;
	}

	wrk = malloc(sizeof(*wrk));
	igt_assert(wrk);

	wrk->nr_steps = nr_steps;
	wrk->steps = steps;
	wrk->prio = arg->prio;
	wrk->sseu = arg->sseu;
	wrk->max_working_set_id = -1;
	wrk->working_sets = NULL;
	wrk->bo_prng = (flags & SYNCEDCLIENTS) ? master_prng : rand();

	free(desc);

	/*
	 * Tag all steps which need to emit a sync fence if another step is
	 * referencing them as a sync fence dependency.
	 */
	for (i = 0; i < nr_steps; i++) {
		for (j = 0; j < steps[i].fence_deps.nr; j++) {
			tmp = steps[i].idx + steps[i].fence_deps.list[j].target;
			check_arg(tmp < 0 || tmp >= i ||
				  (steps[tmp].type != BATCH &&
				   steps[tmp].type != SW_FENCE),
				  "Invalid dependency target %u!\n", i);
			steps[tmp].emit_fence = -1;
		}
	}

	/* Validate SW_FENCE_SIGNAL targets. */
	for (i = 0; i < nr_steps; i++) {
		if (steps[i].type == SW_FENCE_SIGNAL) {
			tmp = steps[i].idx + steps[i].target;
			check_arg(tmp < 0 || tmp >= i ||
				  steps[tmp].type != SW_FENCE,
				  "Invalid sw fence target %u!\n", i);
		}
	}

	/*
	 * Check no duplicate working set ids.
	 */
	for (i = 0, w = wrk->steps; i < wrk->nr_steps; i++, w++) {
		struct w_step *w2;

		if (w->type != WORKINGSET)
			continue;

		for (j = 0, w2 = wrk->steps; j < wrk->nr_steps; w2++, j++) {
			if (j == i)
				continue;
			if (w2->type != WORKINGSET)
				continue;

			check_arg(w->working_set.id == w2->working_set.id,
				  "Duplicate working set id at %u!\n", j);
		}
	}

	/*
	 * Allocate shared working sets.
	 */
	for (i = 0, w = wrk->steps; i < wrk->nr_steps; i++, w++) {
		if (w->type == WORKINGSET && w->working_set.shared)
			allocate_working_set(wrk, &w->working_set);
	}

	wrk->max_working_set_id = -1;
	for (i = 0, w = wrk->steps; i < wrk->nr_steps; i++, w++) {
		if (w->type == WORKINGSET &&
		    w->working_set.shared &&
		    w->working_set.id > wrk->max_working_set_id)
			wrk->max_working_set_id = w->working_set.id;
	}

	wrk->working_sets = calloc(wrk->max_working_set_id + 1,
				   sizeof(*wrk->working_sets));
	igt_assert(wrk->working_sets);

	for (i = 0, w = wrk->steps; i < wrk->nr_steps; i++, w++) {
		if (w->type == WORKINGSET && w->working_set.shared)
			wrk->working_sets[w->working_set.id] = &w->working_set;
	}

	return wrk;
}

static struct workload *
clone_workload(struct workload *_wrk)
{
	struct workload *wrk;
	int i;

	wrk = malloc(sizeof(*wrk));
	igt_assert(wrk);
	memset(wrk, 0, sizeof(*wrk));

	wrk->prio = _wrk->prio;
	wrk->sseu = _wrk->sseu;
	wrk->nr_steps = _wrk->nr_steps;
	wrk->steps = calloc(wrk->nr_steps, sizeof(struct w_step));
	igt_assert(wrk->steps);

	memcpy(wrk->steps, _wrk->steps, sizeof(struct w_step) * wrk->nr_steps);

	wrk->max_working_set_id = _wrk->max_working_set_id;
	if (wrk->max_working_set_id >= 0) {
		wrk->working_sets = calloc(wrk->max_working_set_id + 1,
					sizeof(*wrk->working_sets));
		igt_assert(wrk->working_sets);

		memcpy(wrk->working_sets,
		       _wrk->working_sets,
		       (wrk->max_working_set_id + 1) *
		       sizeof(*wrk->working_sets));
	}

	/* Check if we need a sw sync timeline. */
	for (i = 0; i < wrk->nr_steps; i++) {
		if (wrk->steps[i].type == SW_FENCE) {
			wrk->sync_timeline = sw_sync_timeline_create();
			igt_assert(wrk->sync_timeline >= 0);
			break;
		}
	}

	for (i = 0; i < NUM_ENGINES; i++)
		IGT_INIT_LIST_HEAD(&wrk->requests[i]);

	return wrk;
}

#define rounddown(x, y) (x - (x%y))
#ifndef PAGE_SIZE
#define PAGE_SIZE (4096)
#endif

static unsigned int get_duration(struct workload *wrk, struct w_step *w)
{
	struct duration *dur = &w->duration;

	if (dur->min == dur->max)
		return dur->min;
	else
		return dur->min + hars_petruska_f54_1_random(&wrk->bb_prng) %
		       (dur->max + 1 - dur->min);
}

static struct ctx *
__get_ctx(struct workload *wrk, const struct w_step *w)
{
	return &wrk->ctx_list[w->context];
}

static unsigned long
__get_bb_sz(const struct w_step *w, unsigned int duration)
{
	enum intel_engine_id engine = w->engine;
	struct ctx *ctx = __get_ctx(w->wrk, w);
	unsigned long d;

	if (ctx->engine_map && engine == DEFAULT)
		/* Assume first engine calibration. */
		engine = ctx->engine_map[0];

	igt_assert(engine_calib_map[engine]);
	d = ALIGN(duration * engine_calib_map[engine] * sizeof(uint32_t) /
		  nop_calibration_us,
		  sizeof(uint32_t));

	return d;
}

static unsigned long
get_bb_sz(const struct w_step *w, unsigned int duration)
{
	unsigned long d = __get_bb_sz(w, duration);

	igt_assert(d);

	return d;
}

static void init_bb(struct w_step *w)
{
	const unsigned int arb_period =
			__get_bb_sz(w, w->preempt_us) / sizeof(uint32_t);
	const unsigned int mmap_len = ALIGN(w->bb_sz, 4096);
	unsigned int i;
	uint32_t *ptr;

	if (w->unbound_duration || !arb_period)
		return;

	gem_set_domain(fd, w->bb_handle,
		       I915_GEM_DOMAIN_WC, I915_GEM_DOMAIN_WC);

	ptr = gem_mmap__wc(fd, w->bb_handle, 0, mmap_len, PROT_WRITE);

	for (i = arb_period; i < w->bb_sz / sizeof(uint32_t); i += arb_period)
		ptr[i] = 0x5 << 23; /* MI_ARB_CHK */

	munmap(ptr, mmap_len);
}

static unsigned int terminate_bb(struct w_step *w)
{
	const uint32_t bbe = 0xa << 23;
	unsigned long mmap_start, mmap_len;
	unsigned long batch_start = w->bb_sz;
	unsigned int r = 0;
	uint32_t *ptr, *cs;

	batch_start -= sizeof(uint32_t); /* bbend */

	if (w->unbound_duration)
		batch_start -= 4 * sizeof(uint32_t); /* MI_ARB_CHK + MI_BATCH_BUFFER_START */

	mmap_start = rounddown(batch_start, PAGE_SIZE);
	mmap_len = ALIGN(w->bb_sz - mmap_start, PAGE_SIZE);

	gem_set_domain(fd, w->bb_handle,
		       I915_GEM_DOMAIN_WC, I915_GEM_DOMAIN_WC);

	ptr = gem_mmap__wc(fd, w->bb_handle, mmap_start, mmap_len, PROT_WRITE);
	cs = (uint32_t *)((char *)ptr + batch_start - mmap_start);

	if (w->unbound_duration) {
		w->reloc[r++].offset = batch_start + 2 * sizeof(uint32_t);
		batch_start += 4 * sizeof(uint32_t);

		*cs++ = w->preempt_us ? 0x5 << 23 /* MI_ARB_CHK; */ : MI_NOOP;
		w->recursive_bb_start = cs;
		*cs++ = MI_BATCH_BUFFER_START | 1 << 8 | 1;
		*cs++ = 0;
		*cs++ = 0;
	}

	*cs = bbe;

	return r;
}

static const unsigned int eb_engine_map[NUM_ENGINES] = {
	[DEFAULT] = I915_EXEC_DEFAULT,
	[RCS] = I915_EXEC_RENDER,
	[BCS] = I915_EXEC_BLT,
	[VCS] = I915_EXEC_BSD,
	[VCS1] = I915_EXEC_BSD | I915_EXEC_BSD_RING1,
	[VCS2] = I915_EXEC_BSD | I915_EXEC_BSD_RING2,
	[VECS] = I915_EXEC_VEBOX
};

static void
eb_set_engine(struct drm_i915_gem_execbuffer2 *eb, enum intel_engine_id engine)
{
	eb->flags = eb_engine_map[engine];
}

static unsigned int
find_engine_in_map(struct ctx *ctx, enum intel_engine_id engine)
{
	unsigned int i;

	for (i = 0; i < ctx->engine_map_count; i++) {
		if (ctx->engine_map[i] == engine)
			return i + 1;
	}

	igt_assert(ctx->load_balance);
	return 0;
}

static void
eb_update_flags(struct workload *wrk, struct w_step *w,
		enum intel_engine_id engine)
{
	struct ctx *ctx = __get_ctx(wrk, w);

	if (ctx->engine_map)
		w->eb.flags = find_engine_in_map(ctx, engine);
	else
		eb_set_engine(&w->eb, engine);

	w->eb.flags |= I915_EXEC_HANDLE_LUT;
	w->eb.flags |= I915_EXEC_NO_RELOC;

	igt_assert(w->emit_fence <= 0);
	if (w->emit_fence)
		w->eb.flags |= I915_EXEC_FENCE_OUT;
}

static uint32_t
get_ctxid(struct workload *wrk, struct w_step *w)
{
	return wrk->ctx_list[w->context].id;
}

static uint32_t alloc_bo(int i915, unsigned long size)
{
	return gem_create(i915, size);
}

static void
alloc_step_batch(struct workload *wrk, struct w_step *w)
{
	enum intel_engine_id engine = w->engine;
	unsigned int j = 0;
	unsigned int nr_obj = 2 + w->data_deps.nr;
	unsigned int i;

	w->obj = calloc(nr_obj, sizeof(*w->obj));
	igt_assert(w->obj);

	w->obj[j].handle = alloc_bo(fd, 4096);
	w->obj[j].flags = EXEC_OBJECT_WRITE;
	j++;
	igt_assert(j < nr_obj);

	for (i = 0; i < w->data_deps.nr; i++) {
		struct dep_entry *entry = &w->data_deps.list[i];
		uint32_t dep_handle;

		if (entry->working_set == -1) {
			int dep_idx = w->idx + entry->target;

			igt_assert(entry->target <= 0);
			igt_assert(dep_idx >= 0 && dep_idx < w->idx);
			igt_assert(wrk->steps[dep_idx].type == BATCH);

			dep_handle = wrk->steps[dep_idx].obj[0].handle;
		} else {
			struct working_set *set;

			igt_assert(entry->working_set <=
				   wrk->max_working_set_id);

			set = wrk->working_sets[entry->working_set];

			igt_assert(set->nr);
			igt_assert(entry->target < set->nr);
			igt_assert(set->sizes[entry->target].size);

			dep_handle = set->handles[entry->target];
		}

		w->obj[j].flags = entry->write ? EXEC_OBJECT_WRITE : 0;
		w->obj[j].handle = dep_handle;
		j++;
		igt_assert(j < nr_obj);
	}

	if (w->unbound_duration)
		/* nops + MI_ARB_CHK + MI_BATCH_BUFFER_START */
		w->bb_sz = max(PAGE_SIZE, __get_bb_sz(w, w->preempt_us)) +
			   (1 + 3) * sizeof(uint32_t);
	else
		w->bb_sz = get_bb_sz(w, w->duration.max);

	w->bb_handle = w->obj[j].handle =
		alloc_bo(fd, w->bb_sz + (w->unbound_duration ? 4096 : 0));
	init_bb(w);
	w->obj[j].relocation_count = terminate_bb(w);

	if (w->obj[j].relocation_count) {
		igt_assert(w->unbound_duration);
		w->obj[j].relocs_ptr = to_user_pointer(&w->reloc);
		w->reloc[0].target_handle = j;
	}

	w->eb.buffers_ptr = to_user_pointer(w->obj);
	w->eb.buffer_count = j + 1;
	w->eb.rsvd1 = get_ctxid(wrk, w);

	eb_update_flags(wrk, w, engine);
#ifdef DEBUG
	printf("%u: %u:|", w->idx, w->eb.buffer_count);
	for (i = 0; i <= j; i++)
		printf("%x|", w->obj[i].handle);
	printf(" %10lu flags=%llx bb=%x[%u] ctx[%u]=%u\n",
		w->bb_sz, w->eb.flags, w->bb_handle, j, w->context,
		get_ctxid(wrk, w));
#endif
}

static bool set_priority(uint32_t ctx_id, int prio)
{
	struct drm_i915_gem_context_param param = {
		.ctx_id = ctx_id,
		.param = I915_CONTEXT_PARAM_PRIORITY,
		.value = prio,
	};

	return __gem_context_set_param(fd, &param) == 0;
}

static bool set_persistence(uint32_t ctx_id, bool state)
{
	struct drm_i915_gem_context_param param = {
		.ctx_id = ctx_id,
		.param = I915_CONTEXT_PARAM_PERSISTENCE,
		.value = state,
	};

	return __gem_context_set_param(fd, &param) == 0;
}

static void __configure_context(uint32_t ctx_id, unsigned int prio)
{
	set_priority(ctx_id, prio);

	/* Mark as non-persistent if supported. */
	set_persistence(ctx_id, false);
}

static int __vm_destroy(int i915, uint32_t vm_id)
{
	struct drm_i915_gem_vm_control ctl = { .vm_id = vm_id };
	int err = 0;

	if (igt_ioctl(i915, DRM_IOCTL_I915_GEM_VM_DESTROY, &ctl)) {
		err = -errno;
		igt_assume(err);
	}

	errno = 0;
	return err;
}

static void vm_destroy(int i915, uint32_t vm_id)
{
	igt_assert_eq(__vm_destroy(i915, vm_id), 0);
}

static unsigned int
find_engine(struct i915_engine_class_instance *ci, unsigned int count,
	    enum intel_engine_id engine)
{
	struct i915_engine_class_instance e = get_engine(engine);
	unsigned int i;

	for (i = 0; i < count; i++, ci++) {
		if (!memcmp(&e, ci, sizeof(*ci)))
			return i;
	}

	igt_assert(0);
	return 0;
}

static struct drm_i915_gem_context_param_sseu get_device_sseu(void)
{
	struct drm_i915_gem_context_param param = { };

	if (device_sseu.slice_mask == -1) {
		param.param = I915_CONTEXT_PARAM_SSEU;
		param.value = (uintptr_t)&device_sseu;

		gem_context_get_param(fd, &param);
	}

	return device_sseu;
}

static uint64_t
set_ctx_sseu(struct ctx *ctx, uint64_t slice_mask)
{
	struct drm_i915_gem_context_param_sseu sseu = get_device_sseu();
	struct drm_i915_gem_context_param param = { };

	if (slice_mask == -1)
		slice_mask = device_sseu.slice_mask;

	if (ctx->engine_map && ctx->load_balance) {
		sseu.flags = I915_CONTEXT_SSEU_FLAG_ENGINE_INDEX;
		sseu.engine.engine_class = I915_ENGINE_CLASS_INVALID;
		sseu.engine.engine_instance = 0;
	}

	sseu.slice_mask = slice_mask;

	param.ctx_id = ctx->id;
	param.param = I915_CONTEXT_PARAM_SSEU;
	param.size = sizeof(sseu);
	param.value = (uintptr_t)&sseu;

	gem_context_set_param(fd, &param);

	return slice_mask;
}

static size_t sizeof_load_balance(int count)
{
	return offsetof(struct i915_context_engines_load_balance,
			engines[count]);
}

static size_t sizeof_param_engines(int count)
{
	return offsetof(struct i915_context_param_engines,
			engines[count]);
}

static size_t sizeof_engines_bond(int count)
{
	return offsetof(struct i915_context_engines_bond,
			engines[count]);
}

static unsigned long
get_buffer_size(struct workload *wrk, const struct work_buffer_size *sz)
{
	if (sz->min == sz->max)
		return sz->min;
	else
		return sz->min + hars_petruska_f54_1_random(&wrk->bo_prng) %
		       (sz->max + 1 - sz->min);
}

static void allocate_working_set(struct workload *wrk, struct working_set *set)
{
	unsigned int i;

	set->handles = calloc(set->nr, sizeof(*set->handles));
	igt_assert(set->handles);

	for (i = 0; i < set->nr; i++) {
		set->sizes[i].size = get_buffer_size(wrk, &set->sizes[i]);
		set->handles[i] = alloc_bo(fd, set->sizes[i].size);
	}
}

#define alloca0(sz) ({ size_t sz__ = (sz); memset(alloca(sz__), 0, sz__); })

static int prepare_workload(unsigned int id, struct workload *wrk)
{
	struct working_set **sets;
	uint32_t share_vm = 0;
	int max_ctx = -1;
	struct w_step *w;
	int i, j;

	wrk->id = id;
	wrk->bb_prng = (wrk->flags & SYNCEDCLIENTS) ? master_prng : rand();
	wrk->bo_prng = (wrk->flags & SYNCEDCLIENTS) ? master_prng : rand();
	wrk->run = true;

	/*
	 * Pre-scan workload steps to allocate context list storage.
	 */
	for (i = 0, w = wrk->steps; i < wrk->nr_steps; i++, w++) {
		int ctx = w->context + 1;
		int delta;

		w->wrk = wrk;

		if (ctx <= max_ctx)
			continue;

		delta = ctx + 1 - wrk->nr_ctxs;

		wrk->nr_ctxs += delta;
		wrk->ctx_list = realloc(wrk->ctx_list,
					wrk->nr_ctxs * sizeof(*wrk->ctx_list));
		memset(&wrk->ctx_list[wrk->nr_ctxs - delta], 0,
			delta * sizeof(*wrk->ctx_list));

		max_ctx = ctx;
	}

	/*
	 * Transfer over engine map configuration from the workload step.
	 */
	for (j = 0; j < wrk->nr_ctxs; j++) {
		struct ctx *ctx = &wrk->ctx_list[j];

		for (i = 0, w = wrk->steps; i < wrk->nr_steps; i++, w++) {
			if (w->context != j)
				continue;

			if (w->type == ENGINE_MAP) {
				ctx->engine_map = w->engine_map;
				ctx->engine_map_count = w->engine_map_count;
			} else if (w->type == LOAD_BALANCE) {
				if (!ctx->engine_map) {
					wsim_err("Load balancing needs an engine map!\n");
					return 1;
				}
				ctx->load_balance = w->load_balance;
			} else if (w->type == BOND) {
				if (!ctx->load_balance) {
					wsim_err("Engine bonds need load balancing engine map!\n");
					return 1;
				}
				ctx->bond_count++;
				ctx->bonds = realloc(ctx->bonds,
						     ctx->bond_count *
						     sizeof(struct bond));
				igt_assert(ctx->bonds);
				ctx->bonds[ctx->bond_count - 1].mask =
					w->bond_mask;
				ctx->bonds[ctx->bond_count - 1].master =
					w->bond_master;
			}
		}
	}

	/*
	 * Create and configure contexts.
	 */
	for (i = 0; i < wrk->nr_ctxs; i++) {
		struct drm_i915_gem_context_create_ext_setparam ext = {
			.base.name = I915_CONTEXT_CREATE_EXT_SETPARAM,
			.param.param = I915_CONTEXT_PARAM_VM,
		};
		struct drm_i915_gem_context_create_ext args = { };
		struct ctx *ctx = &wrk->ctx_list[i];
		uint32_t ctx_id;

		igt_assert(!ctx->id);

		/* Find existing context to share ppgtt with. */
		for (j = 0; !share_vm && j < wrk->nr_ctxs; j++) {
			struct drm_i915_gem_context_param param = {
				.param = I915_CONTEXT_PARAM_VM,
				.ctx_id = wrk->ctx_list[j].id,
			};

			if (!param.ctx_id)
				continue;

			gem_context_get_param(fd, &param);
			igt_assert(param.value);
			share_vm = param.value;
			break;
		}

		if (share_vm) {
			ext.param.value = share_vm;
			args.flags = I915_CONTEXT_CREATE_FLAGS_USE_EXTENSIONS;
			args.extensions = to_user_pointer(&ext);
		}

		drmIoctl(fd, DRM_IOCTL_I915_GEM_CONTEXT_CREATE_EXT, &args);
		igt_assert(args.ctx_id);

		ctx_id = args.ctx_id;
		ctx->id = ctx_id;
		ctx->sseu = device_sseu.slice_mask;

		__configure_context(ctx_id, wrk->prio);

		if (ctx->engine_map) {
			struct i915_context_param_engines *set_engines =
				alloca0(sizeof_param_engines(ctx->engine_map_count + 1));
			struct i915_context_engines_load_balance *load_balance =
				alloca0(sizeof_load_balance(ctx->engine_map_count));
			struct drm_i915_gem_context_param param = {
				.ctx_id = ctx_id,
				.param = I915_CONTEXT_PARAM_ENGINES,
				.size = sizeof_param_engines(ctx->engine_map_count + 1),
				.value = to_user_pointer(set_engines),
			};
			struct i915_context_engines_bond *last = NULL;

			if (ctx->load_balance) {
				set_engines->extensions =
					to_user_pointer(load_balance);

				load_balance->base.name =
					I915_CONTEXT_ENGINES_EXT_LOAD_BALANCE;
				load_balance->num_siblings =
					ctx->engine_map_count;

				for (j = 0; j < ctx->engine_map_count; j++)
					load_balance->engines[j] =
						get_engine(ctx->engine_map[j]);
			}

			/* Reserve slot for virtual engine. */
			set_engines->engines[0].engine_class =
				I915_ENGINE_CLASS_INVALID;
			set_engines->engines[0].engine_instance =
				I915_ENGINE_CLASS_INVALID_NONE;

			for (j = 1; j <= ctx->engine_map_count; j++)
				set_engines->engines[j] =
					get_engine(ctx->engine_map[j - 1]);

			last = NULL;
			for (j = 0; j < ctx->bond_count; j++) {
				unsigned long mask = ctx->bonds[j].mask;
				struct i915_context_engines_bond *bond =
					alloca0(sizeof_engines_bond(__builtin_popcount(mask)));
				unsigned int b, e;

				bond->base.next_extension = to_user_pointer(last);
				bond->base.name = I915_CONTEXT_ENGINES_EXT_BOND;

				bond->virtual_index = 0;
				bond->master = get_engine(ctx->bonds[j].master);

				for (b = 0, e = 0; mask; e++, mask >>= 1) {
					unsigned int idx;

					if (!(mask & 1))
						continue;

					idx = find_engine(&set_engines->engines[1],
							  ctx->engine_map_count,
							  e);
					bond->engines[b++] =
						set_engines->engines[1 + idx];
				}

				last = bond;
			}
			load_balance->base.next_extension = to_user_pointer(last);

			gem_context_set_param(fd, &param);
		}

		if (wrk->sseu) {
			/* Set to slice 0 only, one slice. */
			ctx->sseu = set_ctx_sseu(ctx, 1);
		}
	}

	if (share_vm)
		vm_destroy(fd, share_vm);

	/* Record default preemption. */
	for (i = 0, w = wrk->steps; i < wrk->nr_steps; i++, w++) {
		if (w->type == BATCH)
			w->preempt_us = 100;
	}

	/*
	 * Scan for contexts with modified preemption config and record their
	 * preemption period for the following steps belonging to the same
	 * context.
	 */
	for (i = 0, w = wrk->steps; i < wrk->nr_steps; i++, w++) {
		struct w_step *w2;

		if (w->type != PREEMPTION)
			continue;

		for (j = i + 1; j < wrk->nr_steps; j++) {
			w2 = &wrk->steps[j];

			if (w2->context != w->context)
				continue;
			else if (w2->type == PREEMPTION)
				break;
			else if (w2->type != BATCH)
				continue;

			w2->preempt_us = w->period;
		}
	}

	/*
	 * Scan for SSEU control steps.
	 */
	for (i = 0, w = wrk->steps; i < wrk->nr_steps; i++, w++) {
		if (w->type == SSEU) {
			get_device_sseu();
			break;
		}
	}

	/*
	 * Allocate working sets.
	 */
	for (i = 0, w = wrk->steps; i < wrk->nr_steps; i++, w++) {
		if (w->type == WORKINGSET && !w->working_set.shared)
			allocate_working_set(wrk, &w->working_set);
	}

	/*
	 * Map of working set ids.
	 */
	wrk->max_working_set_id = -1;
	for (i = 0, w = wrk->steps; i < wrk->nr_steps; i++, w++) {
		if (w->type == WORKINGSET &&
		    w->working_set.id > wrk->max_working_set_id)
			wrk->max_working_set_id = w->working_set.id;
	}

	sets = wrk->working_sets;
	wrk->working_sets = calloc(wrk->max_working_set_id + 1,
				   sizeof(*wrk->working_sets));
	igt_assert(wrk->working_sets);

	for (i = 0, w = wrk->steps; i < wrk->nr_steps; i++, w++) {
		struct working_set *set;

		if (w->type != WORKINGSET)
			continue;

		if (!w->working_set.shared) {
			set = &w->working_set;
		} else {
			igt_assert(sets);

			set = sets[w->working_set.id];
			igt_assert(set->shared);
			igt_assert(set->sizes);
		}

		wrk->working_sets[w->working_set.id] = set;
	}

	if (sets)
		free(sets);

	/*
	 * Allocate batch buffers.
	 */
	for (i = 0, w = wrk->steps; i < wrk->nr_steps; i++, w++) {
		if (w->type != BATCH)
			continue;

		alloc_step_batch(wrk, w);
	}

	return 0;
}

static double elapsed(const struct timespec *start, const struct timespec *end)
{
	return (end->tv_sec - start->tv_sec) +
	       (end->tv_nsec - start->tv_nsec) / 1e9;
}

static int elapsed_us(const struct timespec *start, const struct timespec *end)
{
	return elapsed(start, end) * 1e6;
}

static void
update_bb_start(struct w_step *w)
{
	if (!w->unbound_duration)
		return;

	gem_set_domain(fd, w->bb_handle,
		       I915_GEM_DOMAIN_WC, I915_GEM_DOMAIN_WC);

	*w->recursive_bb_start = MI_BATCH_BUFFER_START | (1 << 8) | 1;
}

static void w_sync_to(struct workload *wrk, struct w_step *w, int target)
{
	if (target < 0)
		target = wrk->nr_steps + target;

	igt_assert(target < wrk->nr_steps);

	while (wrk->steps[target].type != BATCH) {
		if (--target < 0)
			target = wrk->nr_steps + target;
	}

	igt_assert(target < wrk->nr_steps);
	igt_assert(wrk->steps[target].type == BATCH);

	gem_sync(fd, wrk->steps[target].obj[0].handle);
}

static void
do_eb(struct workload *wrk, struct w_step *w, enum intel_engine_id engine)
{
	unsigned int i;

	eb_update_flags(wrk, w, engine);
	update_bb_start(w);

	w->eb.batch_start_offset =
		w->unbound_duration ?
		0 :
		ALIGN(w->bb_sz - get_bb_sz(w, get_duration(wrk, w)),
		      2 * sizeof(uint32_t));

	for (i = 0; i < w->fence_deps.nr; i++) {
		int tgt = w->idx + w->fence_deps.list[i].target;

		/* TODO: fence merging needed to support multiple inputs */
		igt_assert(i == 0);
		igt_assert(tgt >= 0 && tgt < w->idx);
		igt_assert(wrk->steps[tgt].emit_fence > 0);

		if (w->fence_deps.submit_fence)
			w->eb.flags |= I915_EXEC_FENCE_SUBMIT;
		else
			w->eb.flags |= I915_EXEC_FENCE_IN;

		w->eb.rsvd2 = wrk->steps[tgt].emit_fence;
	}

	if (w->eb.flags & I915_EXEC_FENCE_OUT)
		gem_execbuf_wr(fd, &w->eb);
	else
		gem_execbuf(fd, &w->eb);

	if (w->eb.flags & I915_EXEC_FENCE_OUT) {
		w->emit_fence = w->eb.rsvd2 >> 32;
		igt_assert(w->emit_fence > 0);
	}
}

static void sync_deps(struct workload *wrk, struct w_step *w)
{
	unsigned int i;

	for (i = 0; i < w->data_deps.nr; i++) {
		struct dep_entry *entry = &w->data_deps.list[i];
		int dep_idx;

		if (entry->working_set == -1)
			continue;

		igt_assert(entry->target <= 0);

		if (!entry->target)
			continue;

		dep_idx = w->idx + entry->target;

		igt_assert(dep_idx >= 0 && dep_idx < w->idx);
		igt_assert(wrk->steps[dep_idx].type == BATCH);

		gem_sync(fd, wrk->steps[dep_idx].obj[0].handle);
	}
}

static void *run_workload(void *data)
{
	struct workload *wrk = (struct workload *)data;
	struct timespec t_start, t_end;
	struct w_step *w;
	int throttle = -1;
	int qd_throttle = -1;
	int count, missed = 0;
	unsigned long time_tot = 0, time_min = ULONG_MAX, time_max = 0;
	int i;

	clock_gettime(CLOCK_MONOTONIC, &t_start);

	for (count = 0; wrk->run && (wrk->background || count < wrk->repeat);
	     count++) {
		unsigned int cur_seqno = wrk->sync_seqno;

		clock_gettime(CLOCK_MONOTONIC, &wrk->repeat_start);

		for (i = 0, w = wrk->steps; wrk->run && (i < wrk->nr_steps);
		     i++, w++) {
			enum intel_engine_id engine = w->engine;
			int do_sleep = 0;

			if (w->type == DELAY) {
				do_sleep = w->delay;
			} else if (w->type == PERIOD) {
				struct timespec now;
				int elapsed;

				clock_gettime(CLOCK_MONOTONIC, &now);
				elapsed = elapsed_us(&wrk->repeat_start, &now);
				do_sleep = w->period - elapsed;
				time_tot += elapsed;
				if (elapsed < time_min)
					time_min = elapsed;
				if (elapsed > time_max)
					time_max = elapsed;
				if (do_sleep < 0) {
					missed++;
					if (verbose > 2)
						printf("%u: Dropped period @ %u/%u (%dus late)!\n",
						       wrk->id, count, i, do_sleep);
					continue;
				}
			} else if (w->type == SYNC) {
				unsigned int s_idx = i + w->target;

				igt_assert(s_idx >= 0 && s_idx < i);
				igt_assert(wrk->steps[s_idx].type == BATCH);
				gem_sync(fd, wrk->steps[s_idx].obj[0].handle);
				continue;
			} else if (w->type == THROTTLE) {
				throttle = w->throttle;
				continue;
			} else if (w->type == QD_THROTTLE) {
				qd_throttle = w->throttle;
				continue;
			} else if (w->type == SW_FENCE) {
				igt_assert(w->emit_fence < 0);
				w->emit_fence =
					sw_sync_timeline_create_fence(wrk->sync_timeline,
								      cur_seqno + w->idx);
				igt_assert(w->emit_fence > 0);
				continue;
			} else if (w->type == SW_FENCE_SIGNAL) {
				int tgt = w->idx + w->target;
				int inc;

				igt_assert(tgt >= 0 && tgt < i);
				igt_assert(wrk->steps[tgt].type == SW_FENCE);
				cur_seqno += wrk->steps[tgt].idx;
				inc = cur_seqno - wrk->sync_seqno;
				sw_sync_timeline_inc(wrk->sync_timeline, inc);
				continue;
			} else if (w->type == CTX_PRIORITY) {
				if (w->priority != wrk->ctx_list[w->context].priority) {
					struct drm_i915_gem_context_param param = {
						.ctx_id = wrk->ctx_list[w->context].id,
						.param = I915_CONTEXT_PARAM_PRIORITY,
						.value = w->priority,
					};

					gem_context_set_param(fd, &param);
					wrk->ctx_list[w->context].priority =
								    w->priority;
				}
				continue;
			} else if (w->type == TERMINATE) {
				unsigned int t_idx = i + w->target;

				igt_assert(t_idx >= 0 && t_idx < i);
				igt_assert(wrk->steps[t_idx].type == BATCH);
				igt_assert(wrk->steps[t_idx].unbound_duration);

				*wrk->steps[t_idx].recursive_bb_start =
					MI_BATCH_BUFFER_END;
				__sync_synchronize();
				continue;
			} else if (w->type == SSEU) {
				if (w->sseu != wrk->ctx_list[w->context * 2].sseu) {
					wrk->ctx_list[w->context * 2].sseu =
						set_ctx_sseu(&wrk->ctx_list[w->context * 2],
							     w->sseu);
				}
				continue;
			} else if (w->type == PREEMPTION ||
				   w->type == ENGINE_MAP ||
				   w->type == LOAD_BALANCE ||
				   w->type == BOND ||
				   w->type == WORKINGSET) {
				   /* No action for these at execution time. */
				continue;
			}

			if (do_sleep || w->type == PERIOD) {
				usleep(do_sleep);
				continue;
			}

			igt_assert(w->type == BATCH);

			if (wrk->flags & DEPSYNC)
				sync_deps(wrk, w);

			if (throttle > 0)
				w_sync_to(wrk, w, i - throttle);

			do_eb(wrk, w, engine);

			if (w->request != -1) {
				igt_list_del(&w->rq_link);
				wrk->nrequest[w->request]--;
			}
			w->request = engine;
			igt_list_add_tail(&w->rq_link, &wrk->requests[engine]);
			wrk->nrequest[engine]++;

			if (!wrk->run)
				break;

			if (w->sync)
				gem_sync(fd, w->obj[0].handle);

			if (qd_throttle > 0) {
				while (wrk->nrequest[engine] > qd_throttle) {
					struct w_step *s;

					s = igt_list_first_entry(&wrk->requests[engine],
								 s, rq_link);

					gem_sync(fd, s->obj[0].handle);

					s->request = -1;
					igt_list_del(&s->rq_link);
					wrk->nrequest[engine]--;
				}
			}
		}

		if (wrk->sync_timeline) {
			int inc;

			inc = wrk->nr_steps - (cur_seqno - wrk->sync_seqno);
			sw_sync_timeline_inc(wrk->sync_timeline, inc);
			wrk->sync_seqno += wrk->nr_steps;
		}

		/* Cleanup all fences instantiated in this iteration. */
		for (i = 0, w = wrk->steps; wrk->run && (i < wrk->nr_steps);
		     i++, w++) {
			if (w->emit_fence > 0) {
				close(w->emit_fence);
				w->emit_fence = -1;
			}
		}
	}

	for (i = 0; i < NUM_ENGINES; i++) {
		if (!wrk->nrequest[i])
			continue;

		w = igt_list_last_entry(&wrk->requests[i], w, rq_link);
		gem_sync(fd, w->obj[0].handle);
	}

	clock_gettime(CLOCK_MONOTONIC, &t_end);

	if (wrk->print_stats) {
		double t = elapsed(&t_start, &t_end);

		printf("%c%u: %.3fs elapsed (%d cycles, %.3f workloads/s).",
		       wrk->background ? ' ' : '*', wrk->id,
		       t, count, count / t);
		if (time_tot)
			printf(" Time avg/min/max=%lu/%lu/%luus; %u missed.",
			       time_tot / count, time_min, time_max, missed);
		putchar('\n');
	}

	return NULL;
}

static void fini_workload(struct workload *wrk)
{
	free(wrk->steps);
	free(wrk);
}

static unsigned long calibrate_nop(unsigned int tolerance_pct, struct intel_execution_engine2 *engine)
{
	const uint32_t bbe = 0xa << 23;
	unsigned int loops = 17;
	unsigned int usecs = nop_calibration_us;
	struct drm_i915_gem_exec_object2 obj = {};
	struct drm_i915_gem_execbuffer2 eb = {
		.buffer_count = 1,
		.buffers_ptr = (uintptr_t)&obj,
		.flags = engine->flags
	};
	long size, last_size;
	struct timespec t_0, t_end;

	clock_gettime(CLOCK_MONOTONIC, &t_0);

	size = 256 * 1024;
	do {
		struct timespec t_start;

		obj.handle = alloc_bo(fd, size);
		gem_write(fd, obj.handle, size - sizeof(bbe), &bbe,
			  sizeof(bbe));
		gem_execbuf(fd, &eb);
		gem_sync(fd, obj.handle);

		clock_gettime(CLOCK_MONOTONIC, &t_start);
		for (int loop = 0; loop < loops; loop++)
			gem_execbuf(fd, &eb);
		gem_sync(fd, obj.handle);
		clock_gettime(CLOCK_MONOTONIC, &t_end);

		gem_close(fd, obj.handle);

		last_size = size;
		size = loops * size / elapsed(&t_start, &t_end) / 1e6 * usecs;
		size = ALIGN(size, sizeof(uint32_t));
	} while (elapsed(&t_0, &t_end) < 5 ||
		 labs(size - last_size) > (size * tolerance_pct / 100));

	return size / sizeof(uint32_t);
}

static void
calibrate_sequentially(void)
{
	struct intel_execution_engine2 *engine;
	enum intel_engine_id eng_id;

	__for_each_physical_engine(fd, engine) {
		eng_id = ci_to_engine_id(engine->class, engine->instance);
		igt_assert(eng_id >= 0);
		engine_calib_map[eng_id] = calibrate_nop(fd, engine);
	}
}

struct thread_data {
	struct intel_execution_engine2 *eng;
	pthread_t thr;
	unsigned long calib;
};

static void *
engine_calibration_thread(void *data)
{
	struct thread_data *thr_d = (struct thread_data *) data;

	thr_d->calib = calibrate_nop(fd, thr_d->eng);
	return NULL;
}

static void
calibrate_in_parallel(void)
{
	struct thread_data *thr_d = malloc(NUM_ENGINES * sizeof(*thr_d));
	struct intel_execution_engine2 *engine;
	enum intel_engine_id id;
	int ret;

	__for_each_physical_engine(fd, engine) {
		id = ci_to_engine_id(engine->class, engine->instance);
		thr_d[id].eng = engine;
		ret = pthread_create(&thr_d[id].thr, NULL, engine_calibration_thread, &thr_d[id]);
		igt_assert_eq(ret, 0);
	}

	__for_each_physical_engine(fd, engine) {
		id = ci_to_engine_id(engine->class, engine->instance);
		igt_assert(id >= 0);

		ret = pthread_join(thr_d[id].thr, NULL);
		igt_assert_eq(ret, 0);
		engine_calib_map[id] = thr_d[id].calib;
	}

	free(thr_d);
}

static void
calibrate_engines(void)
{
	if (sequential)
		calibrate_sequentially();
	else
		calibrate_in_parallel();
}

static void print_help(void)
{
	puts(
"Usage: gem_wsim [OPTIONS]\n"
"\n"
"Runs a simulated workload on the GPU.\n"
"When ran without arguments performs a GPU calibration result of which needs to\n"
"be provided when running the simulation in subsequent invocations.\n"
"\n"
"Options:\n"
"  -h                This text.\n"
"  -q                Be quiet - do not output anything to stdout.\n"
"  -n <n |           Nop calibration value - single value is set to all engines\n"
"  e1=v1,e2=v2,n...> without specified value; you can also specify calibrations for\n"
"                    particular engines.\n"
"  -t <n>            Nop calibration tolerance percentage.\n"
"  -T                Disable sequential calibration and perform calibration in parallel.\n"
"                    Use when there is a difficulty obtaining calibration with the\n"
"                    default settings.\n"
"  -I <n>            Initial randomness seed.\n"
"  -p <n>            Context priority to use for the following workload on the\n"
"                    command line.\n"
"  -w <desc|path>    Filename or a workload descriptor.\n"
"                    Can be given multiple times.\n"
"  -W <desc|path>    Filename or a master workload descriptor.\n"
"                    Only one master workload can be optinally specified in which\n"
"                    case all other workloads become background ones and run as\n"
"                    long as the master.\n"
"  -a <desc|path>    Append a workload to all other workloads.\n"
"  -r <n>            How many times to emit the workload.\n"
"  -c <n>            Fork N clients emitting the workload simultaneously.\n"
"  -s                Turn on small SSEU config for the next workload on the\n"
"                    command line. Subsequent -s switches it off.\n"
"  -S                Synchronize the sequence of random batch durations between\n"
"                    clients.\n"
"  -d                Sync between data dependencies in userspace."
	);
}

static char *load_workload_descriptor(char *filename)
{
	struct stat sbuf;
	char *buf;
	int infd, ret, i;
	ssize_t len;

	ret = stat(filename, &sbuf);
	if (ret || !S_ISREG(sbuf.st_mode))
		return filename;

	igt_assert(sbuf.st_size < 1024 * 1024); /* Just so. */
	buf = malloc(sbuf.st_size);
	igt_assert(buf);

	infd = open(filename, O_RDONLY);
	igt_assert(infd >= 0);
	len = read(infd, buf, sbuf.st_size);
	igt_assert(len == sbuf.st_size);
	close(infd);

	for (i = 0; i < len; i++) {
		if (buf[i] == '\n')
			buf[i] = ',';
	}

	len--;
	while (buf[len] == ',')
		buf[len--] = 0;

	return buf;
}

static struct w_arg *
add_workload_arg(struct w_arg *w_args, unsigned int nr_args, char *w_arg,
		 int prio, bool sseu)
{
	w_args = realloc(w_args, sizeof(*w_args) * nr_args);
	igt_assert(w_args);
	w_args[nr_args - 1] = (struct w_arg) { w_arg, NULL, prio, sseu };

	return w_args;
}

int main(int argc, char **argv)
{
	unsigned int repeat = 1;
	unsigned int clients = 1;
	unsigned int flags = 0;
	struct timespec t_start, t_end;
	struct workload **w, **wrk = NULL;
	struct workload *app_w = NULL;
	unsigned int nr_w_args = 0;
	int master_workload = -1;
	char *append_workload_arg = NULL;
	struct w_arg *w_args = NULL;
	unsigned int tolerance_pct = 1;
	int exitcode = EXIT_FAILURE;
	int prio = 0;
	double t;
	int i, c;
	char *subopts, *value;
	int raw_number = 0;
	long calib_val;
	int eng;

	/*
	 * Open the device via the low-level API so we can do the GPU quiesce
	 * manually as close as possible in time to the start of the workload.
	 * This minimizes the gap in engine utilization tracking when observed
	 * via external tools like trace.pl.
	 */
	fd = __drm_open_driver_render(DRIVER_INTEL);
	igt_require(fd);

	master_prng = time(NULL);

	while ((c = getopt(argc, argv,
			   "ThqvsSdc:n:r:w:W:a:t:p:I:")) != -1) {
		switch (c) {
		case 'W':
			if (master_workload >= 0) {
				wsim_err("Only one master workload can be given!\n");
				goto err;
			}
			master_workload = nr_w_args;
			/* Fall through */
		case 'w':
			w_args = add_workload_arg(w_args, ++nr_w_args, optarg,
						  prio, flags & SSEU);
			break;
		case 'p':
			prio = atoi(optarg);
			break;
		case 'a':
			if (append_workload_arg) {
				wsim_err("Only one append workload can be given!\n");
				goto err;
			}
			append_workload_arg = optarg;
			break;
		case 'c':
			clients = strtol(optarg, NULL, 0);
			break;
		case 't':
			tolerance_pct = strtol(optarg, NULL, 0);
			break;
		case 'T':
			sequential = false;
			break;

		case 'n':
			subopts = optarg;
			while (*subopts != '\0') {
				eng = getsubopt(&subopts, (char **)ring_str_map, &value);
				if (!value) {
					/* only engine name was given */
					wsim_err("Missing calibration value for '%s'!\n",
						ring_str_map[eng]);
					goto err;
				}

				calib_val = atol(value);

				if (eng >= 0 && eng < NUM_ENGINES) {
				/* engine name with some value were given */

					if (eng == DEFAULT || eng == VCS) {
						wsim_err("'%s' not allowed in engine calibrations!\n",
							ring_str_map[eng]);
						goto err;
					} else if (calib_val <= 0) {
						wsim_err("Invalid calibration for engine '%s' - value "
						"is either non-positive or is not a number!\n",
							ring_str_map[eng]);
						goto err;
					} else if (engine_calib_map[eng]) {
						wsim_err("Invalid repeated calibration of '%s'!\n",
							ring_str_map[eng]);
						goto err;
					} else {
						engine_calib_map[eng] = calib_val;
						if (eng == RCS)
							engine_calib_map[DEFAULT] = calib_val;
						else if (eng == VCS1 || eng == VCS2)
							engine_calib_map[VCS] = calib_val;
						has_nop_calibration = true;
					}
				} else {
					/* raw number was given */

					if (!calib_val) {
						wsim_err("Invalid engine or zero calibration!\n");
						goto err;
					} else if (calib_val < 0) {
						wsim_err("Invalid negative calibration!\n");
						goto err;
					} else if (raw_number) {
						wsim_err("Default engine calibration provided more than once!\n");
						goto err;
					} else {
						raw_number = calib_val;
						apply_unset_calibrations(raw_number);
						has_nop_calibration = true;
					}
				}
			}
			break;
		case 'r':
			repeat = strtol(optarg, NULL, 0);
			break;
		case 'q':
			verbose = 0;
			break;
		case 'v':
			verbose++;
			break;
		case 'S':
			flags |= SYNCEDCLIENTS;
			break;
		case 's':
			flags ^= SSEU;
			break;
		case 'd':
			flags |= DEPSYNC;
			break;
		case 'I':
			master_prng = strtol(optarg, NULL, 0);
			break;
		case 'h':
			print_help();
			goto out;
		default:
			goto err;
		}
	}

	if (!has_nop_calibration) {
		if (verbose > 1) {
			printf("Calibrating nop delays with %u%% tolerance...\n",
				tolerance_pct);
		}

		calibrate_engines();

		if (verbose)
			print_engine_calibrations();
		goto out;
	} else {
		bool missing = false;

		for (i = 0; i < NUM_ENGINES; i++) {
			if (i == VCS)
				continue;

			if (!engine_calib_map[i]) {
				wsim_err("Missing calibration for '%s'!\n",
					 ring_str_map[i]);
				missing = true;
			}
		}

		if (missing)
			goto err;
	}

	if (!nr_w_args) {
		wsim_err("No workload descriptor(s)!\n");
		goto err;
	}

	if (nr_w_args > 1 && clients > 1) {
		wsim_err("Cloned clients cannot be combined with multiple workloads!\n");
		goto err;
	}

	if (append_workload_arg) {
		append_workload_arg = load_workload_descriptor(append_workload_arg);
		if (!append_workload_arg) {
			wsim_err("Failed to load append workload descriptor!\n");
			goto err;
		}
	}

	if (append_workload_arg) {
		struct w_arg arg = { NULL, append_workload_arg, 0 };
		app_w = parse_workload(&arg, flags, NULL);
		if (!app_w) {
			wsim_err("Failed to parse append workload!\n");
			goto err;
		}
	}

	wrk = calloc(nr_w_args, sizeof(*wrk));
	igt_assert(wrk);

	for (i = 0; i < nr_w_args; i++) {
		w_args[i].desc = load_workload_descriptor(w_args[i].filename);

		if (!w_args[i].desc) {
			wsim_err("Failed to load workload descriptor %u!\n", i);
			goto err;
		}

		wrk[i] = parse_workload(&w_args[i], flags, app_w);
		if (!wrk[i]) {
			wsim_err("Failed to parse workload %u!\n", i);
			goto err;
		}
	}

	if (nr_w_args > 1)
		clients = nr_w_args;

	if (verbose > 1) {
		printf("Random seed is %u.\n", master_prng);
		print_engine_calibrations();
		printf("%u client%s.\n", clients, clients > 1 ? "s" : "");
	}

	srand(master_prng);
	master_prng = rand();

	if (master_workload >= 0 && clients == 1)
		master_workload = -1;

	w = calloc(clients, sizeof(struct workload *));
	igt_assert(w);

	for (i = 0; i < clients; i++) {
		w[i] = clone_workload(wrk[nr_w_args > 1 ? i : 0]);

		w[i]->flags = flags;
		w[i]->repeat = repeat;
		w[i]->background = master_workload >= 0 && i != master_workload;
		w[i]->print_stats = verbose > 1 ||
				    (verbose > 0 && master_workload == i);

		if (prepare_workload(i, w[i])) {
			wsim_err("Failed to prepare workload %u!\n", i);
			goto err;
		}
	}

	clock_gettime(CLOCK_MONOTONIC, &t_start);

	for (i = 0; i < clients; i++) {
		int ret;

		ret = pthread_create(&w[i]->thread, NULL, run_workload, w[i]);
		igt_assert_eq(ret, 0);
	}

	if (master_workload >= 0) {
		int ret = pthread_join(w[master_workload]->thread, NULL);

		igt_assert(ret == 0);

		for (i = 0; i < clients; i++)
			w[i]->run = false;
	}

	for (i = 0; i < clients; i++) {
		if (master_workload != i) {
			int ret = pthread_join(w[i]->thread, NULL);
			igt_assert(ret == 0);
		}
	}

	clock_gettime(CLOCK_MONOTONIC, &t_end);

	t = elapsed(&t_start, &t_end);
	if (verbose)
		printf("%.3fs elapsed (%.3f workloads/s)\n",
		       t, clients * repeat / t);

	for (i = 0; i < clients; i++)
		fini_workload(w[i]);
	free(w);
	for (i = 0; i < nr_w_args; i++)
		fini_workload(wrk[i]);
	free(w_args);

out:
	exitcode = EXIT_SUCCESS;
err:
	return exitcode;
}
