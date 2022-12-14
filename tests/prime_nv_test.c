/* basic set of prime tests between intel and nouveau */

/* test list -
   1. share buffer from intel -> nouveau.
   2. share buffer from nouveau -> intel
   3. share intel->nouveau, map on both, write intel, read nouveau
   4. share intel->nouveau, blit intel fill, readback on nouveau
   test 1 + map buffer, read/write, map other size.
   do some hw actions on the buffer
   some illegal operations -
       close prime fd try and map

   TODO add some nouveau rendering tests
*/


#include "igt.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/ioctl.h>

#include "i915/gem_create.h"
#include "nouveau.h"

int intel_fd = -1, nouveau_fd = -1;
struct buf_ops *bops;
struct nouveau_device *ndev;
struct nouveau_client *nclient;

#define BO_SIZE (256*1024)

static int find_and_open_devices(void)
{
	int i;
	char path[80];
	struct stat buf;
	FILE *fl;
	char vendor_id[8];
	int venid;
	for (i = 0; i < 9; i++) {
		char *ret;

		sprintf(path, "/sys/class/drm/card%d/device/vendor", i);
		if (stat(path, &buf))
			break;

		fl = fopen(path, "r");
		if (!fl)
			break;

		ret = fgets(vendor_id, 8, fl);
		igt_assert(ret);
		fclose(fl);

		venid = strtoul(vendor_id, NULL, 16);
		sprintf(path, "/dev/dri/card%d", i);
		if (venid == 0x8086) {
			intel_fd = open(path, O_RDWR);
			if (!intel_fd)
				return -1;
		} else if (venid == 0x10de) {
			nouveau_fd = open(path, O_RDWR);
			if (!nouveau_fd)
				return -1;
		}
	}
	return 0;
}

/*
 * prime test 1 -
 * allocate buffer on intel,
 * set prime on buffer,
 * retrive buffer from nouveau,
 * close prime_fd,
 *  unref buffers
 */
static void test_i915_nv_sharing(void)
{
	uint32_t intel_handle;
	int prime_fd;
	struct nouveau_bo *nvbo;

	intel_handle = gem_create(intel_fd, BO_SIZE);
	prime_fd = prime_handle_to_fd(intel_fd, intel_handle);

	igt_assert(nouveau_bo_prime_handle_ref(ndev, prime_fd, &nvbo) == 0);
	close(prime_fd);

	nouveau_bo_ref(NULL, &nvbo);
	gem_close(intel_fd, intel_handle);
}

/*
 * prime test 2 -
 * allocate buffer on nouveau
 * set prime on buffer,
 * retrive buffer from intel
 * close prime_fd,
 *  unref buffers
 */
static void test_nv_i915_sharing(void)
{
	uint32_t intel_handle;
	int prime_fd;
	struct nouveau_bo *nvbo;

	igt_assert(nouveau_bo_new(ndev, NOUVEAU_BO_GART | NOUVEAU_BO_MAP,
				  0, BO_SIZE, NULL, &nvbo) == 0);
	igt_assert(nouveau_bo_set_prime(nvbo, &prime_fd) == 0);

	intel_handle = prime_fd_to_handle(intel_fd, prime_fd);
	close(prime_fd);

	nouveau_bo_ref(NULL, &nvbo);
	gem_close(intel_fd, intel_handle);
}

/*
 * allocate intel, give to nouveau, map on nouveau
 * write 0xdeadbeef, non-gtt map on intel, read
 */
static void test_nv_write_i915_cpu_mmap_read(void)
{
	uint32_t intel_handle;
	int prime_fd;
	struct nouveau_bo *nvbo = NULL;
	uint32_t *ptr;

	intel_handle = gem_create(intel_fd, BO_SIZE);
	prime_fd = prime_handle_to_fd(intel_fd, intel_handle);

	igt_assert(nouveau_bo_prime_handle_ref(ndev, prime_fd, &nvbo) == 0);
	close(prime_fd);

	igt_assert(nouveau_bo_map(nvbo, NOUVEAU_BO_RDWR, nclient) == 0);
	ptr = nvbo->map;
	*ptr = 0xdeadbeef;

	ptr = gem_mmap__cpu(intel_fd, intel_handle, 0, BO_SIZE, PROT_READ | PROT_WRITE);

	igt_assert(*ptr == 0xdeadbeef);
	nouveau_bo_ref(NULL, &nvbo);
	gem_munmap(ptr, BO_SIZE);
	gem_close(intel_fd, intel_handle);
}

