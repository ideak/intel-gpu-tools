project('igt-gpu-tools', 'c',
	version : '1.23',
        default_options: [
          'warning_level=2',
          'c_std=gnu11',
        ],
	license : 'MIT',
	meson_version : '>=0.46.0')

cc = meson.get_compiler('c')

cc_args = [
	'-Wbad-function-cast',
	'-Wdeclaration-after-statement',
	'-Wformat=2',
# igt_assert(0) in switch statements triggers a bunch of this.
	'-Wimplicit-fallthrough=0',
	'-Wlogical-op',
	'-Wmissing-declarations',
	'-Wmissing-format-attribute',
	'-Wmissing-noreturn',
	'-Wmissing-prototypes',
	'-Wnested-externs',
	'-Wold-style-definition',
	'-Wpointer-arith',
	'-Wredundant-decls',
	'-Wshadow',
	'-Wstrict-prototypes',
	'-Wuninitialized',
	'-Wunused',

	'-Wno-clobbered',
	'-Wno-maybe-uninitialized',
	'-Wno-missing-field-initializers',
	'-Wno-pointer-arith',
	'-Wno-sign-compare',
# Macros asserting on the range of their arguments triggers this.
	'-Wno-type-limits',
	'-Wno-unused-parameter',
	'-Wno-unused-result',

	'-Werror=address',
	'-Werror=array-bounds',
	'-Werror=implicit',
	'-Werror=init-self',
	'-Werror=int-to-pointer-cast',
	'-Werror=main',
	'-Werror=missing-braces',
	'-Werror=nonnull',
	'-Werror=pointer-to-int-cast',
	'-Werror=return-type',
	'-Werror=sequence-point',
	'-Werror=trigraphs',
	'-Werror=write-strings',
]

foreach cc_arg : cc_args
  if cc.has_argument(cc_arg)
    add_global_arguments(cc_arg, language : 'c')
  endif
endforeach

_build_overlay = false
_overlay_required = false
_build_man = false
_man_required = false
_build_chamelium = false
_chamelium_required = false
_build_docs = false
_docs_required = false
_build_tests = false
_tests_required = false
_build_runner = false
_runner_required = false

build_overlay = get_option('build_overlay')
overlay_backends = get_option('overlay_backends')
build_man = get_option('build_man')
with_valgrind = get_option('with_valgrind')
build_chamelium = get_option('build_chamelium')
build_docs = get_option('build_docs')
build_tests = get_option('build_tests')
with_libdrm = get_option('with_libdrm')
with_libunwind = get_option('with_libunwind')
build_runner = get_option('build_runner')

_build_overlay = build_overlay != 'false'
_overlay_required = build_overlay == 'true'
_build_man = build_man != 'false'
_man_required = build_man == 'true'
_build_chamelium = build_chamelium != 'false'
_chamelium_required = build_chamelium == 'true'
_build_docs = build_docs != 'false'
_docs_required = build_docs == 'true'
_build_tests = build_tests != 'false'
_tests_required = build_tests == 'true'
_build_runner = build_runner != 'false'
_runner_required = build_runner == 'true'

build_info = []

inc = include_directories('include/drm-uapi', 'lib', 'lib/stubs/syscalls', '.')

inc_for_gtkdoc = include_directories('lib')

config = configuration_data()

null_dep = dependency('', required : false)

libdrm_info = []
libdrm_intel = null_dep
libdrm_nouveau = null_dep
libdrm_amdgpu = null_dep

libdrm_version = '>=2.4.82'
libdrm = dependency('libdrm', version : libdrm_version)
if with_libdrm.contains('auto') or with_libdrm.contains('intel')
	libdrm_intel = dependency('libdrm_intel', version : libdrm_version, required : with_libdrm.contains('intel'))
	libdrm_info += 'intel'
endif
if with_libdrm.contains('auto') or with_libdrm.contains('nouveau')
	libdrm_nouveau = dependency('libdrm_nouveau', version : libdrm_version, required : with_libdrm.contains('nouveau'))
	libdrm_info += 'nouveau'
