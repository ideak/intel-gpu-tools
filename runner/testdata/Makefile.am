testdata_progs = no-subtests skippers successtest

noinst_PROGRAMS = $(testdata_progs)

test-list.txt: Makefile
	@echo TESTLIST > $@
	@echo ${testdata_progs} >> $@
	@echo END TESTLIST >> $@

noinst_DATA = test-list.txt

all-local: .gitignore
.gitignore: Makefile.am
	@echo "$(testdata_progs) test-list.txt /.gitignore" | sed 's/\s\+/\n/g' | sort > $@

CLEANFILES = test-list.txt .gitignore

AM_CFLAGS = $(CWARNFLAGS) -Wno-unused-result $(DEBUG_CFLAGS) \
	-I$(top_srcdir)/include/drm-uapi \
	-I$(srcdir)/../.. \
	-I$(srcdir)/../../lib \
	$(DRM_CFLAGS) $(CAIRO_CFLAGS) $(LIBUDEV_CFLAGS) \
	-D_GNU_SOURCE

LDADD = $(top_builddir)/lib/libintel_tools.la
