
vmwgfx_progs = [
	'vmw_tri'
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
