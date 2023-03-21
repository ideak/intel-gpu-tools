scripts = [
	'code_cov_capture',
	'code_cov_gather_on_build',
	'code_cov_gather_on_test',
	'code_cov_gen_report',
	'code_cov_parse_info',
]

if build_tests
	foreach prog : scripts
		install_data(prog, install_dir : bindir, install_mode : 'r-xr-xr-x')
	endforeach
endif

igt_doc_script = find_program('igt_doc.py', required : build_testplan)
gen_rst_index = find_program('gen_rst_index', required : build_sphinx)
