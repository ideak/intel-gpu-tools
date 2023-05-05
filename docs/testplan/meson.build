testplan_title = 'IGT test plans'

sphinx = find_program('sphinx-build', required: build_sphinx)
rst2html = find_program('rst2html-3', 'rst2html', required : false)
rst2pdf = find_program('rst2pdf', required: false)

stylesheet = join_paths(meson.current_source_dir(), 'testplan.css')

xe_test_config = join_paths(source_root, 'tests', 'xe', 'xe_test_config.json')
kms_test_config = join_paths(source_root, 'tests', 'kms_test_config.json')

check_testlist = []
if build_tests
	doc_dependencies = test_executables
	# Check if documentation matches the actual tests and tests can run
	if not meson.is_cross_build()
		build_info += 'Will Check if documentation is in sync with testlist'
		check_testlist = [ '--check-testlist', '--igt-build-path', build_root ]
	else
		warning('WARNING: Will not check if documentation is in sync with testlist')
	endif
else
	doc_dependencies = []
endif

test_dict = {
	      'xe_tests': { 'input': xe_test_config, 'extra_args': check_testlist },
	      'kms_tests': { 'input': kms_test_config, 'extra_args': [] }
	    }

foreach testplan, fields: test_dict
	rst = custom_target(testplan + '.rst',
			    build_by_default : true,
			    command : [ igt_doc_script, '--config', '@INPUT@', '--rest', '@OUTPUT@' ] + fields['extra_args'],
			    depends : doc_dependencies,
			    input : fields['input'],
			    output : testplan + '.rst'
			   )

	if rst2html.found()
		custom_target(testplan + '.html',
			      build_by_default : true,
			      command : [ rst2html, '--stylesheet=' + stylesheet, '--field-name-limit=0', '@INPUT@', '@OUTPUT@' ],
			      input : rst,
			      output : testplan + '.html'
			     )
	endif
endforeach

if sphinx.found()
	if gen_rst_index.found()
		sphinx_out_dir = meson.current_build_dir()+ '/indexed_html'

		index_rst = custom_target('index.rst',
					  build_by_default : true,
					  command : [ gen_rst_index, testplan_title, test_dict.keys(), meson.current_build_dir()],
					  input : rst,
					  output : 'index.rst'
					 )

		custom_target('index.html',
			      build_by_default : true,
			      command : [ 'sphinx-build', '-c', meson.current_source_dir(),
					  meson.current_build_dir(), sphinx_out_dir],
			      input : index_rst,
			      output : 'index.html'
			     )
	endif

	if rst2pdf.found()
		sphinx_out_pdf = meson.current_build_dir() + '/pdf'

		custom_target('xe_tests.pdf',
			      build_by_default : true,
			      command : [ 'sphinx-build', '-c', meson.current_source_dir(),
					  '-b', 'pdf',
					  '-D', 'version=' + meson.project_version(),
					  meson.current_build_dir(), sphinx_out_pdf],
			      input : index_rst,
			      output : 'xe_tests.pdf'
			     )
	endif
endif

build_info += 'Build simple html testplan documentation: @0@'.format(rst2html.found())
build_info += 'Build indexed html testplan documentation: @0@'.format(sphinx.found())
build_info += 'Build pdf testplan documentation: @0@'.format(sphinx.found() and rst2pdf.found())