/*
 * allocate intel, give to nouveau, map on nouveau
 * write 0xdeadbeef, gtt map on intel, read
 */
static void test_nv_write_i915_gtt_mmap_read(void)
{
	uint32_t intel_handle;
	int prime_fd;
	struct nouveau_bo *nvbo = NULL;
	uint32_t *ptr;

	intel_handle = gem_create(intel_fd, BO_SIZE);
	prime_fd = prime_handle_to_fd(intel_fd, intel_handle);

	igt_assert(nouveau_bo_prime_handle_ref(ndev, prime_fd, &nvbo) == 0);
	close(prime_fd);
	igt_assert(nouveau_bo_map(nvbo, NOUVEAU_BO_RDWR, nclient) == 0);
	ptr = nvbo->map;
	*ptr = 0xdeadbeef;

	ptr = gem_mmap__gtt(intel_fd, intel_handle, BO_SIZE, PROT_READ | PROT_WRITE);

	igt_assert(*ptr == 0xdeadbeef);

	nouveau_bo_ref(NULL, &nvbo);
	gem_munmap(ptr, BO_SIZE);
	gem_close(intel_fd, intel_handle);
}

/* test drm_intel_bo_map doesn't work properly,
   this tries to map the backing shmem fd, which doesn't exist
   for these objects */
__noreturn static void test_i915_import_cpu_mmap(void)
{
	uint32_t intel_handle;
	int prime_fd;
	struct nouveau_bo *nvbo;
	uint32_t *ptr;

	igt_skip("cpu mmap support for imported dma-bufs not yet implemented\n");

	igt_assert(nouveau_bo_new(ndev, NOUVEAU_BO_GART | NOUVEAU_BO_MAP,
				  0, BO_SIZE, NULL, &nvbo) == 0);
	igt_assert(nouveau_bo_set_prime(nvbo, &prime_fd) == 0);
	intel_handle = prime_fd_to_handle(intel_fd, prime_fd);
	close(prime_fd);

	igt_assert(nouveau_bo_map(nvbo, NOUVEAU_BO_RDWR, nclient) == 0);

	ptr = nvbo->map;
	*ptr = 0xdeadbeef;

	ptr = gem_mmap__cpu(intel_fd, intel_handle, 0, BO_SIZE, PROT_READ);

	igt_assert(*ptr == 0xdeadbeef);
	nouveau_bo_ref(NULL, &nvbo);
	gem_munmap(ptr, BO_SIZE);
	gem_close(intel_fd, intel_handle);
}

/* test drm_intel_bo_map_gtt works properly,
   this tries to map the backing shmem fd, which doesn't exist
   for these objects */
static void test_i915_import_gtt_mmap(void)
{
	uint32_t intel_handle;
	int prime_fd;
	struct nouveau_bo *nvbo;
	uint32_t *ptr;

	igt_assert(nouveau_bo_new(ndev, NOUVEAU_BO_GART | NOUVEAU_BO_MAP,
				  0, BO_SIZE, NULL, &nvbo) == 0);
	igt_assert(nouveau_bo_set_prime(nvbo, &prime_fd) == 0);

	intel_handle = prime_fd_to_handle(intel_fd, prime_fd);
	close(prime_fd);

	igt_assert(nouveau_bo_map(nvbo, NOUVEAU_BO_RDWR, nclient) == 0);

	ptr = nvbo->map;
	*ptr = 0xdeadbeef;
	*(ptr + 1) = 0xa55a55;

	ptr = gem_mmap__gtt(intel_fd, intel_handle, BO_SIZE, PROT_READ | PROT_WRITE);

	igt_assert(*ptr == 0xdeadbeef);
	nouveau_bo_ref(NULL, &nvbo);
	gem_munmap(ptr, BO_SIZE);
	gem_close(intel_fd, intel_handle);
}

