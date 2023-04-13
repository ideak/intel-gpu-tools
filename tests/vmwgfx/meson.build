
vmwgfx_progs = [
	'vmw_tri',
	'vmw_execution_buffer',
	'vmw_surface_copy',
	'vmw_mob_stress',
	'vmw_ref_count'
]
vmwgfx_deps = test_deps

foreach prog : vmwgfx_progs
	test_executables += executable(prog, prog + '.c',
				       dependencies : vmwgfx_deps,
				       install_dir : vmwgfxdir,
				       install_rpath : vmwgfx_rpathdir,
				       install : true)
	test_list += join_paths('vmwgfx', prog)
endforeach
