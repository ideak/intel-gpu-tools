/* wierd use of API tests */

/* test1- export buffer from intel, import same fd twice into nouveau,
   check handles match
   test2 - export buffer from intel, import fd once, close fd, try import again
   fail if it succeeds
   test3 - export buffer from intel, import twice on nouveau, check handle is the same
   test4 - export handle twice from intel, import into nouveau twice, check handle is the same
*/

#include "igt.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "i915/gem_create.h"
#include "nouveau.h"

#define BO_SIZE (256*1024)

int intel_fd = -1, intel_fd2 = -1, nouveau_fd = -1, nouveau_fd2 = -1;
struct nouveau_device *ndev, *ndev2;
struct nouveau_client *nclient, *nclient2;

static void find_and_open_devices(void)
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
			igt_assert(intel_fd);
			intel_fd2 = open(path, O_RDWR);
			igt_assert(intel_fd2);
		} else if (venid == 0x10de) {
			nouveau_fd = open(path, O_RDWR);
			igt_assert(nouveau_fd);
			nouveau_fd2 = open(path, O_RDWR);
			igt_assert(nouveau_fd2);
		}
	}
}

static void test_i915_nv_import_twice(void)
{
	uint32_t intel_handle;
	int prime_fd;
	struct nouveau_bo *nvbo = NULL, *nvbo2 = NULL;

	intel_handle = gem_create(intel_fd, BO_SIZE);
	prime_fd = prime_handle_to_fd(intel_fd, intel_handle);

	igt_assert(nouveau_bo_prime_handle_ref(ndev, prime_fd, &nvbo) == 0);
	igt_assert(nouveau_bo_prime_handle_ref(ndev2, prime_fd, &nvbo2) == 0);
	close(prime_fd);

	nouveau_bo_ref(NULL, &nvbo2);
	nouveau_bo_ref(NULL, &nvbo);
	gem_close(intel_fd, intel_handle);
}

static void test_i915_nv_import_twice_check_flink_name(void)
{
	uint32_t intel_handle;
	int prime_fd;
	struct nouveau_bo *nvbo = NULL, *nvbo2 = NULL;
	uint32_t flink_name1, flink_name2;

	intel_handle = gem_create(intel_fd, BO_SIZE);
	prime_fd = prime_handle_to_fd(intel_fd, intel_handle);

	igt_assert(nouveau_bo_prime_handle_ref(ndev, prime_fd, &nvbo) == 0);
	igt_assert(nouveau_bo_prime_handle_ref(ndev2, prime_fd, &nvbo2) == 0);
	close(prime_fd);

	igt_assert(nouveau_bo_name_get(nvbo, &flink_name1) == 0);
	igt_assert(nouveau_bo_name_get(nvbo2, &flink_name2) == 0);

	igt_assert_eq_u32(flink_name1, flink_name2);

	nouveau_bo_ref(NULL, &nvbo2);
	nouveau_bo_ref(NULL, &nvbo);
	gem_close(intel_fd, intel_handle);
}

static void test_i915_nv_reimport_twice_check_flink_name(void)
{
	uint32_t intel_handle;
	int prime_fd;
	struct nouveau_bo *nvbo = NULL, *nvbo2 = NULL;
	uint32_t flink_name1, flink_name2;

	intel_handle = gem_create(intel_fd, BO_SIZE);
	prime_fd = prime_handle_to_fd(intel_fd, intel_handle);

	igt_assert(nouveau_bo_prime_handle_ref(ndev, prime_fd, &nvbo) == 0);

	/* create a new dma-buf */
	close(prime_fd);
	prime_fd = prime_handle_to_fd(intel_fd, intel_handle);

	igt_assert(nouveau_bo_prime_handle_ref(ndev2, prime_fd, &nvbo2) == 0);
	close(prime_fd);

	igt_assert(nouveau_bo_name_get(nvbo, &flink_name1) == 0);
	igt_assert(nouveau_bo_name_get(nvbo2, &flink_name2) == 0);

	igt_assert_eq_u32(flink_name1, flink_name2);

	nouveau_bo_ref(NULL, &nvbo2);
	nouveau_bo_ref(NULL, &nvbo);
	gem_close(intel_fd, intel_handle);
}

