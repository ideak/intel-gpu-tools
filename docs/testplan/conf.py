# -*- coding: utf-8 -*-
# SPDX-License-Identifier: (GPL-2.0 OR MIT)

import os
import sphinx
import sys

# Get Sphinx version
major, minor, patch = sphinx.version_info[:3]

extensions = []

def which(program):
    import os
    def is_exe(fpath):
        return os.path.isfile(fpath) and os.access(fpath, os.X_OK)

    fpath, fname = os.path.split(program)
    if fpath:
        if is_exe(program):
            return program
    else:
        for path in os.environ["PATH"].split(os.pathsep):
            exe_file = os.path.join(path, program)
            if is_exe(exe_file):
                return exe_file

    return None

if which('rst2pdf'):
    extensions.append('rst2pdf.pdfbuilder')

source_suffix = '.rst'
master_doc = 'index'

project = 'IGT Test Tools'
copyright = 'Intel'
author = 'IGT developers'
language = 'en'

exclude_patterns = []
todo_include_todos = False

if major < 2 and minor < 6:
    html_use_smartypants = False
else:
    smartquotes = False

html_theme = "nature"

# body_max_width causes build error with nature theme on version < 1.7
if major < 2 and minor < 7:
    html_theme_options = {
        "sidebarwidth": "400px",
    }
else:
    html_theme_options = {
        "body_max_width": "1520px",
        "sidebarwidth": "400px",
    }

html_css_files = []
html_static_path = ['.']
html_copy_source = False

html_sidebars = { '**': ['searchbox.html', 'localtoc.html']}
htmlhelp_basename = 'IGT'

# rst2pdf
pdf_documents = [
    ('index', u'tests', u'IGT Xe Tests', u'IGT authors'),
]