endif
if with_libdrm.contains('auto') or with_libdrm.contains('amdgpu')
	libdrm_amdgpu = dependency('libdrm_amdgpu', version : libdrm_version, required : with_libdrm.contains('amdgpu'))
	libdrm_info += 'amdgpu'
endif

build_info += 'With libdrm: ' + ','.join(libdrm_info)

pciaccess = dependency('pciaccess', version : '>=0.10')
libkmod = dependency('libkmod')
libprocps = dependency('libprocps', required : true)

libunwind = null_dep
libunwindinfo = 'No'
if with_libunwind != 'false'
	libunwind = dependency('libunwind', required : with_libunwind == 'true')
	if libunwind.found()
		libunwindinfo = 'Yes'
	endif
endif
build_info += 'With libunwind: ' + libunwindinfo

libdw = dependency('libdw', required : true)
pixman = dependency('pixman-1', required : true)

valgrind = null_dep
valgrindinfo = 'No'
if with_valgrind != 'false'
	valgrind = dependency('valgrind', required : with_valgrind == 'true')
	if valgrind.found()
		config.set('HAVE_VALGRIND', 1)
		valgrindinfo = 'Yes'
	endif
endif
build_info += 'Valgrind annotations: ' + valgrindinfo

cairo = dependency('cairo', version : '>1.12.0', required : true)
libudev = dependency('libudev', required : true)
glib = dependency('glib-2.0', required : true)

xmlrpc = dependency('xmlrpc', required : false)
xmlrpc_util = dependency('xmlrpc_util', required : false)
xmlrpc_client = dependency('xmlrpc_client', required : false)

xmlrpc_cmd = find_program('xmlrpc-c-config', required : false)
if not xmlrpc.found() and xmlrpc_cmd.found()
	libs_cmd = run_command(xmlrpc_cmd, 'client', '--libs')
	cflags_cmd = run_command(xmlrpc_cmd, 'client', '--cflags')

	if libs_cmd.returncode() == 0 and cflags_cmd.returncode() == 0
		xmlrpc = declare_dependency(compile_args: cflags_cmd.stdout().strip().split(),
					    link_args : libs_cmd.stdout().strip().split())
		xmlrpc_util = declare_dependency()
		xmlrpc_client = declare_dependency()
	endif
endif

gsl = null_dep
alsa = null_dep
chamelium = null_dep
chamelium_found = false # TODO: use a disabler object instead
chameliuminfo = 'No'
if _build_chamelium
	gsl = dependency('gsl', required : _chamelium_required)
	alsa = dependency('alsa', required : _chamelium_required)
	chamelium = declare_dependency(dependencies : [
		xmlrpc,
		xmlrpc_util,
		xmlrpc_client,
		gsl,
		alsa,
	], required : _chamelium_required)
	if xmlrpc.found() and xmlrpc_util.found() and xmlrpc_client.found() and gsl.found() and alsa.found()
		config.set('HAVE_CHAMELIUM', 1)
		chameliuminfo = 'Yes'
		chamelium_found = true
	endif
endif
build_info += 'Build Chamelium test: ' + chameliuminfo

pthreads = dependency('threads')
math = cc.find_library('m')
realtime = cc.find_library('rt')
dlsym = cc.find_library('dl')
zlib = cc.find_library('z')

if cc.has_header('linux/kd.h')
	config.set('HAVE_LINUX_KD_H', 1)
endif
if cc.has_header('sys/kd.h')
	config.set('HAVE_SYS_KD_H', 1)
endif
if cc.has_header('libgen.h')
	config.set('HAVE_LIBGEN_H', 1)
endif
if cc.has_header('sys/io.h')
	config.set('HAVE_SYS_IO_H', 1)
endif
if cc.has_header('cpuid.h')
	# FIXME: Do we need the example link test from configure.ac?
	config.set('HAVE_CPUID_H', 1)
endif

