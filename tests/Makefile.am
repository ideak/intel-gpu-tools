include Makefile.sources

SUBDIRS = intel-ci

if HAVE_LIBDRM_AMDGPU
    TESTS_progs += $(AMDGPU_TESTS)
endif

if HAVE_LIBDRM_NOUVEAU
    TESTS_progs += $(NOUVEAU_TESTS)
endif

if HAVE_LIBDRM_VC4
    TESTS_progs += $(VC4_TESTS)
endif

if HAVE_CHAMELIUM
TESTS_progs += \
	kms_chamelium \
	$(NULL)
endif

if HAVE_AUDIO
TESTS_progs += \
	audio \
	$(NULL)
endif

TESTS_progs += testdisplay

if BUILD_TESTS
test-list.txt: Makefile.sources
	@echo TESTLIST > $@
	@echo ${kernel_tests} >> $@
	@echo END TESTLIST >> $@

test-list-full.txt: Makefile.sources
	@echo TESTLIST > $@
	@echo ${kernel_tests} ${extra_kernel_tests} >> $@
	@echo END TESTLIST >> $@


noinst_PROGRAMS = \
	$(HANG) \
	$(TESTS_testsuite) \
	$(NULL)

pkglibexec_PROGRAMS = \
	gem_stress \
	$(TESTS_progs) \
	$(TESTS_progs_X) \
	$(NULL)

# Piglit expects test-list*.txt to be in the same directory as the
# tests. pkglibexec_DATA is an invalid combo to autotools, so work
# around that by defining a new directory variable that is the same as
# libexec directory and install the .txt files there.
wrap_libexecdir = $(pkglibexecdir)
wrap_libexec_DATA = \
	test-list.txt \
	test-list-full.txt \
	$(NULL)

dist_pkglibexec_SCRIPTS = \
	$(TESTS_scripts) \
	$(scripts) \
	$(NULL)

dist_pkgdata_DATA = \
	$(IMAGES) \
	$(NULL)

all-local: .gitignore
.gitignore: Makefile.sources
	@echo "$(pkglibexec_PROGRAMS) $(HANG) test-list.txt test-list-full.txt /.gitignore" | sed 's/\s\+/\n/g' | sort > $@

EXTRA_PROGRAMS = $(HANG)
EXTRA_DIST = \
	$(common_files) \
	$(check_SCRIPTS) \
	generate_testlist.sh \
	meson.build \
	$(NULL)

CLEANFILES = $(EXTRA_PROGRAMS) test-list.txt test-list-full.txt .gitignore

AM_CFLAGS = $(DRM_CFLAGS) $(CWARNFLAGS) -Wno-unused-result $(DEBUG_CFLAGS)\
	-I$(srcdir)/.. \
	-I$(srcdir)/../lib \
	-include "$(srcdir)/../lib/check-ndebug.h" \
	-DIGT_SRCDIR=\""$(abs_srcdir)"\" \
	-DIGT_DATADIR=\""$(pkgdatadir)"\" \
	-D_GNU_SOURCE \
	$(LIBUNWIND_CFLAGS) $(WERROR_CFLAGS) \
	$(NULL)

LDADD = ../lib/libintel_tools.la $(XMLRPC_LIBS)

AM_CFLAGS += $(CAIRO_CFLAGS) $(LIBUDEV_CFLAGS)
AM_LDFLAGS = -Wl,--as-needed