/* test 7 - import from nouveau into intel, test pread/pwrite fail */
static void test_i915_import_pread_pwrite(void)
{
	uint32_t intel_handle;
	int prime_fd;
	struct nouveau_bo *nvbo;
	uint32_t *ptr;
	uint32_t buf[64];

	igt_assert(nouveau_bo_new(ndev, NOUVEAU_BO_GART | NOUVEAU_BO_MAP,
				  0, BO_SIZE, NULL, &nvbo) == 0);
	igt_assert(nouveau_bo_set_prime(nvbo, &prime_fd) == 0);

	intel_handle = prime_fd_to_handle(intel_fd, prime_fd);
	close(prime_fd);

	igt_assert(nouveau_bo_map(nvbo, NOUVEAU_BO_RDWR, nclient) == 0);

	ptr = nvbo->map;
	*ptr = 0xdeadbeef;

	gem_read(intel_fd, intel_handle, 0, buf, 256);
	igt_assert(buf[0] == 0xdeadbeef);
	buf[0] = 0xabcdef55;

	gem_write(intel_fd, intel_handle, 0, buf, 4);

	igt_assert(*ptr == 0xabcdef55);

	nouveau_bo_ref(NULL, &nvbo);
	gem_close(intel_fd, intel_handle);
}

static uint32_t create_bo(uint32_t val, int width, int height)
{
	uint32_t intel_handle;
	int size = width * height;
	uint32_t *ptr, *currptr;

	intel_handle = gem_create(intel_fd, 4*width*height);
	igt_assert(intel_handle);

        /* gtt map doesn't have a write parameter, so just keep the mapping
         * around (to avoid the set_domain with the gtt write domain set) and
         * manually tell the kernel when we start access the gtt. */
	ptr = gem_mmap__gtt(intel_fd, intel_handle, size, PROT_READ | PROT_WRITE);
	gem_set_domain(intel_fd, intel_handle, I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);

	currptr = ptr;
	while (size--)
		*currptr++ = val;

	gem_munmap(ptr, size);

	return intel_handle;
}

/* use intel hw to fill the BO with a blit from another BO,
   then readback from the nouveau bo, check value is correct */
static void test_i915_blt_fill_nv_read(void)
{
	uint32_t dst_handle, src_handle;
	int prime_fd;
	struct nouveau_bo *nvbo = NULL;
	uint32_t *ptr;
	struct intel_bb *ibb;
	struct intel_buf src, dst;
	int w = 256;
	int h = 4; /* for intel_bb_copy size requirement % 4096 */

	ibb = intel_bb_create(intel_fd, 4096);

	src_handle = create_bo(0xaa55aa55, w, h);
	dst_handle = gem_create(intel_fd, BO_SIZE);

	prime_fd = prime_handle_to_fd(intel_fd, dst_handle);

	igt_assert(nouveau_bo_prime_handle_ref(ndev, prime_fd, &nvbo) == 0);
	close(prime_fd);

	intel_buf_init_using_handle(bops, src_handle, &src, w, h, 32, 0,
				    I915_TILING_NONE, I915_COMPRESSION_NONE);
	intel_buf_init_using_handle(bops, dst_handle, &dst, w, 256, 32, 0,
				    I915_TILING_NONE, I915_COMPRESSION_NONE);
	intel_bb_copy_intel_buf(ibb, &dst, &src, w * h * 4);

	igt_assert(nouveau_bo_map(nvbo, NOUVEAU_BO_RDWR, nclient) == 0);

	ptr = nvbo->map;
	igt_assert(*ptr == 0xaa55aa55);
	nouveau_bo_ref(NULL, &nvbo);

	intel_buf_destroy(&src);
	intel_buf_destroy(&dst);
	intel_bb_destroy(ibb);
	gem_close(intel_fd, dst_handle);
	gem_close(intel_fd, src_handle);
}

/* test 8 use nouveau to do blit */

/* test 9 nouveau copy engine?? */

igt_main
{
	igt_fixture {
		igt_assert(find_and_open_devices() == 0);

		igt_require(nouveau_fd != -1);
		igt_require(intel_fd != -1);
		bops = buf_ops_create(intel_fd);

		/* set up nouveau bufmgr */
		igt_assert(nouveau_device_wrap(nouveau_fd, 0, &ndev) == 0);
		igt_assert(nouveau_client_new(ndev, &nclient) == 0);

	}

#define xtest(name) \
	igt_subtest(#name) \
		test_##name();

	xtest(i915_nv_sharing);
	xtest(nv_i915_sharing);
	xtest(nv_write_i915_cpu_mmap_read);
	xtest(nv_write_i915_gtt_mmap_read);
	xtest(i915_import_cpu_mmap);
	xtest(i915_import_gtt_mmap);
	xtest(i915_import_pread_pwrite);
	xtest(i915_blt_fill_nv_read);

	igt_fixture {
		nouveau_device_del(&ndev);

		buf_ops_destroy(bops);
		close(intel_fd);
		close(nouveau_fd);
	}
}
