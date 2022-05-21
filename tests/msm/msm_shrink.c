/*
 * Copyright © 2022 Google, Inc.
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
 */

#include <fcntl.h>
#include <sys/mman.h>

#include "igt.h"
#include "igt_msm.h"
#include "igt_os.h"
#include "igt_sysfs.h"

#define SZ_1M (1024 * 1024)

static void leak(uint64_t alloc)
{
	char *ptr;

	ptr = mmap(NULL, alloc, PROT_READ | PROT_WRITE,
		   MAP_ANON | MAP_PRIVATE | MAP_POPULATE,
		   -1, 0);
	if (ptr == MAP_FAILED)
		return;

	while (alloc > 4096) {
		alloc -= 4096;
		ptr[alloc] = 0;
	}
}

static void madvise_dontneed(struct msm_bo *bo)
{
	struct drm_msm_gem_madvise req = {
		.handle = bo->handle,
		.madv = MSM_MADV_DONTNEED,
	};
	do_ioctl(bo->dev->fd, DRM_IOCTL_MSM_GEM_MADVISE, &req);
}

static struct msm_cmd *cmd_copy_gpu(struct msm_pipe *pipe, unsigned num_bos, struct msm_bo **bos)
{
	struct msm_cmd *cmd = igt_msm_cmd_new(pipe, 0x1000);

	assert((num_bos % 2) == 0);

	for (unsigned i = 0; i < (num_bos / 2); i++) {
		struct msm_bo *dst = bos[2*i];
		struct msm_bo *src = bos[2*i+1];

		unsigned dwords = min(0x2000u, dst->size / 4);

		msm_cmd_pkt7(cmd, CP_MEMCPY, 5);
		msm_cmd_emit(cmd, dwords);          /* DWORDS */
		msm_cmd_bo  (cmd, src, 0);          /* SRC_LO/HI */
		msm_cmd_bo  (cmd, dst, 0);          /* DST_LO/HI */
		msm_cmd_pkt7(cmd, CP_WAIT_MEM_WRITES, 0);
		msm_cmd_pkt7(cmd, CP_WAIT_FOR_IDLE, 0);
		msm_cmd_pkt7(cmd, CP_WAIT_FOR_ME, 0);
	}

	return cmd;
}

