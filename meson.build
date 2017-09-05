project('IGT gpu tests', 'c',
	version : '1.19',
        default_options: [
          'warning_level=2',
          'c_std=gnu99',
        ],
	license : 'MIT')

cc = meson.get_compiler('c')

add_global_arguments('-Wno-unused-parameter', language: 'c')
add_global_arguments('-Wno-sign-compare', language: 'c')
add_global_arguments('-Wno-missing-field-initializers', language: 'c')
add_global_arguments('-Wno-clobbered', language: 'c')

# Macros asserting on the range of their arguments triggers this.
add_global_arguments('-Wno-type-limits', language: 'c')

# igt_assert(0) in switch statements triggers a bunch of this.
add_global_arguments('-Wimplicit-fallthrough=0', language: 'c')

inc = include_directories('lib', '.')

config_h = configuration_data()

libdrm = dependency('libdrm', version : '>=2.4.82')
libdrm_intel = dependency('libdrm_intel', required : false)
libdrm_vc4 = dependency('libdrm_vc4', required : false)
libdrm_nouveau = dependency('libdrm_nouveau', required : false)
libdrm_amdgpu = dependency('libdrm_amdgpu', required : false)

pciaccess = dependency('pciaccess', version : '>=0.10')
libkmod = dependency('libkmod')
libprocps = dependency('libprocps', required : false)
if libprocps.found()
	config_h.set('HAVE_PROCPS', 1)
endif

valgrind = dependency('valgrind', required : false)
if valgrind.found()
	config_h.set('HAVE_VALGRIND', 1)
endif

cairo = dependency('cairo', version : '>1.12.0', required : false)

libudev = dependency('libudev', required : false)
if libudev.found()
	config_h.set('HAVE_UDEV', 1)
endif

glib = dependency('glib-2.0', required : false)
if glib.found()
	config_h.set('HAVE_GLIB', 1)
endif

libunwind = dependency('libunwind')
gsl = dependency('gsl', required : false)
alsa = dependency('alsa', required : false)

pixman = dependency('pixman-1', required : false)
xmlrpc = dependency('xmlrpc', required : false)
xmlrpc_util = dependency('xmlrpc_util', required : false)
xmlrpc_client = dependency('xmlrpc_client', required : false)

if pixman.found() and xmlrpc.found() and xmlrpc_util.found() and xmlrpc_client.found()
	chamelium = declare_dependency(dependencies : [ pixman, xmlrpc,
	  xmlrpc_util, xmlrpc_client])
else
	chamelium = dependency('', required: false)
endif

pthreads = dependency('threads')
math = cc.find_library('m')
realtime = cc.find_library('rt')
dlsym = cc.find_library('dl')
zlib = cc.find_library('z')

if cc.has_header('linux/kd.h')
	config_h.set('HAVE_LINUX_KD_H', 1)
endif
if cc.has_header('sys/kd.h')
	config_h.set('HAVE_SYS_KD_H', 1)
endif
if cc.has_header('libgen.h')
	config_h.set('HAVE_LIBGEN_H', 1)
endif
if cc.has_header('sys/io.h')
	config_h.set('HAVE_SYS_IO_H', 1)
endif
if cc.has_header('cpuid.h')
	# FIXME: Do we need the example link test from configure.ac?
	config_h.set('HAVE_CPUID_H', 1)
endif

if cc.has_member('struct sysinfo', 'totalram',
		prefix : '#include <sys/sysinfo.h>')
	config_h.set('HAVE_STRUCT_SYSINFO_TOTALRAM', 1)
endif

add_project_arguments('-D_GNU_SOURCE', language : 'c')
add_project_arguments('-include', 'config.h', language : 'c')

config_h.set_quoted('PACKAGE_VERSION', meson.project_version())
config_h.set_quoted('PACKAGE', meson.project_name())
config_h.set_quoted('TARGET_CPU_PLATFORM', host_machine.cpu_family())

configure_file(output: 'config.h', install: false, configuration: config_h)

subdir('lib')
subdir('tests')
subdir('benchmarks')
subdir('tools')
if libdrm_intel.found()
	subdir('assembler')
	subdir('overlay')
endif