drm_import_export_CFLAGS = $(AM_CFLAGS) $(THREAD_CFLAGS)
drm_import_export_LDADD = $(LDADD) -lpthread
gem_close_race_CFLAGS = $(AM_CFLAGS) $(THREAD_CFLAGS)
gem_close_race_LDADD = $(LDADD) -lpthread
gem_ctx_basic_CFLAGS = $(AM_CFLAGS) $(THREAD_CFLAGS)
gem_ctx_basic_LDADD = $(LDADD) -lpthread
gem_ctx_thrash_CFLAGS = $(AM_CFLAGS) $(THREAD_CFLAGS)
gem_ctx_thrash_LDADD = $(LDADD) -lpthread
gem_exec_parallel_CFLAGS = $(AM_CFLAGS) $(THREAD_CFLAGS)
gem_exec_parallel_LDADD = $(LDADD) -lpthread
gem_fence_thrash_CFLAGS = $(AM_CFLAGS) $(THREAD_CFLAGS)
gem_fence_thrash_LDADD = $(LDADD) -lpthread
gem_fence_upload_CFLAGS = $(AM_CFLAGS) $(THREAD_CFLAGS)
gem_fence_upload_LDADD = $(LDADD) -lpthread
gem_flink_race_CFLAGS = $(AM_CFLAGS) $(THREAD_CFLAGS)
gem_flink_race_LDADD = $(LDADD) -lpthread
gem_mmap_gtt_CFLAGS = $(AM_CFLAGS) $(THREAD_CFLAGS)
gem_mmap_gtt_LDADD = $(LDADD) -lpthread
gem_mmap_wc_CFLAGS = $(AM_CFLAGS) $(THREAD_CFLAGS)
gem_mmap_wc_LDADD = $(LDADD) -lpthread
gem_sync_CFLAGS = $(AM_CFLAGS) $(THREAD_CFLAGS)
gem_sync_LDADD = $(LDADD) -lpthread
gem_threaded_access_tiled_CFLAGS = $(AM_CFLAGS) $(THREAD_CFLAGS)
gem_threaded_access_tiled_LDADD = $(LDADD) -lpthread
gem_tiled_swapping_CFLAGS = $(AM_CFLAGS) $(THREAD_CFLAGS)
gem_tiled_swapping_LDADD = $(LDADD) -lpthread
prime_self_import_CFLAGS = $(AM_CFLAGS) $(THREAD_CFLAGS)
prime_self_import_LDADD = $(LDADD) -lpthread
gen7_forcewake_mt_CFLAGS = $(AM_CFLAGS) $(THREAD_CFLAGS)
gen7_forcewake_mt_LDADD = $(LDADD) -lpthread
gem_userptr_blits_CFLAGS = $(AM_CFLAGS) $(THREAD_CFLAGS)
gem_userptr_blits_LDADD = $(LDADD) -lpthread

gem_wait_LDADD = $(LDADD) -lrt
kms_flip_LDADD = $(LDADD) -lrt -lpthread
pm_rc6_residency_LDADD = $(LDADD) -lrt

prime_nv_test_CFLAGS = $(AM_CFLAGS) $(DRM_NOUVEAU_CFLAGS)
prime_nv_test_LDADD = $(LDADD) $(DRM_NOUVEAU_LIBS)
prime_nv_api_CFLAGS = $(AM_CFLAGS) $(DRM_NOUVEAU_CFLAGS)
prime_nv_api_LDADD = $(LDADD) $(DRM_NOUVEAU_LIBS)
prime_nv_pcopy_CFLAGS = $(AM_CFLAGS) $(DRM_NOUVEAU_CFLAGS)
prime_nv_pcopy_LDADD = $(LDADD) $(DRM_NOUVEAU_LIBS)
vc4_create_bo_CFLAGS = $(AM_CFLAGS) $(DRM_VC4_CFLAGS)
vc4_create_bo_LDADD = $(LDADD) $(DRM_VC4_LIBS)
vc4_lookup_fail_CFLAGS = $(AM_CFLAGS) $(DRM_VC4_CFLAGS)
vc4_lookup_fail_LDADD = $(LDADD) $(DRM_VC4_LIBS)
vc4_dmabuf_poll_CFLAGS = $(AM_CFLAGS) $(DRM_VC4_CFLAGS)
vc4_dmabuf_poll_LDADD = $(LDADD) $(DRM_VC4_LIBS)
vc4_wait_bo_CFLAGS = $(AM_CFLAGS) $(DRM_VC4_CFLAGS)
vc4_wait_bo_LDADD = $(LDADD) $(DRM_VC4_LIBS)
vc4_wait_seqno_CFLAGS = $(AM_CFLAGS) $(DRM_VC4_CFLAGS)
vc4_wait_seqno_LDADD = $(LDADD) $(DRM_VC4_LIBS)

kms_chamelium_CFLAGS = $(AM_CFLAGS) $(XMLRPC_CFLAGS) $(LIBUDEV_CFLAGS)
kms_chamelium_LDADD = $(LDADD) $(XMLRPC_LIBS) $(LIBUDEV_LIBS)

audio_CFLAGS = $(AM_CFLAGS) $(ALSA_CFLAGS)
audio_LDADD = $(LDADD) $(ALSA_LIBS)

amdgpu_amd_basic_CFLAGS = $(AM_CFLAGS) $(DRM_AMDGPU_CFLAGS)
amdgpu_amd_basic_LDADD = $(LDADD) $(DRM_AMDGPU_LIBS)
amdgpu_amd_cs_nop_CFLAGS = $(AM_CFLAGS) $(DRM_AMDGPU_CFLAGS)
amdgpu_amd_cs_nop_LDADD = $(LDADD) $(DRM_AMDGPU_LIBS)
amdgpu_amd_prime_CFLAGS = $(AM_CFLAGS) $(DRM_AMDGPU_CFLAGS)
amdgpu_amd_prime_LDADD = $(LDADD) $(DRM_AMDGPU_LIBS)

endif
