
amdgpu_progs = []
amdgpu_deps = test_deps
if libdrm_amdgpu.found()
	amdgpu_progs += [ 'amd_basic',
			  'amd_cs_nop',
			  'amd_prime',
			]
	amdgpu_deps += libdrm_amdgpu
endif

amdgpudir = join_paths(libexecdir, 'amdgpu')

if get_option('use_rpath')
	# Set up runpath for the test executables towards libigt.so.
	# The path should be relative to $ORIGIN so the library is
	# still found properly even if installed to a path other than
	# prefix.

	# libdir and libexecdir are pathnames relative to
	# prefix. meson enforces this.

	# Start from the executable
	amdgpu_rpathdir = '$ORIGIN'
	# Executables are installed in amdgpudir. Add a .. for each
	# directory name in it.
	foreach p : amdgpudir.split('/')
		amdgpu_rpathdir = join_paths(amdgpu_rpathdir, '..')
	endforeach
	# Add relative path to libdir
	amdgpu_rpathdir = join_paths(amdgpu_rpathdir, libdir)
else
	amdgpu_rpathdir = ''
endif

foreach prog : amdgpu_progs
	test_executables += executable(prog, prog + '.c',
				       dependencies : amdgpu_deps,
				       install_dir : amdgpudir,
				       install_rpath : amdgpu_rpathdir,
				       install : true)
	test_progs += join_paths('amdgpu', prog)
endforeach