static void test_nv_i915_import_twice_check_flink_name(void)
{
	uint32_t intel_handle, intel_handle2;
	int prime_fd;
	struct nouveau_bo *nvbo = NULL;
	uint32_t flink_name1, flink_name2;

	igt_assert(nouveau_bo_new(ndev, NOUVEAU_BO_GART | NOUVEAU_BO_MAP,
				  0, BO_SIZE, NULL, &nvbo) == 0);

	igt_assert(nouveau_bo_set_prime(nvbo, &prime_fd) == 0);

	intel_handle = prime_fd_to_handle(intel_fd, prime_fd);
	intel_handle2 = prime_fd_to_handle(intel_fd2, prime_fd);
	close(prime_fd);

	flink_name1 = gem_flink(intel_fd, intel_handle);
	flink_name2 = gem_flink(intel_fd2, intel_handle2);

	igt_assert_eq_u32(flink_name1, flink_name2);

	nouveau_bo_ref(NULL, &nvbo);
	gem_close(intel_fd, intel_handle);
	gem_close(intel_fd2, intel_handle2);
}

static void test_nv_i915_reimport_twice_check_flink_name(void)
{
	uint32_t intel_handle, intel_handle2;
	int prime_fd;
	struct nouveau_bo *nvbo = NULL;
	uint32_t flink_name1, flink_name2;

	igt_assert(nouveau_bo_new(ndev, NOUVEAU_BO_GART | NOUVEAU_BO_MAP,
				  0, BO_SIZE, NULL, &nvbo) == 0);

	igt_assert(nouveau_bo_set_prime(nvbo, &prime_fd) == 0);

	intel_handle = prime_fd_to_handle(intel_fd, prime_fd);
	close(prime_fd);

	igt_assert(nouveau_bo_set_prime(nvbo, &prime_fd) == 0);

	intel_handle2 = prime_fd_to_handle(intel_fd2, prime_fd);
	close(prime_fd);

	flink_name1 = gem_flink(intel_fd, intel_handle);
	flink_name2 = gem_flink(intel_fd2, intel_handle2);

	igt_assert_eq_u32(flink_name1, flink_name2);

	nouveau_bo_ref(NULL, &nvbo);
	gem_close(intel_fd, intel_handle);
	gem_close(intel_fd2, intel_handle2);
}

static void test_i915_nv_import_vs_close(void)
{
	uint32_t intel_handle;
	int prime_fd;
	struct nouveau_bo *nvbo = NULL, *nvbo2 = NULL;

	intel_handle = gem_create(intel_fd, BO_SIZE);
	prime_fd = prime_handle_to_fd(intel_fd, intel_handle);

	igt_assert(nouveau_bo_prime_handle_ref(ndev, prime_fd, &nvbo) == 0);
	close(prime_fd);
	igt_assert(nouveau_bo_prime_handle_ref(ndev2, prime_fd, &nvbo2) < 0);

	nouveau_bo_ref(NULL, &nvbo2);
	nouveau_bo_ref(NULL, &nvbo);
	gem_close(intel_fd, intel_handle);
}

/* import handle twice on one driver */
static void test_i915_nv_double_import(void)
{
	uint32_t intel_handle;
	int prime_fd;
	struct nouveau_bo *nvbo = NULL, *nvbo2 = NULL;

	intel_handle = gem_create(intel_fd, BO_SIZE);
	prime_fd = prime_handle_to_fd(intel_fd, intel_handle);

	igt_assert(nouveau_bo_prime_handle_ref(ndev, prime_fd, &nvbo) == 0);
	igt_assert(nouveau_bo_prime_handle_ref(ndev, prime_fd, &nvbo2) == 0);
	close(prime_fd);

	igt_assert(nvbo->handle == nvbo2->handle);

	nouveau_bo_ref(NULL, &nvbo2);
	nouveau_bo_ref(NULL, &nvbo);
	gem_close(intel_fd, intel_handle);
}

/* export handle twice from one driver - import twice
   see if we get same object */
static void test_i915_nv_double_export(void)
{
	uint32_t intel_handle;
	int prime_fd, prime_fd2;
	struct nouveau_bo *nvbo = NULL, *nvbo2 = NULL;

	intel_handle = gem_create(intel_fd, BO_SIZE);
	prime_fd = prime_handle_to_fd(intel_fd, intel_handle);
	prime_fd2 = prime_handle_to_fd(intel_fd2, intel_handle);

	igt_assert(nouveau_bo_prime_handle_ref(ndev, prime_fd, &nvbo) == 0);
	close(prime_fd);
	igt_assert(nouveau_bo_prime_handle_ref(ndev, prime_fd2, &nvbo2) == 0);
	close(prime_fd2);

	igt_assert(nvbo->handle == nvbo2->handle);

	nouveau_bo_ref(NULL, &nvbo2);
	nouveau_bo_ref(NULL, &nvbo);

	gem_close(intel_fd, intel_handle);
}

