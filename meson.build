project('ciggurat', 'c', version : '0.1.0', license : 'MIT',
  default_options : ['c_std=c11'])

cc = meson.get_compiler('c')

if get_option('buildtype').startswith('debug')
	# Define DEBUG if we are building with debug mode.
  add_project_arguments('-DDEBUG', language : 'c')
endif

ciggurat_inc = include_directories('.')

# Dependencies
ciggurat_deps = []

mylib_proj = subproject('mylib')
mylib_dep = mylib_proj.get_variable('mylib_dep')
ciggurat_deps += mylib_dep

# We currently have no dependencies, it's possible we may have some later.

# Define the source array which is filled by the meson.build in src/.
ciggurat_src = []
subdir('src')

install_headers('ciggurat.h')

ciggurat_lib = library('ciggurat', ciggurat_src, install : true,
	dependencies : ciggurat_deps,
  include_directories : ciggurat_inc,
  version : meson.project_version())

ciggurat_dep = declare_dependency(link_with : ciggurat_lib,
	dependencies : ciggurat_deps,
  include_directories : ciggurat_inc,
  version : meson.project_version())

pkg = import('pkgconfig')
pkg.generate(name : 'ciggurat', requires : ['ciggurat_lib'],
  description : 'Modern 2D game framework powered by ECS & Vulkan.',
  version : meson.project_version())

if get_option('enable-tests') == true
  subdir('tests')
endif