if cc.has_member('struct sysinfo', 'totalram',
		prefix : '#include <sys/sysinfo.h>')
	config.set('HAVE_STRUCT_SYSINFO_TOTALRAM', 1)
endif

have = cc.has_function('memfd_create', prefix : '''#include <sys/mman.h>''', args : '-D_GNU_SOURCE')
config.set10('HAVE_MEMFD_CREATE', have)

add_project_arguments('-D_GNU_SOURCE', language : 'c')
add_project_arguments('-include', 'config.h', language : 'c')

config.set('PACKAGE_NAME', meson.project_name())
config.set_quoted('PACKAGE_VERSION', meson.project_version())
config.set_quoted('PACKAGE', meson.project_name())
config.set('PACKAGE_STRING', meson.project_name() + ' ' + meson.project_version())
config.set_quoted('TARGET_CPU_PLATFORM', host_machine.cpu_family())

configure_file(output: 'config.h', install: false, configuration: config)

prefix = get_option('prefix')
bindir = get_option('bindir')
datadir = join_paths(get_option('datadir'), 'igt-gpu-tools')
includedir = get_option('includedir')
libdir = get_option('libdir')
libexecdir = join_paths(get_option('libexecdir'), 'igt-gpu-tools')
amdgpudir = join_paths(libexecdir, 'amdgpu')
mandir = get_option('mandir')
pkgconfigdir = join_paths(libdir, 'pkgconfig')

if get_option('use_rpath')
	# Set up runpath for the test executables towards libigt.so.
	# The path should be relative to $ORIGIN so the library is
	# still found properly even if installed to a path other than
	# prefix.

	# libdir and bindir are pathnames relative to prefix. meson
	# enforces this.

	# 1. Start from the executable.
	# 2. Executables are installed in certain dir. Add a .. for each
	#    directory name in it.
	# 3. Add relative path to libdir.

	bindir_rpathdir = '$ORIGIN'
	foreach p : bindir.split('/')
		bindir_rpathdir = join_paths(bindir_rpathdir, '..')
	endforeach
	bindir_rpathdir = join_paths(bindir_rpathdir, libdir)

	libexecdir_rpathdir = '$ORIGIN'
	foreach p : libexecdir.split('/')
		libexecdir_rpathdir = join_paths(libexecdir_rpathdir, '..')
	endforeach
	libexecdir_rpathdir = join_paths(libexecdir_rpathdir, libdir)

	amdgpudir_rpathdir = '$ORIGIN'
	foreach p : amdgpudir.split('/')
		amdgpudir_rpathdir = join_paths(amdgpudir_rpathdir, '..')
	endforeach
	amdgpudir_rpathdir = join_paths(amdgpudir_rpathdir, libdir)
else
	bindir_rpathdir = ''
	libexecdir_rpathdir = ''
	amdgpudir_rpathdir = ''
endif

subdir('lib')
if _build_tests
	subdir('tests')
	build_info += 'Build tests: Yes'
else
	build_info += 'Build tests: No'
endif
subdir('benchmarks')
subdir('tools')
subdir('runner')
if libdrm_intel.found()
	subdir('assembler')
endif
subdir('overlay')
subdir('man')

gtk_doc = dependency('gtk-doc', required : _docs_required)

docs_info = 'No'
if _build_docs
	if _build_tests and gtk_doc.found()
		subdir('docs')
		docs_info = 'Yes'
	elif _docs_required
		error('Documentation requires building tests')
	endif
endif
build_info += 'Build documentation: ' + docs_info

message('Build options')
message('=============')
foreach str : build_info
	message(str)
endforeach

if cairo.version().version_compare('<1.17.2')
	if pixman.version().version_compare('<0.36.0')
		warning('Pixman < 0.36.0 found, cannot test HDR formats')
	endif
	warning('Cairo < 1.17.2 found, cannot test HDR formats')
elif pixman.version().version_compare('<0.36.0')
	# Cairo 1.17.2 requires 0.36.0 to compile, but somehow it went missing?
	error('Cairo with floating point support found, but pixman version too old')
endif