/* export handle from intel driver - reimport to intel driver
   see if you get same object */
static void test_i915_self_import(void)
{
	uint32_t intel_handle, intel_handle2;
	int prime_fd;

	intel_handle = gem_create(intel_fd, BO_SIZE);
	prime_fd = prime_handle_to_fd(intel_fd, intel_handle);

	intel_handle2 = prime_fd_to_handle(intel_fd, prime_fd);
	close(prime_fd);

	igt_assert(intel_handle == intel_handle2);

	gem_close(intel_fd, intel_handle);
}

/* nouveau export reimport test */
static void test_nv_self_import(void)
{
	int prime_fd;
	struct nouveau_bo *nvbo, *nvbo2;

	igt_assert(nouveau_bo_new(ndev, NOUVEAU_BO_GART | NOUVEAU_BO_MAP,
				  0, BO_SIZE, NULL, &nvbo) == 0);
	igt_assert(nouveau_bo_set_prime(nvbo, &prime_fd) == 0);

	igt_assert(nouveau_bo_prime_handle_ref(ndev, prime_fd, &nvbo2) == 0);
	close(prime_fd);

	igt_assert(nvbo->handle == nvbo2->handle);
	nouveau_bo_ref(NULL, &nvbo);
	nouveau_bo_ref(NULL, &nvbo2);
}

/* export handle from intel driver - reimport to another intel driver bufmgr
   see if you get same object */
static void test_i915_self_import_to_different_fd(void)
{
	uint32_t intel_handle, intel_handle2;
	int prime_fd;

	intel_handle = gem_create(intel_fd, BO_SIZE);
	prime_fd = prime_handle_to_fd(intel_fd, intel_handle);

	intel_handle2 = prime_fd_to_handle(intel_fd2, prime_fd);
	close(prime_fd);

	gem_close(intel_fd, intel_handle);
	gem_close(intel_fd2, intel_handle2);
}

/* nouveau export reimport to other driver test */
static void test_nv_self_import_to_different_fd(void)
{
	int prime_fd;
	struct nouveau_bo *nvbo, *nvbo2;

	igt_assert(nouveau_bo_new(ndev, NOUVEAU_BO_GART | NOUVEAU_BO_MAP,
				  0, BO_SIZE, NULL, &nvbo) == 0);
	igt_assert(nouveau_bo_set_prime(nvbo, &prime_fd) == 0);

	igt_assert(nouveau_bo_prime_handle_ref(ndev2, prime_fd, &nvbo2) == 0);
	close(prime_fd);

	/* not sure what to test for, just make sure we don't explode */
	nouveau_bo_ref(NULL, &nvbo);
	nouveau_bo_ref(NULL, &nvbo2);
}

igt_main
{
	igt_fixture {
		find_and_open_devices();

		igt_require(nouveau_fd != -1);
		igt_require(nouveau_fd2 != -1);
		igt_require(intel_fd != -1);
		igt_require(intel_fd2 != -1);

		/* set up nouveau bufmgr */
		igt_assert(nouveau_device_wrap(nouveau_fd, 0, &ndev) >= 0);
		igt_assert(nouveau_client_new(ndev, &nclient) >= 0);

		/* set up nouveau bufmgr */
		igt_assert(nouveau_device_wrap(nouveau_fd2, 0, &ndev2) >= 0);

		igt_assert(nouveau_client_new(ndev2, &nclient2) >= 0);;
	}

#define xtest(name) \
	igt_subtest(#name) \
		test_##name();

	xtest(i915_nv_import_twice);
	xtest(i915_nv_import_twice_check_flink_name);
	xtest(i915_nv_reimport_twice_check_flink_name);
	xtest(nv_i915_import_twice_check_flink_name);
	xtest(nv_i915_reimport_twice_check_flink_name);
	xtest(i915_nv_import_vs_close);
	xtest(i915_nv_double_import);
	xtest(i915_nv_double_export);
	xtest(i915_self_import);
	xtest(nv_self_import);
	xtest(i915_self_import_to_different_fd);
	xtest(nv_self_import_to_different_fd);
	
	igt_fixture {
		nouveau_device_del(&ndev);

		close(intel_fd);
		close(intel_fd2);
		close(nouveau_fd);
		close(nouveau_fd2);
	}
}
