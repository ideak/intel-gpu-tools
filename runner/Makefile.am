
if BUILD_RUNNER

SUBDIRS = testdata

runnerlib = librunner.la
noinst_LTLIBRARIES = $(runnerlib)
librunner_la_SOURCES =	\
	settings.c	\
	job_list.c	\
	executor.c	\
	resultgen.c	\
	$(NULL)

bin_PROGRAMS =		\
	igt_runner	\
	igt_resume	\
	igt_results	\
	$(NULL)

LDADD = $(runnerlib) $(JSONC_LIBS) ../lib/libintel_tools.la

igt_runner_SOURCES = runner.c
igt_resume_SOURCES = resume.c
igt_results_SOURCES = results.c

AM_CFLAGS = $(JSONC_CFLAGS) \
	$(CWARNFLAGS) -Wno-unused-result $(DEBUG_CFLAGS) \
	-I$(srcdir)/.. \
	-I$(srcdir)/../lib \
	-D_GNU_SOURCE

TESTS = runner_test
check_PROGRAMS = runner_test
runner_test_SOURCES = runner_tests.c
runner_test_CFLAGS = -DTESTDATA_DIRECTORY=\"$(abs_builddir)/testdata\" \
	-I$(top_srcdir)/include/drm-uapi \
	$(DRM_CFLAGS) $(CAIRO_CFLAGS) $(LIBUDEV_CFLAGS) \
	-I$(srcdir)/.. \
	-I$(srcdir)/../lib \
	-D_GNU_SOURCE

endif