static void *map_dmabuf(struct msm_bo *bo)
{
	int fd, ret;
	void *ptr;

	ret = drmPrimeHandleToFD(bo->dev->fd, bo->handle, DRM_CLOEXEC | DRM_RDWR, &fd);
	igt_assert_eq(ret, 0);

	ptr = mmap(0, bo->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	igt_assert(ptr != MAP_FAILED);

	close(fd);

	return ptr;
}

struct test {
	const char *name;
	struct msm_cmd *(*cmd)(struct msm_pipe *pipe, unsigned num_bos, struct msm_bo **bos);
	void *(*map)(struct msm_bo *bo);
} tests[] = {
	{ "copy-gpu", cmd_copy_gpu },
	{ "copy-mmap", cmd_copy_gpu, igt_msm_bo_map },
	{ "copy-mmap-dmabuf", cmd_copy_gpu, map_dmabuf },
	{},
};

enum testmode {
	SANITY_CHECK	= BIT(0),
	SINGLE_THREAD	= BIT(1),
	MADVISE		= BIT(2),
	OOM		= BIT(3),
};

static const struct mode {
	const char *suffix;
	unsigned flags;
} modes[] = {
	{ "-sanitycheck", SANITY_CHECK },
/* Disable by default to keep test runtime down:
	{ "-singlethread", SINGLE_THREAD },
 */
	{ "", 0 },
	{ "-madvise", MADVISE },
	{ "-oom", OOM },
	{ NULL },
};

static void do_test(int num_submits, uint64_t alloc_size_kb, int num_bos,
		    unsigned timeout, bool do_madvise, const struct test *test)
{
	struct msm_device *dev = igt_msm_dev_open();
	struct msm_pipe *pipe = igt_msm_pipe_open(dev, 0);
	struct msm_bo *bo[num_submits][num_bos];
	struct msm_cmd *cmd[num_submits];
	void *map[num_submits][num_bos];
	int fence_fd = -1;

	/* Allocate the buffer objects and prepare the cmds: */
	for (int i = 0; i < num_submits; i++) {
		for (int j = 0; j < num_bos; j++) {
			bo[i][j] = igt_msm_bo_new(dev, alloc_size_kb * 1024, MSM_BO_WC);
		}
		cmd[i] = test->cmd(pipe, num_bos, bo[i]);
	}

	/* Prepare the CPU maps, if necessary: */
	if (test->map) {
		for (int i = 0; i < num_submits; i++) {
			for (int j = 0; j < num_bos; j++) {
				map[i][j] = test->map(bo[i][j]);
				memset(map[i][j], 0xde, bo[i][j]->size);
			}
		}
	}

	igt_until_timeout(timeout) {
		for (int i = 0; i < num_submits / 2; i++) {
			if (fence_fd > 0)
				close(fence_fd);
			fence_fd = igt_msm_cmd_submit(cmd[i]);
		}

		igt_wait_and_close(fence_fd);
		fence_fd = -1;

		if (test->map) {
			for (int i = 0; i < num_submits; i++) {
				for (int j = 0; j < num_bos; j++) {
					memset(map[i][j], 0xde, bo[i][j]->size);
				}
			}
		}

		for (int i = num_submits / 2; i < num_submits; i++) {
			if (fence_fd > 0)
				close(fence_fd);
			fence_fd = igt_msm_cmd_submit(cmd[i]);
		}
		igt_wait_and_close(fence_fd);
		fence_fd = -1;
	}

	if (do_madvise) {
		for (int i = 0; i < num_submits; i++) {
			if (fence_fd > 0)
				close(fence_fd);
			fence_fd = igt_msm_cmd_submit(cmd[i]);
			for (int j = 0; j < num_bos; j++) {
				madvise_dontneed(bo[i][j]);
			}
		}
		igt_wait_and_close(fence_fd);
		fence_fd = -1;
	}
}

static void run_test(int nchildren, uint64_t alloc_size_mb, unsigned num_bos,
		     const struct test *test, unsigned flags)
{
	const int timeout = (test->map) || (flags & (SANITY_CHECK | MADVISE)) ? 1 : 10;
	bool madvise = !!(flags & MADVISE);
	uint64_t alloc_size_kb;

	/* We are trying to use more GEM buffers that will fit in
	 * memory, but less than 2x avail RAM.  Split across at
	 * least two submits so we aren't getting into a scenario
	 * where all the children are trying to pin all the memory
	 * at the same time and get into a situation where no one
	 * can make forward progress.
	 */
	unsigned num_submits = 8;

	if (flags & SANITY_CHECK)
		nchildren = 1;

	alloc_size_kb = DIV_ROUND_UP(alloc_size_mb * 1024, num_bos * num_submits);

	if (flags & SINGLE_THREAD) {
		num_submits *= nchildren;
		nchildren = 1;
	}

	igt_info("%s, %d submits, %d processes, and %d x %'"PRIu64"KiB bos per submit for total size of %'"PRIu64"KiB\n",
		 test->name, num_submits, nchildren, num_bos, alloc_size_kb,
		 num_bos * num_submits * nchildren * alloc_size_kb);

	/* Background load */
	if (flags & OOM) {
		igt_fork(child, nchildren) {
			for (int pass = 0; pass < nchildren; pass++)
				leak(alloc_size_kb / 1024);
		}
	}

	/* Exercise major ioctls */
	igt_fork(child, nchildren) {
		do_test(num_submits, alloc_size_kb, num_bos, timeout, madvise, test);
	}
	igt_waitchildren();
}

static const unsigned num_bos[] = { 8, 32 };

igt_main
{
	struct msm_device *dev = NULL;
	uint64_t alloc_size_mb = 0;
	int num_processes = 0;

	igt_fixture {
		int params, ncpus;
		uint64_t mem_size;
		uint64_t swap_size;

		/* Make sure we are running on the right hw: */
		dev = igt_msm_dev_open();

		igt_require(dev->gen >= 6);

		/* Ensure that eviction is enabled: */
		params = igt_params_open(dev->fd);
		igt_sysfs_set(params, "enable_eviction", "1");
		igt_sysfs_set(params, "address_space_size", "0x400000000");
		close(params);

		/* Figure out # of processes and allocation size: */
		ncpus = sysconf(_SC_NPROCESSORS_ONLN);
		mem_size = igt_get_total_ram_mb();
		swap_size = igt_get_total_swap_mb();

		igt_require(swap_size > 0);

		/*
		 * Spawn enough processes to use all memory, but each only
		 * uses a fraction of the available per-cpu memory.
		 * Individually the processes would be ok, but en masse
		 * we expect the shrinker to start purging objects,
		 * and possibly fail.
		 *
		 * Note, consider only a fraction of avail swap when
		 * calculating target size, as we have no good way to
		 * determine if it is zram swap (which will consume an
		 * increasing portion of RAM as it is filled)
		 */
		mem_size += swap_size / 4;
		alloc_size_mb = (mem_size + ncpus - 1) / ncpus / 8;
		num_processes = ncpus + (mem_size / alloc_size_mb);

		igt_info("Using %d processes and %'"PRIu64"MiB per process for total size of %'"PRIu64"MiB\n",
			 num_processes, alloc_size_mb, num_processes * alloc_size_mb);

		igt_require_memory(num_processes, alloc_size_mb,
				   CHECK_SWAP | CHECK_RAM);
	}

	for(const struct test *t = tests; t->name; t++) {
		for(const struct mode *m = modes; m->suffix; m++) {
			for (int i = 0; i < ARRAY_SIZE(num_bos); i++) {
				igt_subtest_f("%s%s-%u", t->name, m->suffix, num_bos[i]) {
					run_test(num_processes, alloc_size_mb,
						 num_bos[i], t, m->flags);
				}
			}
		}
	}

	igt_fixture {
		igt_msm_dev_close(dev);
	}
}
