IGT_LIB_PATH := $(builddir)
GPU_TOOLS_PATH := $(top_srcdir)

i915_perf_hardware =		\
	hsw			\
	bdw chv			\
	sklgt2 sklgt3 sklgt4	\
	kblgt2 kblgt3		\
	cflgt2 cflgt3		\
	bxt glk			\
	cnl			\
	icl ehl			\
	tgl

i915_perf_xml_files_ = $(addprefix i915/perf-configs/oa-,$(i915_perf_hardware))
i915_perf_xml_files = $(addsuffix .xml,$(i915_perf_xml_files_))

$(builddir)/i915/i915_perf_metrics.h: i915/perf-configs/perf-codegen.py $(i915_perf_xml_files)
	$(PYTHON) i915/perf-configs/perf-codegen.py --code $(builddir)/i915/i915_perf_metrics.c --header $(builddir)/i915/i915_perf_metrics.h $(i915_perf_xml_files)

$(builddir)/i915/i915_perf_metrics.c: $(builddir)/i915/i915_perf_metrics.h

include Makefile.sources

libintel_tools_la_SOURCES = $(lib_source_list)

libigt_perf_la_SOURCES = \
	igt_perf.c	 \
	igt_perf.h

libi915_perf_la_SOURCES = $(i915_perf_sources)
libi915_perf_HEADERS =	\
	igt_list.h	\
	i915/perf.h
libi915_perfdir = $(includedir)/i915-perf

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = i915-perf.pc

lib_LTLIBRARIES = libi915_perf.la

noinst_LTLIBRARIES = libintel_tools.la libigt_perf.la
noinst_HEADERS = check-ndebug.h

if !HAVE_LIBDRM_INTEL
    libintel_tools_la_SOURCES += 	\
        stubs/drm/intel_bufmgr.c	\
        stubs/drm/intel_bufmgr.h
endif

if HAVE_CHAMELIUM
lib_source_list +=	 	\
	igt_chamelium.h		\
	igt_chamelium.c		\
	igt_chamelium_stream.h	\
	igt_chamelium_stream.c	\
	$(NULL)
endif

if HAVE_GSL
lib_source_list +=		\
	igt_frame.c		\
	igt_frame.h		\
	igt_audio.c		\
	igt_audio.h		\
	$(NULL)
endif

if HAVE_ALSA
lib_source_list +=		\
	igt_alsa.c		\
	igt_alsa.h		\
	$(NULL)
endif

AM_CPPFLAGS = \
	-I$(top_srcdir)/include/drm-uapi \
	-I$(top_srcdir) \
	-I$(top_srcdir)/lib/stubs/syscalls

AM_CFLAGS = \
	    $(CWARNFLAGS) \
	    $(DRM_CFLAGS) \
	    $(PCIACCESS_CFLAGS) \
	    $(LIBUNWIND_CFLAGS) \
	    $(LIBDW_CFLAGS) \
	    $(GSL_CFLAGS) \
	    $(KMOD_CFLAGS) \
	    $(PROCPS_CFLAGS) \
	    $(DEBUG_CFLAGS) \
	    $(XMLRPC_CFLAGS) \
	    $(LIBUDEV_CFLAGS) \
	    $(PIXMAN_CFLAGS) \
	    $(GLIB_CFLAGS) \
	    $(VALGRIND_CFLAGS) \
	    -DIGT_SRCDIR=\""$(abs_top_srcdir)/tests"\" \
	    -DIGT_DATADIR=\""$(pkgdatadir)"\" \
	    -D_GNU_SOURCE \
	    -DIGT_LOG_DOMAIN=\""$(subst _,-,$*)"\" \
	    -pthread

AM_CFLAGS += $(CAIRO_CFLAGS)

libintel_tools_la_LIBADD = \
	$(DRM_LIBS) \
	$(PCIACCESS_LIBS) \
	$(PROCPS_LIBS) \
	$(GSL_LIBS) \
	$(KMOD_LIBS) \
	$(CAIRO_LIBS) \
	$(LIBUDEV_LIBS) \
	$(LIBUNWIND_LIBS) \
	$(LIBDW_LIBS) \
	$(TIMER_LIBS) \
	$(XMLRPC_LIBS) \
	$(LIBUDEV_LIBS) \
	$(PIXMAN_LIBS) \
	$(GLIB_LIBS) \
	libigt_perf.la \
	-lm
