vc4_progs = [
	'vc4_create_bo',
	'vc4_dmabuf_poll',
	'vc4_label_bo',
	'vc4_lookup_fail',
	'vc4_mmap',
	'vc4_perfmon',
	'vc4_purgeable_bo',
	'vc4_tiling',
	'vc4_wait_bo',
	'vc4_wait_seqno',
]

foreach prog : vc4_progs
	test_executables += executable(prog, prog + '.c',
				       dependencies : test_deps,
				       install_dir : vc4dir,
				       install_rpath : vc4_rpathdir,
				       install : true)
	test_list += join_paths('vc4', prog)
endforeach
