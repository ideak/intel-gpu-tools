project('igt-gpu-tools', 'c',
	version : '1.22',
        default_options: [
          'warning_level=2',
          'c_std=gnu11',
        ],
	license : 'MIT',
	meson_version : '>0.44.0')

cc = meson.get_compiler('c')

cc_args = [
	'-Wno-unused-parameter',
	'-Wno-sign-compare',
	'-Wno-missing-field-initializers',
	'-Wno-clobbered',
# Macros asserting on the range of their arguments triggers this.
	'-Wno-type-limits',
# igt_assert(0) in switch statements triggers a bunch of this.
	'-Wimplicit-fallthrough=0',
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
_build_audio = false
_audio_required = false
_build_chamelium = false
_chamelium_required = false
_build_docs = false
_docs_required = false
_build_tests = false
_tests_required = false

build_overlay = get_option('build_overlay')
overlay_backends = get_option('overlay_backends')
build_man = get_option('build_man')
with_valgrind = get_option('with_valgrind')
build_audio = get_option('build_audio')
build_chamelium = get_option('build_chamelium')
build_docs = get_option('build_docs')
build_tests = get_option('build_tests')
with_libdrm = get_option('with_libdrm')

_build_overlay = build_overlay != 'false'
_overlay_required = build_overlay == 'true'
_build_man = build_man != 'false'
_man_required = build_man == 'true'
_build_audio = build_audio != 'false'
_audio_required = build_audio == 'true'
_build_chamelium = build_chamelium != 'false'
_chamelium_required = build_chamelium == 'true'
_build_docs = build_docs != 'false'
_docs_required = build_docs == 'true'
_build_tests = build_tests != 'false'
_tests_required = build_tests == 'true'

build_info = []

inc = include_directories('include/drm-uapi', 'lib', '.')

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
libunwind = dependency('libunwind', required : true)

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

gsl = null_dep
alsa = null_dep
pixman = null_dep
if _build_audio or _build_chamelium
	gsl = dependency('gsl', required : _audio_required or _chamelium_required)
endif
if _build_audio
	alsa = dependency('alsa', required : _audio_required)
endif
if _build_chamelium
	pixman = dependency('pixman-1', required : _chamelium_required)
endif

audioinfo = 'No'
if _build_audio and alsa.found() and gsl.found()
	audioinfo = 'Yes'
else
	if _audio_required
		error('Cannot build audio test due to missing dependencies')
	endif
	_build_audio = false
endif
build_info += 'Build audio test: ' + audioinfo

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

chamelium = null_dep
chameliuminfo = 'No'
if _build_chamelium and pixman.found() and gsl.found() and xmlrpc.found() and xmlrpc_util.found() and xmlrpc_client.found()
	chamelium = declare_dependency(dependencies : [ pixman, xmlrpc,
							xmlrpc_util, xmlrpc_client])
	config.set('HAVE_CHAMELIUM', 1)
	chameliuminfo = 'Yes'
elif _chamelium_required
	error('Cannot build chamelium test due to missing dependencies')
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
mandir = get_option('mandir')
pkgconfigdir = join_paths(libdir, 'pkgconfig')

subdir('lib')
if _build_tests
	subdir('tests')
	build_info += 'Build tests: Yes'
else
	build_info += 'Build tests: No'
endif
subdir('benchmarks')
subdir('tools')
if libdrm_intel.found()
	subdir('assembler')
endif
subdir('overlay')
subdir('man')

docs_info = 'No'
if _build_docs
	if _build_tests
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
