v3d_progs = [
	'v3d_create_bo',
	'v3d_get_bo_offset',
	'v3d_get_param',
	'v3d_mmap',
	'v3d_perfmon',
]

foreach prog : v3d_progs
	test_executables += executable(prog, prog + '.c',
				       dependencies : test_deps,
				       install_dir : v3ddir,
				       install_rpath : v3d_rpathdir,
				       install : true)
	test_list += join_paths('v3d', prog)
endforeach
