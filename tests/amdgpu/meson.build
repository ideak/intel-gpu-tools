
amdgpu_progs = []
amdgpu_deps = test_deps
if libdrm_amdgpu.found()
	amdgpu_progs += [ 'amd_basic',
			  'amd_cs_nop',
			  'amd_prime',
			]
	amdgpu_deps += libdrm_amdgpu
endif

foreach prog : amdgpu_progs
	test_executables += executable(prog, prog + '.c',
				       dependencies : amdgpu_deps,
				       install_dir : join_paths(libexecdir, 'amdgpu'),
				       install_rpath : rpathdir,
				       install : true)
	test_progs += join_paths('amdgpu', prog)
endforeach
