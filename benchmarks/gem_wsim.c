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


#include "intel_chipset.h"
#include "drm.h"
#include "ioctl_wrappers.h"
#include "drmtest.h"
#include "intel_io.h"
#include "igt_rand.h"

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
	QD_THROTTLE
};

struct w_step
{
	/* Workload step metadata */
	enum w_type type;
	unsigned int context;
	unsigned int engine;
	struct duration duration;
	int nr_deps;
	int *dep;
	int wait;

	/* Implementation details */
	unsigned int idx;

	struct drm_i915_gem_execbuffer2 eb;
	struct drm_i915_gem_exec_object2 *obj;
	struct drm_i915_gem_relocation_entry reloc[3];
	unsigned long bb_sz;
	uint32_t bb_handle;
	uint32_t *mapped_batch;
	uint32_t *seqno_value;
	uint32_t *seqno_address;
	uint32_t *rt0_value;
	uint32_t *rt0_address;
	uint32_t *rt1_address;
	unsigned int mapped_len;
};

struct workload
{
	unsigned int nr_steps;
	struct w_step *steps;

	struct timespec repeat_start;

	int pipe[2];

	unsigned int nr_ctxs;
	uint32_t *ctx_id;

	uint32_t seqno[NUM_ENGINES];
	uint32_t status_page_handle;
	uint32_t *status_page;
	unsigned int vcs_rr;

	unsigned long qd_sum[NUM_ENGINES];
	unsigned long nr_bb[NUM_ENGINES];
};

static const unsigned int eb_engine_map[NUM_ENGINES] = {
	[RCS] = I915_EXEC_RENDER,
	[BCS] = I915_EXEC_BLT,
	[VCS] = I915_EXEC_BSD,
	[VCS1] = I915_EXEC_BSD | I915_EXEC_BSD_RING1,
	[VCS2] = I915_EXEC_BSD | I915_EXEC_BSD_RING2,
	[VECS] = I915_EXEC_VEBOX
};

static const unsigned int nop_calibration_us = 1000;
static unsigned long nop_calibration;

static bool quiet;
static int fd;

#define SWAPVCS	(1<<0)
#define SEQNO	(1<<1)
#define BALANCE	(1<<2)
#define RT	(1<<3)

#define VCS_SEQNO_IDX(engine) (((engine) - VCS1) * 16)
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

static int parse_dependencies(struct w_step *w, char *_desc)
{
	char *desc = strdup(_desc);
	char *token, *tctx = NULL, *tstart = desc;
	int dep;

	igt_assert(desc);

	w->nr_deps = 0;
	w->dep = NULL;

	while ((token = strtok_r(tstart, "/", &tctx)) != NULL) {
		tstart = NULL;

		dep = atoi(token);
		if (dep > 0) {
			if (w->dep)
				free(w-dep);
			return -1;
		}

		if (dep < 0) {
			w->nr_deps++;
			w->dep = realloc(w->dep, sizeof(*w->dep) * w->nr_deps);
			igt_assert(w->dep);
			w->dep[w->nr_deps - 1] = dep;
		}
	}

	free(desc);

	return 0;
}

