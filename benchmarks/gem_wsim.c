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
#include "drm.h"
#include "ioctl_wrappers.h"
#include "drmtest.h"

#include "intel_io.h"
#include "igt_aux.h"
#include "igt_rand.h"
#include "sw_sync.h"

#include "ewma.h"

enum intel_engine_id {
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
	SW_FENCE_SIGNAL
};

struct deps
{
	int nr;
	int *list;
};

struct w_arg {
	char *filename;
	char *desc;
	int prio;
};

struct w_step
{
	/* Workload step metadata */
	enum w_type type;
	unsigned int context;
	unsigned int engine;
	struct duration duration;
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
	};

	/* Implementation details */
	unsigned int idx;
	struct igt_list rq_link;
	unsigned int request;

	struct drm_i915_gem_execbuffer2 eb;
	struct drm_i915_gem_exec_object2 *obj;
	struct drm_i915_gem_relocation_entry reloc[4];
	unsigned long bb_sz;
	uint32_t bb_handle;
	uint32_t *mapped_batch;
	uint32_t *seqno_value;
	uint32_t *seqno_address;
	uint32_t *rt0_value;
	uint32_t *rt0_address;
	uint32_t *rt1_address;
	uint32_t *latch_value;
	uint32_t *latch_address;
	unsigned int mapped_len;
};

DECLARE_EWMA(uint64_t, rt, 4, 2)

struct workload
{
	unsigned int id;

	unsigned int nr_steps;
	struct w_step *steps;
	int prio;

	pthread_t thread;
	bool run;
	bool background;
	const struct workload_balancer *balancer;
	unsigned int repeat;
	unsigned int flags;
	bool print_stats;

	uint32_t prng;

	struct timespec repeat_start;

	unsigned int nr_ctxs;
	uint32_t *ctx_id;

	int sync_timeline;
	uint32_t sync_seqno;

	uint32_t seqno[NUM_ENGINES];
	struct drm_i915_gem_exec_object2 status_object[2];
	uint32_t *status_page;
	uint32_t *status_cs;
	unsigned int vcs_rr;

	unsigned long qd_sum[NUM_ENGINES];
	unsigned long nr_bb[NUM_ENGINES];

	struct igt_list requests[NUM_ENGINES];
	unsigned int nrequest[NUM_ENGINES];

	union {
		struct rtavg {
			struct ewma_rt avg[NUM_ENGINES];
			uint32_t last[NUM_ENGINES];
		} rt;
	};
};

static const unsigned int nop_calibration_us = 1000;
static unsigned long nop_calibration;

static int verbose = 1;
static int fd;

#define SWAPVCS		(1<<0)
#define SEQNO		(1<<1)
#define BALANCE		(1<<2)
#define RT		(1<<3)
#define VCS2REMAP	(1<<4)
#define INITVCSRR	(1<<5)
#define SYNCEDCLIENTS	(1<<6)
#define HEARTBEAT	(1<<7)

#define VCS_SEQNO_IDX(engine) ((engine) * 16)
#define VCS_SEQNO_OFFSET(engine) (VCS_SEQNO_IDX(engine) * sizeof(uint32_t))

#define RCS_TIMESTAMP (0x2000 + 0x358)
#define REG(x) (volatile uint32_t *)((volatile char *)igt_global_mmio + x)

static const char *ring_str_map[NUM_ENGINES] = {
	[RCS] = "RCS",
	[BCS] = "BCS",
	[VCS] = "VCS",
	[VCS1] = "VCS1",
	[VCS2] = "VCS2",
	[VECS] = "VECS",
};

static int
parse_dependencies(unsigned int nr_steps, struct w_step *w, char *_desc)
{
	char *desc = strdup(_desc);
	char *token, *tctx = NULL, *tstart = desc;

	igt_assert(desc);
	igt_assert(!w->data_deps.nr && w->data_deps.nr == w->fence_deps.nr);
	igt_assert(!w->data_deps.list &&
		   w->data_deps.list == w->fence_deps.list);

	while ((token = strtok_r(tstart, "/", &tctx)) != NULL) {
		char *str = token;
		struct deps *deps;
		int dep;

		tstart = NULL;

		if (strlen(token) > 1 && token[0] == 'f') {
			deps = &w->fence_deps;
			str++;
		} else {
			deps = &w->data_deps;
		}

		dep = atoi(str);
		if (dep > 0 || ((int)nr_steps + dep) < 0) {
			if (deps->list)
				free(deps->list);
			return -1;
		}

		if (dep < 0) {
			deps->nr++;
			/* Multiple fences not yet supported. */
			igt_assert(deps->nr == 1 || deps != &w->fence_deps);
			deps->list = realloc(deps->list,
					     sizeof(*deps->list) * deps->nr);
			igt_assert(deps->list);
			deps->list[deps->nr - 1] = dep;
		}
	}

	free(desc);

	return 0;
}

static struct workload *
parse_workload(struct w_arg *arg, unsigned int flags, struct workload *app_w)
{
	struct workload *wrk;
	unsigned int nr_steps = 0;
	char *desc = strdup(arg->desc);
	char *_token, *token, *tctx = NULL, *tstart = desc;
	char *field, *fctx = NULL, *fstart;
	struct w_step step, *steps = NULL;
	bool bcs_used = false;
	unsigned int valid;
	int i, j, tmp;

	igt_assert(desc);

	while ((_token = strtok_r(tstart, ",", &tctx)) != NULL) {
		tstart = NULL;
		token = strdup(_token);
		igt_assert(token);
		fstart = token;
		valid = 0;
		memset(&step, 0, sizeof(step));

		if ((field = strtok_r(fstart, ".", &fctx)) != NULL) {
			fstart = NULL;

			if (!strcasecmp(field, "d")) {
				if ((field = strtok_r(fstart, ".", &fctx)) !=
				    NULL) {
					tmp = atoi(field);
					if (tmp <= 0) {
						if (verbose)
							fprintf(stderr,
								"Invalid delay at step %u!\n",
								nr_steps);
						return NULL;
					}

					step.type = DELAY;
					step.delay = tmp;
					goto add_step;
				}
			} else if (!strcasecmp(field, "p")) {
				if ((field = strtok_r(fstart, ".", &fctx)) !=
				    NULL) {
					tmp = atoi(field);
					if (tmp <= 0) {
						if (verbose)
							fprintf(stderr,
								"Invalid period at step %u!\n",
								nr_steps);
						return NULL;
					}

					step.type = PERIOD;
					step.period = tmp;
					goto add_step;
				}
			} else if (!strcasecmp(field, "s")) {
				if ((field = strtok_r(fstart, ".", &fctx)) !=
				    NULL) {
					tmp = atoi(field);
					if (tmp >= 0 ||
					    ((int)nr_steps + tmp) < 0) {
						if (verbose)
							fprintf(stderr,
								"Invalid sync target at step %u!\n",
								nr_steps);
						return NULL;
					}

					step.type = SYNC;
					step.target = tmp;
					goto add_step;
				}
			} else if (!strcasecmp(field, "t")) {
				if ((field = strtok_r(fstart, ".", &fctx)) !=
				    NULL) {
					tmp = atoi(field);
					if (tmp < 0) {
						if (verbose)
							fprintf(stderr,
								"Invalid throttle at step %u!\n",
								nr_steps);
						return NULL;
					}

					step.type = THROTTLE;
					step.throttle = tmp;
					goto add_step;
				}
			} else if (!strcasecmp(field, "q")) {
				if ((field = strtok_r(fstart, ".", &fctx)) !=
				    NULL) {
					tmp = atoi(field);
					if (tmp < 0) {
						if (verbose)
							fprintf(stderr,
								"Invalid qd throttle at step %u!\n",
								nr_steps);
						return NULL;
					}

					step.type = QD_THROTTLE;
					step.throttle = tmp;
					goto add_step;
				}
			} else if (!strcasecmp(field, "a")) {
				if ((field = strtok_r(fstart, ".", &fctx)) !=
				    NULL) {
					tmp = atoi(field);
					if (tmp >= 0) {
						if (verbose)
							fprintf(stderr,
								"Invalid sw fence signal at step %u!\n",
								nr_steps);
						return NULL;
					}

					step.type = SW_FENCE_SIGNAL;
					step.target = tmp;
					goto add_step;
				}
			} else if (!strcasecmp(field, "f")) {
				step.type = SW_FENCE;
				goto add_step;
			}

			tmp = atoi(field);
			if (tmp < 0) {
				if (verbose)
					fprintf(stderr,
						"Invalid ctx id at step %u!\n",
						nr_steps);
				return NULL;
			}
			step.context = tmp;

			valid++;
		}

		if ((field = strtok_r(fstart, ".", &fctx)) != NULL) {
			unsigned int old_valid = valid;

			fstart = NULL;

			for (i = 0; i < ARRAY_SIZE(ring_str_map); i++) {
				if (!strcasecmp(field, ring_str_map[i])) {
					step.engine = i;
					if (step.engine == BCS)
						bcs_used = true;
					valid++;
					break;
				}
			}

			if (old_valid == valid) {
				if (verbose)
					fprintf(stderr,
						"Invalid engine id at step %u!\n",
						nr_steps);
				return NULL;
			}
		}

		if ((field = strtok_r(fstart, ".", &fctx)) != NULL) {
			char *sep = NULL;
			long int tmpl;

			fstart = NULL;

			tmpl = strtol(field, &sep, 10);
			if (tmpl == 0 || tmpl == LONG_MIN || tmpl == LONG_MAX) {
				if (verbose)
					fprintf(stderr,
						"Invalid duration at step %u!\n",
						nr_steps);
				return NULL;
			}
			step.duration.min = tmpl;

			if (sep && *sep == '-') {
				tmpl = strtol(sep + 1, NULL, 10);
				if (tmpl == 0 ||
				    tmpl == LONG_MIN || tmpl == LONG_MAX) {
					if (verbose)
						fprintf(stderr,
							"Invalid duration range at step %u!\n",
							nr_steps);
					return NULL;
				}
				step.duration.max = tmpl;
			} else {
				step.duration.max = step.duration.min;
			}

			valid++;
		}

		if ((field = strtok_r(fstart, ".", &fctx)) != NULL) {
			fstart = NULL;

			tmp = parse_dependencies(nr_steps, &step, field);
			if (tmp < 0) {
				if (verbose)
					fprintf(stderr,
						"Invalid dependency at step %u!\n",
						nr_steps);
				return NULL;
			}

			valid++;
		}

		if ((field = strtok_r(fstart, ".", &fctx)) != NULL) {
			fstart = NULL;

			if (strlen(field) != 1 ||
			    (field[0] != '0' && field[0] != '1')) {
				if (verbose)
					fprintf(stderr,
						"Invalid wait boolean at step %u!\n",
						nr_steps);
				return NULL;
			}
			step.sync = field[0] - '0';

			valid++;
		}

		if (valid != 5) {
			if (verbose)
				fprintf(stderr, "Invalid record at step %u!\n",
					nr_steps);
			return NULL;
		}

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

	free(desc);

	/*
	 * Tag all steps which need to emit a sync fence if another step is
	 * referencing them as a sync fence dependency.
	 */
	for (i = 0; i < nr_steps; i++) {
		for (j = 0; j < steps[i].fence_deps.nr; j++) {
			tmp = steps[i].idx + steps[i].fence_deps.list[j];
			if (tmp < 0 || tmp >= i ||
			    (steps[tmp].type != BATCH &&
			     steps[tmp].type != SW_FENCE)) {
				if (verbose)
					fprintf(stderr,
						"Invalid dependency target %u!\n",
						i);
				return NULL;
			}
			steps[tmp].emit_fence = -1;
		}
	}

	/* Validate SW_FENCE_SIGNAL targets. */
	for (i = 0; i < nr_steps; i++) {
		if (steps[i].type == SW_FENCE_SIGNAL) {
			tmp = steps[i].idx + steps[i].target;
			if (tmp < 0 || tmp >= i ||
			    steps[tmp].type != SW_FENCE) {
				if (verbose)
					fprintf(stderr,
						"Invalid sw fence target %u!\n",
						i);
				return NULL;
			}
		}
	}

	if (bcs_used && verbose)
		printf("BCS usage in workload with VCS2 remapping enabled!\n");

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
	wrk->nr_steps = _wrk->nr_steps;
	wrk->steps = calloc(wrk->nr_steps, sizeof(struct w_step));
	igt_assert(wrk->steps);

	memcpy(wrk->steps, _wrk->steps, sizeof(struct w_step) * wrk->nr_steps);

	/* Check if we need a sw sync timeline. */
	for (i = 0; i < wrk->nr_steps; i++) {
		if (wrk->steps[i].type == SW_FENCE) {
			wrk->sync_timeline = sw_sync_timeline_create();
			igt_assert(wrk->sync_timeline >= 0);
			break;
		}
	}

	for (i = 0; i < NUM_ENGINES; i++)
		igt_list_init(&wrk->requests[i]);

	return wrk;
}

#define rounddown(x, y) (x - (x%y))
#ifndef PAGE_SIZE
#define PAGE_SIZE (4096)
#endif

static unsigned int get_duration(struct w_step *w)
{
	struct duration *dur = &w->duration;

	if (dur->min == dur->max)
		return dur->min;
	else
		return dur->min + hars_petruska_f54_1_random_unsafe() %
		       (dur->max + 1 - dur->min);
}

static unsigned long get_bb_sz(unsigned int duration)
{
	return ALIGN(duration * nop_calibration * sizeof(uint32_t) /
		     nop_calibration_us, sizeof(uint32_t));
}

static void
terminate_bb(struct w_step *w, unsigned int flags)
{
	const uint32_t bbe = 0xa << 23;
	unsigned long mmap_start, mmap_len;
	unsigned long batch_start = w->bb_sz;
	uint32_t *ptr, *cs;

	igt_assert(((flags & RT) && (flags & SEQNO)) || !(flags & RT));

	batch_start -= sizeof(uint32_t); /* bbend */
	if (flags & SEQNO)
		batch_start -= 4 * sizeof(uint32_t);
	if (flags & RT)
		batch_start -= 12 * sizeof(uint32_t);

	mmap_start = rounddown(batch_start, PAGE_SIZE);
	mmap_len = w->bb_sz - mmap_start;

	gem_set_domain(fd, w->bb_handle,
		       I915_GEM_DOMAIN_WC, I915_GEM_DOMAIN_WC);

	ptr = gem_mmap__wc(fd, w->bb_handle, mmap_start, mmap_len, PROT_WRITE);
	cs = (uint32_t *)((char *)ptr + batch_start - mmap_start);

	if (flags & SEQNO) {
		w->reloc[0].offset = batch_start + sizeof(uint32_t);
		batch_start += 4 * sizeof(uint32_t);

		*cs++ = MI_STORE_DWORD_IMM;
		w->seqno_address = cs;
		*cs++ = 0;
		*cs++ = 0;
		w->seqno_value = cs;
		*cs++ = 0;
	}

	if (flags & RT) {
		w->reloc[1].offset = batch_start + sizeof(uint32_t);
		batch_start += 4 * sizeof(uint32_t);

		*cs++ = MI_STORE_DWORD_IMM;
		w->rt0_address = cs;
		*cs++ = 0;
		*cs++ = 0;
		w->rt0_value = cs;
		*cs++ = 0;

		w->reloc[2].offset = batch_start + 2 * sizeof(uint32_t);
		batch_start += 4 * sizeof(uint32_t);

		*cs++ = 0x24 << 23 | 2; /* MI_STORE_REG_MEM */
		*cs++ = RCS_TIMESTAMP;
		w->rt1_address = cs;
		*cs++ = 0;
		*cs++ = 0;

		w->reloc[3].offset = batch_start + sizeof(uint32_t);
		batch_start += 4 * sizeof(uint32_t);

		*cs++ = MI_STORE_DWORD_IMM;
		w->latch_address = cs;
		*cs++ = 0;
		*cs++ = 0;
		w->latch_value = cs;
		*cs++ = 0;
	}

	*cs = bbe;

	w->mapped_batch = ptr;
	w->mapped_len = mmap_len;
}

static const unsigned int eb_engine_map[NUM_ENGINES] = {
	[RCS] = I915_EXEC_RENDER,
	[BCS] = I915_EXEC_BLT,
	[VCS] = I915_EXEC_BSD,
	[VCS1] = I915_EXEC_BSD | I915_EXEC_BSD_RING1,
	[VCS2] = I915_EXEC_BSD | I915_EXEC_BSD_RING2,
	[VECS] = I915_EXEC_VEBOX
};

static void
eb_update_flags(struct w_step *w, enum intel_engine_id engine,
		unsigned int flags)
{
	if (engine == VCS2 && (flags & VCS2REMAP))
		engine = BCS;

	w->eb.flags = eb_engine_map[engine];

	w->eb.flags |= I915_EXEC_HANDLE_LUT;
	w->eb.flags |= I915_EXEC_NO_RELOC;

	igt_assert(w->emit_fence <= 0);
	if (w->emit_fence)
		w->eb.flags |= I915_EXEC_FENCE_OUT;
}

static void
alloc_step_batch(struct workload *wrk, struct w_step *w, unsigned int flags)
{
	enum intel_engine_id engine = w->engine;
	unsigned int j = 0;
	unsigned int nr_obj = 3 + w->data_deps.nr;
	unsigned int i;

	w->obj = calloc(nr_obj, sizeof(*w->obj));
	igt_assert(w->obj);

	w->obj[j].handle = gem_create(fd, 4096);
	w->obj[j].flags = EXEC_OBJECT_WRITE;
	j++;
	igt_assert(j < nr_obj);

	if (flags & SEQNO) {
		w->obj[j++] = wrk->status_object[0];
		igt_assert(j < nr_obj);
	}

	for (i = 0; i < w->data_deps.nr; i++) {
		igt_assert(w->data_deps.list[i] <= 0);
		if (w->data_deps.list[i]) {
			int dep_idx = w->idx + w->data_deps.list[i];

			igt_assert(dep_idx >= 0 && dep_idx < w->idx);
			igt_assert(wrk->steps[dep_idx].type == BATCH);

			w->obj[j].handle = wrk->steps[dep_idx].obj[0].handle;
			j++;
			igt_assert(j < nr_obj);
		}
	}

	w->bb_sz = get_bb_sz(w->duration.max);
	w->bb_handle = w->obj[j].handle = gem_create(fd, w->bb_sz);
	terminate_bb(w, flags);

	if (flags & SEQNO) {
		w->obj[j].relocs_ptr = to_user_pointer(&w->reloc);
		if (flags & RT)
			w->obj[j].relocation_count = 4;
		else
			w->obj[j].relocation_count = 1;
		for (i = 0; i < w->obj[j].relocation_count; i++)
			w->reloc[i].target_handle = 1;
	}

	w->eb.buffers_ptr = to_user_pointer(w->obj);
	w->eb.buffer_count = j + 1;
	w->eb.rsvd1 = wrk->ctx_id[w->context];

	if (flags & SWAPVCS && engine == VCS1)
		engine = VCS2;
	else if (flags & SWAPVCS && engine == VCS2)
		engine = VCS1;
	eb_update_flags(w, engine, flags);
#ifdef DEBUG
	printf("%u: %u:|", w->idx, w->eb.buffer_count);
	for (i = 0; i <= j; i++)
		printf("%x|", w->obj[i].handle);
	printf(" %10lu flags=%llx bb=%x[%u] ctx[%u]=%u\n",
		w->bb_sz, w->eb.flags, w->bb_handle, j, w->context,
		wrk->ctx_id[w->context]);
#endif
}

static void
prepare_workload(unsigned int id, struct workload *wrk, unsigned int flags)
{
	int max_ctx = -1;
	struct w_step *w;
	int i;

	wrk->id = id;
	wrk->prng = rand();

	if (flags & INITVCSRR)
		wrk->vcs_rr = id & 1;

	if (flags & SEQNO) {
		uint32_t handle = gem_create(fd, 4096);

		gem_set_caching(fd, handle, I915_CACHING_CACHED);
		wrk->status_object[0].handle = handle;
		wrk->status_page = gem_mmap__cpu(fd, handle, 0, 4096,
						 PROT_READ);

		handle = gem_create(fd, 4096);
		wrk->status_object[1].handle = handle;
		wrk->status_cs = gem_mmap__wc(fd, handle,
					      0, 4096, PROT_WRITE);
	}

	for (i = 0, w = wrk->steps; i < wrk->nr_steps; i++, w++) {
		if ((int)w->context > max_ctx) {
			int delta = w->context + 1 - wrk->nr_ctxs;

			wrk->nr_ctxs += delta;
			wrk->ctx_id = realloc(wrk->ctx_id,
					      wrk->nr_ctxs * sizeof(uint32_t));
			memset(&wrk->ctx_id[wrk->nr_ctxs - delta], 0,
			       delta * sizeof(uint32_t));

			max_ctx = w->context;
		}

		if (!wrk->ctx_id[w->context]) {
			struct drm_i915_gem_context_create arg = {};

			drmIoctl(fd, DRM_IOCTL_I915_GEM_CONTEXT_CREATE, &arg);
			igt_assert(arg.ctx_id);

			wrk->ctx_id[w->context] = arg.ctx_id;

			if (wrk->prio) {
				struct local_i915_gem_context_param param = {
					.context = arg.ctx_id,
					.param = 0x6,
					.value = wrk->prio,
				};
				gem_context_set_param(fd, &param);
			}
		}
	}

	for (i = 0, w = wrk->steps; i < wrk->nr_steps; i++, w++) {
		unsigned int _flags = flags;
		enum intel_engine_id engine = w->engine;

		if (w->type != BATCH)
			continue;

		if (engine == VCS)
			_flags &= ~SWAPVCS;

		alloc_step_batch(wrk, w, _flags);
	}
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

static enum intel_engine_id get_vcs_engine(unsigned int n)
{
	const enum intel_engine_id vcs_engines[2] = { VCS1, VCS2 };

	igt_assert(n < ARRAY_SIZE(vcs_engines));

	return vcs_engines[n];
}

static uint32_t new_seqno(struct workload *wrk, enum intel_engine_id engine)
{
	return ++wrk->seqno[engine];
}

static uint32_t
current_seqno(struct workload *wrk, enum intel_engine_id engine)
{
	return wrk->seqno[engine];
}

struct workload_balancer {
	unsigned int id;
	const char *name;
	const char *desc;
	unsigned int flags;
	unsigned int min_gen;

	unsigned int (*get_qd)(const struct workload_balancer *balancer,
			       struct workload *wrk,
			       enum intel_engine_id engine);
	enum intel_engine_id (*balance)(const struct workload_balancer *balancer,
					struct workload *wrk, struct w_step *w);
};

static enum intel_engine_id
rr_balance(const struct workload_balancer *balancer,
	   struct workload *wrk, struct w_step *w)
{
	unsigned int engine;

	engine = get_vcs_engine(wrk->vcs_rr);
	wrk->vcs_rr ^= 1;

	return engine;
}

static enum intel_engine_id
rand_balance(const struct workload_balancer *balancer,
	     struct workload *wrk, struct w_step *w)
{
	return get_vcs_engine(hars_petruska_f54_1_random(&wrk->prng) & 1);
}

static unsigned int
get_qd_depth(const struct workload_balancer *balancer,
	     struct workload *wrk, enum intel_engine_id engine)
{
	return current_seqno(wrk, engine) -
	       wrk->status_page[VCS_SEQNO_IDX(engine)];
}

static enum intel_engine_id
__qd_select_engine(struct workload *wrk, const unsigned long *qd, bool random)
{
	unsigned int n;

	if (qd[VCS1] < qd[VCS2])
		n = 0;
	else if (qd[VCS1] > qd[VCS2])
		n = 1;
	else if (random)
		n = hars_petruska_f54_1_random(&wrk->prng) & 1;
	else
		n = wrk->vcs_rr;
	wrk->vcs_rr = n ^ 1;

	return get_vcs_engine(n);
}

static enum intel_engine_id
__qd_balance(const struct workload_balancer *balancer,
	     struct workload *wrk, struct w_step *w, bool random)
{
	enum intel_engine_id engine;
	unsigned long qd[NUM_ENGINES];

	igt_assert(w->engine == VCS);

	qd[VCS1] = balancer->get_qd(balancer, wrk, VCS1);
	wrk->qd_sum[VCS1] += qd[VCS1];

	qd[VCS2] = balancer->get_qd(balancer, wrk, VCS2);
	wrk->qd_sum[VCS2] += qd[VCS2];

	engine = __qd_select_engine(wrk, qd, random);

#ifdef DEBUG
	printf("qd_balance: 1:%ld 2:%ld rr:%u = %u\t(%lu - %u) (%lu - %u)\n",
	       qd[VCS1], qd[VCS2], wrk->vcs_rr, engine,
	       current_seqno(wrk, VCS1), wrk->status_page[VCS_SEQNO_IDX(VCS1)],
	       current_seqno(wrk, VCS2), wrk->status_page[VCS_SEQNO_IDX(VCS2)]);
#endif
	return engine;
}

static enum intel_engine_id
qd_balance(const struct workload_balancer *balancer,
	     struct workload *wrk, struct w_step *w)
{
	return __qd_balance(balancer, wrk, w, false);
}

static enum intel_engine_id
qdr_balance(const struct workload_balancer *balancer,
	     struct workload *wrk, struct w_step *w)
{
	return __qd_balance(balancer, wrk, w, true);
}

static enum intel_engine_id
qdavg_balance(const struct workload_balancer *balancer,
	     struct workload *wrk, struct w_step *w)
{
	unsigned long qd[NUM_ENGINES];
	unsigned int engine;

	igt_assert(w->engine == VCS);

	for (engine = VCS1; engine <= VCS2; engine++) {
		qd[engine] = balancer->get_qd(balancer, wrk, engine);
		wrk->qd_sum[engine] += qd[engine];

		ewma_rt_add(&wrk->rt.avg[engine], qd[engine]);
		qd[engine] = ewma_rt_read(&wrk->rt.avg[engine]);
	}

	return __qd_select_engine(wrk, qd, false);
}

static enum intel_engine_id
__rt_select_engine(struct workload *wrk, unsigned long *qd, bool random)
{
	qd[VCS1] >>= 10;
	qd[VCS2] >>= 10;

	return __qd_select_engine(wrk, qd, random);
}

struct rt_depth {
	uint32_t seqno;
	uint32_t submitted;
	uint32_t completed;
};

#define READ_ONCE(x) (*(volatile typeof(x) *)(&(x)))

static void get_rt_depth(struct workload *wrk,
			 unsigned int engine,
			 struct rt_depth *rt)
{
	const unsigned int idx = VCS_SEQNO_IDX(engine);
	uint32_t latch;

	do {
		latch = READ_ONCE(wrk->status_page[idx + 3]);

		rt->submitted = READ_ONCE(wrk->status_page[idx + 1]);
		rt->completed = READ_ONCE(wrk->status_page[idx + 2]);
		rt->seqno = READ_ONCE(wrk->status_page[idx]);
	} while (latch != rt->seqno);
}

static enum intel_engine_id
__rt_balance(const struct workload_balancer *balancer,
	     struct workload *wrk, struct w_step *w, bool random)
{
	unsigned long qd[NUM_ENGINES];
	unsigned int engine;

	igt_assert(w->engine == VCS);

	/* Estimate the "speed" of the most recent batch
	 *    (finish time - submit time)
	 * and use that as an approximate for the total remaining time for
	 * all batches on that engine, plus the time we expect this batch to
	 * take. We try to keep the total balanced between the engines.
	 */
	for (engine = VCS1; engine <= VCS2; engine++) {
		struct rt_depth rt;

		get_rt_depth(wrk, engine, &rt);
		qd[engine] = current_seqno(wrk, engine) - rt.seqno;
		wrk->qd_sum[engine] += qd[engine];
		qd[engine] = (qd[engine] + 1) * (rt.completed - rt.submitted);
#ifdef DEBUG
		printf("rt[0] = %d (%d - %d) x %d (%d - %d) = %ld\n",
		       current_seqno(wrk, engine) - rt.seqno,
		       current_seqno(wrk, engine), rt.seqno,
		       rt.completed - rt.submitted,
		       rt.completed, rt.submitted,
		       qd[engine]);
#endif
	}

	return __rt_select_engine(wrk, qd, random);
}

static enum intel_engine_id
rt_balance(const struct workload_balancer *balancer,
	   struct workload *wrk, struct w_step *w)
{

	return __rt_balance(balancer, wrk, w, false);
}

static enum intel_engine_id
rtr_balance(const struct workload_balancer *balancer,
	   struct workload *wrk, struct w_step *w)
{
	return __rt_balance(balancer, wrk, w, true);
}

static enum intel_engine_id
rtavg_balance(const struct workload_balancer *balancer,
	   struct workload *wrk, struct w_step *w)
{
	unsigned long qd[NUM_ENGINES];
	unsigned int engine;

	igt_assert(w->engine == VCS);

	/* Estimate the average "speed" of the most recent batches
	 *    (finish time - submit time)
	 * and use that as an approximate for the total remaining time for
	 * all batches on that engine plus the time we expect to execute in.
	 * We try to keep the total remaining balanced between the engines.
	 */
	for (engine = VCS1; engine <= VCS2; engine++) {
		struct rt_depth rt;

		get_rt_depth(wrk, engine, &rt);
		if (rt.seqno != wrk->rt.last[engine]) {
			igt_assert((long)(rt.completed - rt.submitted) > 0);
			ewma_rt_add(&wrk->rt.avg[engine],
				    rt.completed - rt.submitted);
			wrk->rt.last[engine] = rt.seqno;
		}
		qd[engine] = current_seqno(wrk, engine) - rt.seqno;
		wrk->qd_sum[engine] += qd[engine];
		qd[engine] =
			(qd[engine] + 1) * ewma_rt_read(&wrk->rt.avg[engine]);

#ifdef DEBUG
		printf("rtavg[%d] = %d (%d - %d) x %ld (%d) = %ld\n",
		       engine,
		       current_seqno(wrk, engine) - rt.seqno,
		       current_seqno(wrk, engine), rt.seqno,
		       ewma_rt_read(&wrk->rt.avg[engine]),
		       rt.completed - rt.submitted,
		       qd[engine]);
#endif
	}

	return __rt_select_engine(wrk, qd, false);
}

static const struct workload_balancer all_balancers[] = {
	{
		.id = 0,
		.name = "rr",
		.desc = "Simple round-robin.",
		.balance = rr_balance,
	},
	{
		.id = 6,
		.name = "rand",
		.desc = "Random selection.",
		.balance = rand_balance,
	},
	{
		.id = 1,
		.name = "qd",
		.desc = "Queue depth estimation with round-robin on equal depth.",
		.flags = SEQNO,
		.min_gen = 8,
		.get_qd = get_qd_depth,
		.balance = qd_balance,
	},
	{
		.id = 5,
		.name = "qdr",
		.desc = "Queue depth estimation with random selection on equal depth.",
		.flags = SEQNO,
		.min_gen = 8,
		.get_qd = get_qd_depth,
		.balance = qdr_balance,
	},
	{
		.id = 7,
		.name = "qdavg",
		.desc = "Like qd, but using an average queue depth estimator.",
		.flags = SEQNO,
		.min_gen = 8,
		.get_qd = get_qd_depth,
		.balance = qdavg_balance,
	},
	{
		.id = 2,
		.name = "rt",
		.desc = "Queue depth plus last runtime estimation.",
		.flags = SEQNO | RT,
		.min_gen = 8,
		.get_qd = get_qd_depth,
		.balance = rt_balance,
	},
	{
		.id = 3,
		.name = "rtr",
		.desc = "Like rt but with random engine selection on equal depth.",
		.flags = SEQNO | RT,
		.min_gen = 8,
		.get_qd = get_qd_depth,
		.balance = rtr_balance,
	},
	{
		.id = 4,
		.name = "rtavg",
		.desc = "Improved version rt tracking average execution speed per engine.",
		.flags = SEQNO | RT,
		.min_gen = 8,
		.get_qd = get_qd_depth,
		.balance = rtavg_balance,
	},
};

static void
update_bb_seqno(struct w_step *w, enum intel_engine_id engine, uint32_t seqno)
{
	gem_set_domain(fd, w->bb_handle,
		       I915_GEM_DOMAIN_WC, I915_GEM_DOMAIN_WC);

	w->reloc[0].delta = VCS_SEQNO_OFFSET(engine);

	*w->seqno_value = seqno;
	*w->seqno_address = w->reloc[0].presumed_offset + w->reloc[0].delta;

	/* If not using NO_RELOC, force the relocations */
	if (!(w->eb.flags & I915_EXEC_NO_RELOC))
		w->reloc[0].presumed_offset = -1;
}

static void
update_bb_rt(struct w_step *w, enum intel_engine_id engine, uint32_t seqno)
{
	gem_set_domain(fd, w->bb_handle,
		       I915_GEM_DOMAIN_WC, I915_GEM_DOMAIN_WC);

	w->reloc[1].delta = VCS_SEQNO_OFFSET(engine) + sizeof(uint32_t);
	w->reloc[2].delta = VCS_SEQNO_OFFSET(engine) + 2 * sizeof(uint32_t);
	w->reloc[3].delta = VCS_SEQNO_OFFSET(engine) + 3 * sizeof(uint32_t);

	*w->latch_value = seqno;
	*w->latch_address = w->reloc[3].presumed_offset + w->reloc[3].delta;

	*w->rt0_value = *REG(RCS_TIMESTAMP);
	*w->rt0_address = w->reloc[1].presumed_offset + w->reloc[1].delta;
	*w->rt1_address = w->reloc[2].presumed_offset + w->reloc[2].delta;

	/* If not using NO_RELOC, force the relocations */
	if (!(w->eb.flags & I915_EXEC_NO_RELOC)) {
		w->reloc[1].presumed_offset = -1;
		w->reloc[2].presumed_offset = -1;
		w->reloc[3].presumed_offset = -1;
	}
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

#define INIT_CLOCKS 0x1
#define INIT_ALL (INIT_CLOCKS)
static void init_status_page(struct workload *wrk, unsigned int flags)
{
	struct drm_i915_gem_relocation_entry reloc[4] = {};
	struct drm_i915_gem_execbuffer2 eb = {
		.buffer_count = ARRAY_SIZE(wrk->status_object),
		.buffers_ptr = to_user_pointer(wrk->status_object)
	};
	uint32_t *base = wrk->status_cs;

	/* Want to make sure that the balancer has a reasonable view of
	 * the background busyness of each engine. To do that we occasionally
	 * send a dummy batch down the pipeline.
	 */

	if (!wrk->status_cs)
		return;

	gem_set_domain(fd, wrk->status_object[1].handle,
		       I915_GEM_DOMAIN_WC, I915_GEM_DOMAIN_WC);

	wrk->status_object[1].relocs_ptr = to_user_pointer(reloc);
	wrk->status_object[1].relocation_count = 2;
	if (flags & INIT_CLOCKS)
		wrk->status_object[1].relocation_count += 2;

	for (int engine = 0; engine < NUM_ENGINES; engine++) {
		struct drm_i915_gem_relocation_entry *r = reloc;
		uint64_t presumed_offset = wrk->status_object[0].offset;
		uint32_t offset = engine * 128;
		uint32_t *cs = base + offset / sizeof(*cs);
		uint64_t addr;

		r->offset = offset + sizeof(uint32_t);
		r->delta = VCS_SEQNO_OFFSET(engine);
		r->presumed_offset = presumed_offset;
		addr = presumed_offset + r->delta;
		r++;
		*cs++ = MI_STORE_DWORD_IMM;
		*cs++ = addr;
		*cs++ = addr >> 32;
		*cs++ = new_seqno(wrk, engine);
		offset += 4 * sizeof(uint32_t);

		/* When we are busy, we can just reuse the last set of timings.
		 * If we have been idle for a while, we want to resample the
		 * latency on each engine (to measure external load).
		 */
		if (flags & INIT_CLOCKS) {
			r->offset = offset + sizeof(uint32_t);
			r->delta = VCS_SEQNO_OFFSET(engine) + sizeof(uint32_t);
			r->presumed_offset = presumed_offset;
			addr = presumed_offset + r->delta;
			r++;
			*cs++ = MI_STORE_DWORD_IMM;
			*cs++ = addr;
			*cs++ = addr >> 32;
			*cs++ = *REG(RCS_TIMESTAMP);
			offset += 4 * sizeof(uint32_t);

			r->offset = offset + 2 * sizeof(uint32_t);
			r->delta = VCS_SEQNO_OFFSET(engine) + 2*sizeof(uint32_t);
			r->presumed_offset = presumed_offset;
			addr = presumed_offset + r->delta;
			r++;
			*cs++ = 0x24 << 23 | 2; /* MI_STORE_REG_MEM */
			*cs++ = RCS_TIMESTAMP;
			*cs++ = addr;
			*cs++ = addr >> 32;
			offset += 4 * sizeof(uint32_t);
		}

		r->offset = offset + sizeof(uint32_t);
		r->delta = VCS_SEQNO_OFFSET(engine) + 3*sizeof(uint32_t);
		r->presumed_offset = presumed_offset;
		addr = presumed_offset + r->delta;
		r++;
		*cs++ = MI_STORE_DWORD_IMM;
		*cs++ = addr;
		*cs++ = addr >> 32;
		*cs++ = current_seqno(wrk, engine);
		offset += 4 * sizeof(uint32_t);

		*cs++ = MI_BATCH_BUFFER_END;

		eb.flags = eb_engine_map[engine];
		eb.flags |= I915_EXEC_HANDLE_LUT;
		eb.flags |= I915_EXEC_NO_RELOC;

		eb.batch_start_offset = 128 * engine;

		gem_execbuf(fd, &eb);
	}
}

#define LOCAL_IOCTL_I915_GEM_EXECBUFFER2_WR       DRM_IOWR(DRM_COMMAND_BASE + DRM_I915_GEM_EXECBUFFER2, struct drm_i915_gem_execbuffer2)

static int __gem_execbuf_wr(int gemfd, struct drm_i915_gem_execbuffer2 *execbuf)
{
	int err = 0;
	if (igt_ioctl(gemfd, LOCAL_IOCTL_I915_GEM_EXECBUFFER2_WR, execbuf))
		err = -errno;
	errno = 0;
	return err;
}

static void gem_execbuf_wr(int gemfd, struct drm_i915_gem_execbuffer2 *execbuf)
{
	igt_assert_eq(__gem_execbuf_wr(gemfd, execbuf), 0);
}

static void
do_eb(struct workload *wrk, struct w_step *w, enum intel_engine_id engine,
      unsigned int flags)
{
	uint32_t seqno = new_seqno(wrk, engine);
	unsigned int i;

	eb_update_flags(w, engine, flags);

	if (flags & SEQNO)
		update_bb_seqno(w, engine, seqno);
	if (flags & RT)
		update_bb_rt(w, engine, seqno);

	w->eb.batch_start_offset =
		ALIGN(w->bb_sz - get_bb_sz(get_duration(w)),
			2 * sizeof(uint32_t));

	for (i = 0; i < w->fence_deps.nr; i++) {
		int tgt = w->idx + w->fence_deps.list[i];

		/* TODO: fence merging needed to support multiple inputs */
		igt_assert(i == 0);
		igt_assert(tgt >= 0 && tgt < w->idx);
		igt_assert(wrk->steps[tgt].emit_fence > 0);

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

static void *run_workload(void *data)
{
	struct workload *wrk = (struct workload *)data;
	struct timespec t_start, t_end;
	struct w_step *w;
	int throttle = -1;
	int qd_throttle = -1;
	int count;
	int i;

	clock_gettime(CLOCK_MONOTONIC, &t_start);

	hars_petruska_f54_1_random_seed((wrk->flags & SYNCEDCLIENTS) ?
					0 : wrk->id);

	init_status_page(wrk, INIT_ALL);
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

				clock_gettime(CLOCK_MONOTONIC, &now);
				do_sleep = w->period -
					   elapsed_us(&wrk->repeat_start, &now);
				if (do_sleep < 0) {
					if (verbose > 1)
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
			}

			if (do_sleep || w->type == PERIOD) {
				usleep(do_sleep);
				continue;
			}

			igt_assert(w->type == BATCH);

			wrk->nr_bb[engine]++;
			if (engine == VCS && wrk->balancer) {
				engine = wrk->balancer->balance(wrk->balancer,
								wrk, w);
				wrk->nr_bb[engine]++;
			}

			if (throttle > 0)
				w_sync_to(wrk, w, i - throttle);

			do_eb(wrk, w, engine, wrk->flags);

			if (w->request != -1) {
				igt_list_del(&w->rq_link);
				wrk->nrequest[w->request]--;
			}
			w->request = engine;
			igt_list_add_tail(&w->rq_link, &wrk->requests[engine]);
			wrk->nrequest[engine]++;

			if (!wrk->run)
				break;

			if (w->sync) {
				gem_sync(fd, w->obj[0].handle);
				if (wrk->flags & HEARTBEAT)
					init_status_page(wrk, 0);
			}

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
		if (wrk->balancer)
			printf(" %lu (%lu + %lu) total VCS batches.",
			       wrk->nr_bb[VCS], wrk->nr_bb[VCS1], wrk->nr_bb[VCS2]);
		if (wrk->balancer && wrk->balancer->get_qd)
			printf(" Average queue depths %.3f, %.3f.",
			       (double)wrk->qd_sum[VCS1] / wrk->nr_bb[VCS],
			       (double)wrk->qd_sum[VCS2] / wrk->nr_bb[VCS]);
		putchar('\n');
	}

	return NULL;
}

static void fini_workload(struct workload *wrk)
{
	free(wrk->steps);
	free(wrk);
}

static unsigned long calibrate_nop(unsigned int tolerance_pct)
{
	const uint32_t bbe = 0xa << 23;
	unsigned int loops = 17;
	unsigned int usecs = nop_calibration_us;
	struct drm_i915_gem_exec_object2 obj = {};
	struct drm_i915_gem_execbuffer2 eb =
		{ .buffer_count = 1, .buffers_ptr = (uintptr_t)&obj};
	long size, last_size;
	struct timespec t_0, t_end;

	clock_gettime(CLOCK_MONOTONIC, &t_0);

	size = 256 * 1024;
	do {
		struct timespec t_start;

		obj.handle = gem_create(fd, size);
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
		 abs(size - last_size) > (size * tolerance_pct / 100));

	return size / sizeof(uint32_t);
}

static void print_help(void)
{
	unsigned int i;

	puts(
"Usage: gem_wsim [OPTIONS]\n"
"\n"
"Runs a simulated workload on the GPU.\n"
"When ran without arguments performs a GPU calibration result of which needs to\n"
"be provided when running the simulation in subsequent invocations.\n"
"\n"
"Options:\n"
"  -h              This text.\n"
"  -q              Be quiet - do not output anything to stdout.\n"
"  -n <n>          Nop calibration value.\n"
"  -t <n>          Nop calibration tolerance percentage.\n"
"                  Use when there is a difficulty obtaining calibration with the\n"
"                  default settings.\n"
"  -p <n>          Context priority to use for the following workload on the\n"
"                  command line.\n"
"  -w <desc|path>  Filename or a workload descriptor.\n"
"                  Can be given multiple times.\n"
"  -W <desc|path>  Filename or a master workload descriptor.\n"
"                  Only one master workload can be optinally specified in which\n"
"                  case all other workloads become background ones and run as\n"
"                  long as the master.\n"
"  -a <desc|path>  Append a workload to all other workloads.\n"
"  -r <n>          How many times to emit the workload.\n"
"  -c <n>          Fork N clients emitting the workload simultaneously.\n"
"  -x              Swap VCS1 and VCS2 engines in every other client.\n"
"  -b <n>          Load balancing to use.\n"
"                  Available load balancers are:"
	);

	for (i = 0; i < ARRAY_SIZE(all_balancers); i++) {
		igt_assert(all_balancers[i].desc);
		printf(
"                     %s (%u): %s\n",
		       all_balancers[i].name, all_balancers[i].id,
		       all_balancers[i].desc);
	}
	puts(
"                  Balancers can be specified either as names or as their id\n"
"                  number as listed above.\n"
"  -2              Remap VCS2 to BCS.\n"
"  -R              Round-robin initial VCS assignment per client.\n"
"  -S              Synchronize the sequence of random batch durations between\n"
"                  clients."
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
add_workload_arg(struct w_arg *w_args, unsigned int nr_args, char *w_arg, int prio)
{
	w_args = realloc(w_args, sizeof(*w_args) * nr_args);
	igt_assert(w_args);
	w_args[nr_args - 1] = (struct w_arg) { w_arg, NULL, prio };

	return w_args;
}

static int find_balancer_by_name(char *name)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(all_balancers); i++) {
		if (!strcasecmp(name, all_balancers[i].name))
			return all_balancers[i].id;
	}

	return -1;
}

static const struct workload_balancer *find_balancer_by_id(unsigned int id)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(all_balancers); i++) {
		if (id == all_balancers[i].id)
			return &all_balancers[i];
	}

	return NULL;
}

static void init_clocks(void)
{
	struct timespec t_start, t_end;
	uint32_t rcs_start, rcs_end;
	double overhead, t;

	intel_register_access_init(intel_get_pci_device(), false, fd);

	if (verbose <= 1)
		return;

	clock_gettime(CLOCK_MONOTONIC, &t_start);
	for (int i = 0; i < 100; i++)
		rcs_start = *REG(RCS_TIMESTAMP);
	clock_gettime(CLOCK_MONOTONIC, &t_end);
	overhead = 2 * elapsed(&t_start, &t_end) / 100;

	clock_gettime(CLOCK_MONOTONIC, &t_start);
	for (int i = 0; i < 100; i++)
		clock_gettime(CLOCK_MONOTONIC, &t_end);
	clock_gettime(CLOCK_MONOTONIC, &t_end);
	overhead += elapsed(&t_start, &t_end) / 100;

	clock_gettime(CLOCK_MONOTONIC, &t_start);
	rcs_start = *REG(RCS_TIMESTAMP);
	usleep(100);
	rcs_end = *REG(RCS_TIMESTAMP);
	clock_gettime(CLOCK_MONOTONIC, &t_end);

	t = elapsed(&t_start, &t_end) - overhead;
	printf("%d cycles in %.1fus, i.e. 1024 cycles takes %1.fus\n",
	       rcs_end - rcs_start, 1e6*t, 1024e6 * t / (rcs_end - rcs_start));
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
	const struct workload_balancer *balancer = NULL;
	char *endptr = NULL;
	int prio = 0;
	double t;
	int i, c;

	/*
	 * Open the device via the low-level API so we can do the GPU quiesce
	 * manually as close as possible in time to the start of the workload.
	 * This minimizes the gap in engine utilization tracking when observed
	 * via external tools like trace.pl.
	 */
	fd = __drm_open_driver(DRIVER_INTEL);
	igt_require(fd);

	init_clocks();

	while ((c = getopt(argc, argv, "hqv2RSHxc:n:r:w:W:a:t:b:p:")) != -1) {
		switch (c) {
		case 'W':
			if (master_workload >= 0) {
				if (verbose)
					fprintf(stderr,
						"Only one master workload can be given!\n");
				return 1;
			}
			master_workload = nr_w_args;
			/* Fall through */
		case 'w':
			w_args = add_workload_arg(w_args, ++nr_w_args, optarg, prio);
			break;
		case 'p':
			prio = atoi(optarg);
			break;
		case 'a':
			if (append_workload_arg) {
				if (verbose)
					fprintf(stderr,
						"Only one append workload can be given!\n");
				return 1;
			}
			append_workload_arg = optarg;
			break;
		case 'c':
			clients = strtol(optarg, NULL, 0);
			break;
		case 't':
			tolerance_pct = strtol(optarg, NULL, 0);
			break;
		case 'n':
			nop_calibration = strtol(optarg, NULL, 0);
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
		case 'x':
			flags |= SWAPVCS;
			break;
		case '2':
			flags |= VCS2REMAP;
			break;
		case 'R':
			flags |= INITVCSRR;
			break;
		case 'S':
			flags |= SYNCEDCLIENTS;
			break;
		case 'H':
			flags |= HEARTBEAT;
			break;
		case 'b':
			i = find_balancer_by_name(optarg);
			if (i < 0) {
				i = strtol(optarg, &endptr, 0);
				if (endptr && *endptr)
					i = -1;
			}

			if (i >= 0) {
				balancer = find_balancer_by_id(i);
				if (balancer) {
					igt_assert(intel_gen(intel_get_drm_devid(fd)) >= balancer->min_gen);
					flags |= BALANCE | balancer->flags;
				}
			}

			if (!balancer) {
				if (verbose)
					fprintf(stderr,
						"Unknown balancing mode '%s'!\n",
						optarg);
				return 1;
			}
			break;
		case 'h':
			print_help();
			return 0;
		default:
			return 1;
		}
	}

	if (!nop_calibration) {
		if (verbose > 1)
			printf("Calibrating nop delay with %u%% tolerance...\n",
				tolerance_pct);
		nop_calibration = calibrate_nop(tolerance_pct);
		if (verbose)
			printf("Nop calibration for %uus delay is %lu.\n",
			       nop_calibration_us, nop_calibration);

		return 0;
	}

	if (!nr_w_args) {
		if (verbose)
			fprintf(stderr, "No workload descriptor(s)!\n");
		return 1;
	}

	if (nr_w_args > 1 && clients > 1) {
		if (verbose)
			fprintf(stderr,
				"Cloned clients cannot be combined with multiple workloads!\n");
		return 1;
	}

	if (append_workload_arg) {
		append_workload_arg = load_workload_descriptor(append_workload_arg);
		if (!append_workload_arg) {
			if (verbose)
				fprintf(stderr,
					"Failed to load append workload descriptor!\n");
			return 1;
		}
	}

	if (append_workload_arg) {
		struct w_arg arg = { NULL, append_workload_arg, 0 };
		app_w = parse_workload(&arg, flags, NULL);
		if (!app_w) {
			if (verbose)
				fprintf(stderr,
					"Failed to parse append workload!\n");
			return 1;
		}
	}

	wrk = calloc(nr_w_args, sizeof(*wrk));
	igt_assert(wrk);

	for (i = 0; i < nr_w_args; i++) {
		w_args[i].desc = load_workload_descriptor(w_args[i].filename);

		if (!w_args[i].desc) {
			if (verbose)
				fprintf(stderr,
					"Failed to load workload descriptor %u!\n",
					i);
			return 1;
		}

		wrk[i] = parse_workload(&w_args[i], flags, app_w);
		if (!wrk[i]) {
			if (verbose)
				fprintf(stderr,
					"Failed to parse workload %u!\n", i);
			return 1;
		}
	}

	if (nr_w_args > 1)
		clients = nr_w_args;

	if (verbose > 1) {
		printf("Using %lu nop calibration for %uus delay.\n",
		       nop_calibration, nop_calibration_us);
		printf("%u client%s.\n", clients, clients > 1 ? "s" : "");
		if (flags & SWAPVCS)
			printf("Swapping VCS rings between clients.\n");
		if (balancer)
			printf("Using %s balancer.\n", balancer->name);
	}

	if (master_workload >= 0 && clients == 1)
		master_workload = -1;

	w = calloc(clients, sizeof(struct workload *));
	igt_assert(w);

	for (i = 0; i < clients; i++) {
		unsigned int flags_ = flags;

		w[i] = clone_workload(wrk[nr_w_args > 1 ? i : 0]);

		if (flags & SWAPVCS && i & 1)
			flags_ &= ~SWAPVCS;

		prepare_workload(i, w[i], flags_);

		w[i]->flags = flags;
		w[i]->repeat = repeat;
		w[i]->balancer = balancer;
		w[i]->run = true;
		w[i]->background = master_workload >= 0 && i != master_workload;
		w[i]->print_stats = verbose > 1 ||
				    (verbose > 0 && master_workload == i);
	}

	gem_quiescent_gpu(fd);

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

	return 0;
}