static struct workload *parse_workload(char *_desc)
{
	struct workload *wrk;
	unsigned int nr_steps = 0;
	char *desc = strdup(_desc);
	char *_token, *token, *tctx = NULL, *tstart = desc;
	char *field, *fctx = NULL, *fstart;
	struct w_step step, *steps = NULL;
	unsigned int valid;
	int tmp;

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
						if (!quiet)
							fprintf(stderr,
								"Invalid delay at step %u!\n",
								nr_steps);
						return NULL;
					}

					step.type = DELAY;
					step.wait = tmp;
					goto add_step;
				}
			} else if (!strcasecmp(field, "p")) {
				if ((field = strtok_r(fstart, ".", &fctx)) !=
				    NULL) {
					tmp = atoi(field);
					if (tmp <= 0) {
						if (!quiet)
							fprintf(stderr,
								"Invalid period at step %u!\n",
								nr_steps);
						return NULL;
					}

					step.type = PERIOD;
					step.wait = tmp;
					goto add_step;
				}
			} else if (!strcasecmp(field, "s")) {
				if ((field = strtok_r(fstart, ".", &fctx)) !=
				    NULL) {
					tmp = atoi(field);
					if (tmp >= 0) {
						if (!quiet)
							fprintf(stderr,
								"Invalid sync target at step %u!\n",
								nr_steps);
						return NULL;
					}

					step.type = SYNC;
					step.wait = tmp;
					goto add_step;
				}
			} else if (!strcasecmp(field, "t")) {
				if ((field = strtok_r(fstart, ".", &fctx)) !=
				    NULL) {
					tmp = atoi(field);
					if (tmp < 0) {
						if (!quiet)
							fprintf(stderr,
								"Invalid throttle at step %u!\n",
								nr_steps);
						return NULL;
					}

					step.type = THROTTLE;
					step.wait = tmp;
					goto add_step;
				}
			} else if (!strcasecmp(field, "q")) {
				if ((field = strtok_r(fstart, ".", &fctx)) !=
				    NULL) {
					tmp = atoi(field);
					if (tmp < 0) {
						if (!quiet)
							fprintf(stderr,
								"Invalid qd throttle at step %u!\n",
								nr_steps);
						return NULL;
					}

					step.type = QD_THROTTLE;
					step.wait = tmp;
					goto add_step;
				}
			}

			tmp = atoi(field);
			if (tmp < 0) {
				if (!quiet)
					fprintf(stderr,
						"Invalid ctx id at step %u!\n",
						nr_steps);
				return NULL;
			}
			step.context = tmp;

			valid++;
		}

		if ((field = strtok_r(fstart, ".", &fctx)) != NULL) {
			unsigned int i, old_valid = valid;

			fstart = NULL;

			for (i = 0; i < ARRAY_SIZE(ring_str_map); i++) {
				if (!strcasecmp(field, ring_str_map[i])) {
					step.engine = i;
					valid++;
					break;
				}
			}

			if (old_valid == valid) {
				if (!quiet)
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
			if (tmpl == LONG_MIN || tmpl == LONG_MAX) {
				if (!quiet)
					fprintf(stderr,
						"Invalid duration at step %u!\n",
						nr_steps);
				return NULL;
			}
			step.duration.min = tmpl;

			if (sep && *sep == '-') {
				tmpl = strtol(sep + 1, NULL, 10);
				if (tmpl == LONG_MIN || tmpl == LONG_MAX) {
					if (!quiet)
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

			tmp = parse_dependencies(&step, field);
			if (tmp < 0) {
				if (!quiet)
					fprintf(stderr,
						"Invalid forward dependency at step %u!\n",
						nr_steps);
				return NULL;
			}

			valid++;
		}

		if ((field = strtok_r(fstart, ".", &fctx)) != NULL) {
			fstart = NULL;

			tmp = atoi(field);
			if (tmp != 0 && tmp != 1) {
				if (!quiet)
					fprintf(stderr,
						"Invalid wait boolean at step %u!\n",
						nr_steps);
				return NULL;
			}
			step.wait = tmp;

			valid++;
		}

		if (valid != 5) {
			if (!quiet)
				fprintf(stderr, "Invalid record at step %u!\n",
					nr_steps);
			return NULL;
		}

		step.type = BATCH;

add_step:
		step.idx = nr_steps++;
		steps = realloc(steps, sizeof(step) * nr_steps);
		igt_assert(steps);

		memcpy(&steps[nr_steps - 1], &step, sizeof(step));

		free(token);
	}

	wrk = malloc(sizeof(*wrk));
	igt_assert(wrk);

	wrk->nr_steps = nr_steps;
	wrk->steps = steps;

	free(desc);

	return wrk;
}

static struct workload *
clone_workload(struct workload *_wrk)
{
	struct workload *wrk;

	wrk = malloc(sizeof(*wrk));
	igt_assert(wrk);
	memset(wrk, 0, sizeof(*wrk));

	wrk->nr_steps = _wrk->nr_steps;
	wrk->steps = calloc(wrk->nr_steps, sizeof(struct w_step));
	igt_assert(wrk->steps);

	memcpy(wrk->steps, _wrk->steps, sizeof(struct w_step) * wrk->nr_steps);

	return wrk;
}

#define rounddown(x, y) (x - (x%y))
#ifndef PAGE_SIZE
#define PAGE_SIZE (4096)
#endif

static unsigned int get_duration(struct duration *dur)
{
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
		batch_start -= 8 * sizeof(uint32_t);

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
	}

	*cs = bbe;

	w->mapped_batch = ptr;
	w->mapped_len = mmap_len;
}

static void
eb_update_flags(struct w_step *w, enum intel_engine_id engine,
		unsigned int flags)
{
	w->eb.flags = eb_engine_map[engine];
	w->eb.flags |= I915_EXEC_HANDLE_LUT;
	w->eb.flags |= I915_EXEC_NO_RELOC;
}

static void
alloc_step_batch(struct workload *wrk, struct w_step *w, unsigned int flags)
{
	enum intel_engine_id engine = w->engine;
	unsigned int j = 0;
	unsigned int nr_obj = 3 + w->nr_deps;
	unsigned int i;

	w->obj = calloc(nr_obj, sizeof(*w->obj));
	igt_assert(w->obj);

	w->obj[j].handle = gem_create(fd, 4096);
	w->obj[j].flags = EXEC_OBJECT_WRITE;
	j++;
	igt_assert(j < nr_obj);

	if (flags & SEQNO) {
		w->obj[j].handle = wrk->status_page_handle;
		j++;
		igt_assert(j < nr_obj);
	}

	for (i = 0; i < w->nr_deps; i++) {
		igt_assert(w->dep[i] <= 0);
		if (w->dep[i]) {
			int dep_idx = w->idx + w->dep[i];

			igt_assert(dep_idx >= 0 && dep_idx < wrk->nr_steps);
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
			w->obj[j].relocation_count = 3;
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
prepare_workload(struct workload *wrk, unsigned int flags)
{
	int max_ctx = -1;
	struct w_step *w;
	int i;

	if (flags & SEQNO) {
		const unsigned int status_sz = sizeof(uint32_t);
		uint32_t handle = gem_create(fd, status_sz);

		gem_set_caching(fd, handle, I915_CACHING_CACHED);
		wrk->status_page_handle = handle;
		wrk->status_page = gem_mmap__cpu(fd, handle, 0, status_sz,
						 PROT_READ);
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
		}
	}

	for (i = 0, w = wrk->steps; i < wrk->nr_steps; i++, w++) {
		unsigned int _flags = flags;
		enum intel_engine_id engine = w->engine;

		if (w->type != BATCH)
			continue;

		if (engine != VCS && engine != VCS1 && engine != VCS2)
			_flags &= ~(SEQNO | RT);

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

struct workload_balancer {
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

static const struct workload_balancer rr_balancer = {
	.balance = rr_balance,
};

static unsigned int
get_qd_depth(const struct workload_balancer *balancer,
	     struct workload *wrk, enum intel_engine_id engine)
{
	return wrk->seqno[engine] -
	       wrk->status_page[VCS_SEQNO_IDX(engine)];
}

static enum intel_engine_id
qd_balance(const struct workload_balancer *balancer,
	   struct workload *wrk, struct w_step *w)
{
	enum intel_engine_id engine;
	long qd[NUM_ENGINES];
	unsigned int n;

	igt_assert(w->engine == VCS);

	qd[VCS1] = balancer->get_qd(balancer, wrk, VCS1);
	wrk->qd_sum[VCS1] += qd[VCS1];

	qd[VCS2] = balancer->get_qd(balancer, wrk, VCS2);
	wrk->qd_sum[VCS2] += qd[VCS2];

	if (qd[VCS1] < qd[VCS2])
		n = 0;
	else if (qd[VCS2] < qd[VCS1])
		n = 1;
	else
		n = wrk->vcs_rr;

	engine = get_vcs_engine(n);
	wrk->vcs_rr = n ^ 1;

#ifdef DEBUG
	printf("qd_balance: 1:%ld 2:%ld rr:%u = %u\t(%lu - %u) (%lu - %u)\n",
	       qd[VCS1], qd[VCS2], wrk->vcs_rr, engine,
	       wrk->seqno[VCS1], wrk->status_page[VCS_SEQNO_IDX(VCS1)],
	       wrk->seqno[VCS2], wrk->status_page[VCS_SEQNO_IDX(VCS2)]);
#endif
	return engine;
}

static const struct workload_balancer qd_balancer = {
	.get_qd = get_qd_depth,
	.balance = qd_balance,
};

static enum intel_engine_id
rt_balance(const struct workload_balancer *balancer,
	   struct workload *wrk, struct w_step *w)
{
	enum intel_engine_id engine;
	long qd[NUM_ENGINES];
	unsigned int n;

	igt_assert(w->engine == VCS);

	/* Estimate the "speed" of the most recent batch
	 *    (finish time - submit time)
	 * and use that as an approximate for the total remaining time for
	 * all batches on that engine. We try to keep the total remaining
	 * balanced between the engines.
	 */
	qd[VCS1] = balancer->get_qd(balancer, wrk, VCS1);
	wrk->qd_sum[VCS1] += qd[VCS1];
	qd[VCS1] *= wrk->status_page[2] - wrk->status_page[1];
#ifdef DEBUG
	printf("qd[0] = %d (%d - %d) x %d (%d - %d) = %ld\n",
	       wrk->seqno[VCS1] - wrk->status_page[0],
	       wrk->seqno[VCS1], wrk->status_page[0],
	       wrk->status_page[2] - wrk->status_page[1],
	       wrk->status_page[2], wrk->status_page[1],
	       qd[VCS1]);
#endif

	qd[VCS2] = balancer->get_qd(balancer, wrk, VCS2);
	wrk->qd_sum[VCS2] += qd[VCS2];
	qd[VCS2] *= wrk->status_page[2 + 16] - wrk->status_page[1 + 16];
#ifdef DEBUG
	printf("qd[1] = %d (%d - %d) x %d (%d - %d) = %ld\n",
	       wrk->seqno[VCS2] - wrk->status_page[16],
	       wrk->seqno[VCS2], wrk->status_page[16],
	       wrk->status_page[18] - wrk->status_page[17],
	       wrk->status_page[18], wrk->status_page[17],
	       qd[VCS2]);
#endif

	if (qd[VCS1] < qd[VCS2])
		n = 0;
	else if (qd[VCS2] < qd[VCS1])
		n = 1;
	else
		n = wrk->vcs_rr;

	engine = get_vcs_engine(n);
	wrk->vcs_rr = n ^ 1;

	return engine;
}

static const struct workload_balancer rt_balancer = {
	.get_qd = get_qd_depth,
	.balance = rt_balance,
};

static void
update_bb_seqno(struct w_step *w, enum intel_engine_id engine, uint32_t seqno)
{
	igt_assert(engine == VCS1 || engine == VCS2);

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
update_bb_rt(struct w_step *w, enum intel_engine_id engine)
{
	igt_assert(engine == VCS1 || engine == VCS2);

	gem_set_domain(fd, w->bb_handle,
		       I915_GEM_DOMAIN_WC, I915_GEM_DOMAIN_WC);

	w->reloc[1].delta = VCS_SEQNO_OFFSET(engine) + sizeof(uint32_t);
	w->reloc[2].delta = VCS_SEQNO_OFFSET(engine) + 2 * sizeof(uint32_t);

	*w->rt0_value = *REG(RCS_TIMESTAMP);
	*w->rt0_address = w->reloc[1].presumed_offset + w->reloc[1].delta;
	*w->rt1_address = w->reloc[2].presumed_offset + w->reloc[2].delta;

	/* If not using NO_RELOC, force the relocations */
	if (!(w->eb.flags & I915_EXEC_NO_RELOC)) {
		w->reloc[1].presumed_offset = -1;
		w->reloc[2].presumed_offset = -1;
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

static void
run_workload(unsigned int id, struct workload *wrk,
	     bool background, int pipe_fd,
	     const struct workload_balancer *balancer,
	     unsigned int repeat,
	     unsigned int flags)
{
	struct timespec t_start, t_end;
	struct w_step *w;
	bool run = true;
	int throttle = -1;
	int qd_throttle = -1;
	double t;
	int i, j;

	clock_gettime(CLOCK_MONOTONIC, &t_start);

	hars_petruska_f54_1_random_seed(0);

	for (j = 0; run && (background || j < repeat); j++) {
		clock_gettime(CLOCK_MONOTONIC, &wrk->repeat_start);

		for (i = 0, w = wrk->steps; run && (i < wrk->nr_steps);
		     i++, w++) {
			enum intel_engine_id engine = w->engine;
			int do_sleep = 0;

			if (w->type == DELAY) {
				do_sleep = w->wait;
			} else if (w->type == PERIOD) {
				struct timespec now;

				clock_gettime(CLOCK_MONOTONIC, &now);
				do_sleep = w->wait -
					   elapsed_us(&wrk->repeat_start, &now);
				if (do_sleep < 0) {
					if (!quiet) {
						printf("%u: Dropped period @ %u/%u (%dus late)!\n",
						       id, j, i, do_sleep);
						continue;
					}
				}
			} else if (w->type == SYNC) {
				unsigned int s_idx = i + w->wait;

				igt_assert(i > 0 && i < wrk->nr_steps);
				igt_assert(wrk->steps[s_idx].type == BATCH);
				gem_sync(fd, wrk->steps[s_idx].obj[0].handle);
				continue;
			} else if (w->type == THROTTLE) {
				throttle = w->wait;
				continue;
			} else if (w->type == QD_THROTTLE) {
				qd_throttle = w->wait;
				continue;
			}

			if (do_sleep) {
				usleep(do_sleep);
				continue;
			}

			wrk->nr_bb[engine]++;

			if (engine == VCS && balancer) {
				engine = balancer->balance(balancer, wrk, w);
				wrk->nr_bb[engine]++;

				eb_update_flags(w, engine, flags);

				if (flags & SEQNO)
					update_bb_seqno(w, engine,
							++wrk->seqno[engine]);
				if (flags & RT)
					update_bb_rt(w, engine);
			}

			if (w->duration.min != w->duration.max) {
				unsigned int d = get_duration(&w->duration);
				unsigned long offset;

				offset = ALIGN(w->bb_sz - get_bb_sz(d),
					       2 * sizeof(uint32_t));
				w->eb.batch_start_offset = offset;
			}

			/* If workload want qd throttling when qd is not
			 * available approximate with normal throttling. */
			if (qd_throttle > 0 && throttle < 0 &&
			    !(balancer && balancer->get_qd))
				throttle = qd_throttle;

			if (throttle > 0)
				w_sync_to(wrk, w, i - throttle);

			if (qd_throttle > 0 && balancer && balancer->get_qd) {
				unsigned int target;

				for (target = wrk->nr_steps - 1; target > 0;
				     target--) {
					if (balancer->get_qd(balancer, wrk,
							     engine) <
					    qd_throttle)
						break;
					w_sync_to(wrk, w, i - target);
				}
			}

			gem_execbuf(fd, &w->eb);

			if (pipe_fd >= 0) {
				struct pollfd fds;

				fds.fd = pipe_fd;
				fds.events = POLLHUP;
				if (poll(&fds, 1, 0)) {
					run = false;
					break;
				}
			}

			if (w->wait)
				gem_sync(fd, w->obj[0].handle);
		}
	}

	if (run)
		gem_sync(fd, wrk->steps[wrk->nr_steps - 1].obj[0].handle);

	clock_gettime(CLOCK_MONOTONIC, &t_end);

	t = elapsed(&t_start, &t_end);
	if (!quiet && !balancer)
		printf("%c%u: %.3fs elapsed (%.3f workloads/s)\n",
		       background ? ' ' : '*', id, t, repeat / t);
	else if (!quiet && !balancer->get_qd)
		printf("%c%u: %.3fs elapsed (%.3f workloads/s). %lu (%lu + %lu) total VCS batches.\n",
		       background ? ' ' : '*', id, t, repeat / t,
		       wrk->nr_bb[VCS], wrk->nr_bb[VCS1], wrk->nr_bb[VCS2]);
	else if (!quiet && balancer)
		printf("%c%u: %.3fs elapsed (%.3f workloads/s). %lu (%lu + %lu) total VCS batches. Average queue depths %.3f, %.3f.\n",
		       background ? ' ' : '*', id, t, repeat / t,
		       wrk->nr_bb[VCS], wrk->nr_bb[VCS1], wrk->nr_bb[VCS2],
		       (double)wrk->qd_sum[VCS1] / wrk->nr_bb[VCS],
		       (double)wrk->qd_sum[VCS2] / wrk->nr_bb[VCS]);
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
	puts(
"Usage: gem_wsim [OPTIONS]\n"
"\n"
"Runs a simulated workload on the GPU.\n"
"When ran without arguments performs a GPU calibration result of which needs\n"
"to be provided when running the simulation in subsequent invocations.\n"
"\n"
"Options:\n"
"	-h		This text.\n"
"	-q		Be quiet - do not output anything to stdout.\n"
"	-n <n>		Nop calibration value.\n"
"	-t <n>		Nop calibration tolerance percentage.\n"
"			Use when there is a difficulty obtaining calibration\n"
"			with the default settings.\n"
"	-w <desc|path>	Filename or a workload descriptor.\n"
"			Can be given multiple times.\n"
"	-W <desc|path>	Filename or a master workload descriptor.\n"
"			Only one master workload can be optinally specified\n"
"			in which case all other workloads become background\n"
"			ones and run as long as the master.\n"
"	-r <n>		How many times to emit the workload.\n"
"	-c <n>		Fork N clients emitting the workload simultaneously.\n"
"	-x		Swap VCS1 and VCS2 engines in every other client.\n"
"	-b <n>		Load balancing to use. (0: rr, 1: qd, 2: rt)\n"
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

static char **
add_workload_arg(char **w_args, unsigned int nr_args, char *w_arg)
{
	w_args = realloc(w_args, sizeof(char *) * nr_args);
	igt_assert(w_args);
	w_args[nr_args - 1] = w_arg;

	return w_args;
}

int main(int argc, char **argv)
{
	unsigned int repeat = 1;
	unsigned int clients = 1;
	unsigned int flags = 0;
	struct timespec t_start, t_end;
	struct workload **w, **wrk = NULL;
	unsigned int nr_w_args = 0;
	int master_workload = -1;
	char **w_args = NULL;
	unsigned int tolerance_pct = 1;
	const struct workload_balancer *balancer = NULL;
	double t;
	int i, c;

	fd = drm_open_driver(DRIVER_INTEL);
	intel_register_access_init(intel_get_pci_device(), false, fd);

	while ((c = getopt(argc, argv, "qc:n:r:xw:W:t:b:h")) != -1) {
		switch (c) {
		case 'W':
			if (master_workload >= 0) {
				if (!quiet)
					fprintf(stderr,
						"Only one master workload can be given!\n");
				return 1;
			}
			master_workload = nr_w_args;
			/* Fall through */
		case 'w':
			w_args = add_workload_arg(w_args, ++nr_w_args, optarg);
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
			quiet = true;
			break;
		case 'x':
			flags |= SWAPVCS;
			break;
		case 'b':
			switch (strtol(optarg, NULL, 0)) {
			case 0:
				balancer = &rr_balancer;
				flags |= BALANCE;
				break;
			case 1:
				igt_assert(intel_gen(intel_get_drm_devid(fd)) >=
					   8);
				balancer = &qd_balancer;
				flags |= SEQNO | BALANCE;
				break;
			case 2:
				igt_assert(intel_gen(intel_get_drm_devid(fd)) >=
					   8);
				balancer = &rt_balancer;
				flags |= SEQNO | BALANCE | RT;
				break;
			default:
				if (!quiet)
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
		if (!quiet)
			printf("Calibrating nop delay with %u%% tolerance...\n",
				tolerance_pct);
		nop_calibration = calibrate_nop(tolerance_pct);
		if (!quiet)
			printf("Nop calibration for %uus delay is %lu.\n",
			       nop_calibration_us, nop_calibration);

		return 0;
	}

	if (!nr_w_args) {
		if (!quiet)
			fprintf(stderr, "No workload descriptor(s)!\n");
		return 1;
	}

	if (nr_w_args > 1 && clients > 1) {
		if (!quiet)
			fprintf(stderr,
				"Cloned clients cannot be combined with multiple workloads!\n");
		return 1;
	}

	wrk = calloc(nr_w_args, sizeof(*wrk));
	igt_assert(wrk);

	for (i = 0; i < nr_w_args; i++) {
		w_args[i] = load_workload_descriptor(w_args[i]);
		if (!w_args[i]) {
			if (!quiet)
				fprintf(stderr,
					"Failed to load workload descriptor %u!\n",
					i);
			return 1;
		}

		wrk[i] = parse_workload(w_args[i]);
		if (!wrk[i]) {
			if (!quiet)
				fprintf(stderr,
					"Failed to parse workload %u!\n", i);
			return 1;
		}
	}

	if (!quiet) {
		printf("Using %lu nop calibration for %uus delay.\n",
		       nop_calibration, nop_calibration_us);
		if (nr_w_args > 1)
			clients = nr_w_args;
		printf("%u client%s.\n", clients, clients > 1 ? "s" : "");
		if (flags & SWAPVCS)
			printf("Swapping VCS rings between clients.\n");
	}

	if (master_workload >= 0 && clients == 1)
		master_workload = -1;

	w = calloc(clients, sizeof(struct workload *));
	igt_assert(w);

	for (i = 0; i < clients; i++) {
		unsigned int flags_ = flags;

		w[i] = clone_workload(wrk[nr_w_args > 1 ? i : 0]);

		if (master_workload >= 0) {
			int ret = pipe(w[i]->pipe);

			igt_assert(ret == 0);
		}

		if (flags & SWAPVCS && i & 1)
			flags_ &= ~SWAPVCS;

		prepare_workload(w[i], flags_);
	}

	clock_gettime(CLOCK_MONOTONIC, &t_start);

	igt_fork(child, clients) {
		int pipe_fd = -1;
		bool background = false;

		if (master_workload >= 0) {
			close(w[child]->pipe[0]);
			if (child != master_workload) {
				pipe_fd = w[child]->pipe[1];
				background = true;
			} else {
				close(w[child]->pipe[1]);
			}
		}

		run_workload(child, w[child], background, pipe_fd, balancer,
			     repeat, flags);
	}

	if (master_workload >= 0) {
		int status = -1;
		pid_t pid;

		for (i = 0; i < clients; i++)
			close(w[i]->pipe[1]);

		pid = wait(&status);
		if (pid >= 0)
			igt_child_done(pid);

		for (i = 0; i < clients; i++)
			close(w[i]->pipe[0]);
	}

	igt_waitchildren();

	clock_gettime(CLOCK_MONOTONIC, &t_end);

	t = elapsed(&t_start, &t_end);
	if (!quiet)
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
