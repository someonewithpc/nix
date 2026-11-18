requires_private = []
foreach dep : deps_private_subproject
  requires_private += dep.name()
endforeach
requires_private += deps_private

requires_public = []
foreach dep : deps_public_subproject
  requires_public += dep.name()
endforeach
requires_public += deps_public

extra_pkg_config_variables = get_variable('extra_pkg_config_variables', {})
fs = import('fs')
plugin_c_api_enabled = get_variable('plugin_c_api_enabled', false)

# default_library=both also builds the static libraries that the
# `static-nix-libraries` option of `nix` links
build_both_libraries = plugin_c_api_enabled or get_option('default_library') == 'both'

if plugin_c_api_enabled
  this_libraries = get_variable('this_libraries')
else
  this_libraries = get_variable('this_library')
endif
if build_both_libraries
  this_library = this_libraries.get_shared_lib()
  this_static_library = this_libraries.get_static_lib()
else
  this_library = this_libraries
endif

extra_cflags = []
if not meson.project_name().endswith('-c')
  extra_cflags += [ '-std=c++23' ]
endif

whole_archive_dep_name = meson.project_name() + '-whole-archive'
installed_whole_archive_link_arg = ''
if build_both_libraries and host_machine.system() == 'darwin'
  installed_whole_archive_link_arg = '-Wl,-force_load,${libdir}/' + fs.name(
    this_static_library.full_path(),
  )
elif build_both_libraries and not host_machine.system().startswith('windows')
  installed_whole_archive_link_arg = '-Wl,--whole-archive,${libdir}/' + fs.name(
    this_static_library.full_path(),
  ) + ',--no-whole-archive'
endif

libraries_public = get_variable('libraries_public', [])

import('pkgconfig').generate(
  this_library,
  filebase : meson.project_name(),
  name : 'Nix',
  description : 'Nix Package Manager',
  extra_cflags : extra_cflags,
  requires : requires_public,
  requires_private : requires_private,
  libraries : libraries_public,
  libraries_private : libraries_private,
  variables : extra_pkg_config_variables,
)

whole_archive_external_deps = []
foreach dep : deps_public_no_pkgconfig + deps_private_no_pkgconfig
  if dep.type_name() != 'internal'
    whole_archive_external_deps += dep
  endif
endforeach

if installed_whole_archive_link_arg != ''
  import('pkgconfig').generate(
    filebase : whole_archive_dep_name,
    name : 'Nix whole-archive static dependency',
    description : 'Force-load static Nix C API archive into the host executable',
    extra_cflags : extra_cflags,
    # The executable links what the archive needs, private dependencies too
    requires : requires_public + requires_private,
    libraries : [ installed_whole_archive_link_arg ] + whole_archive_external_deps,
    variables : extra_pkg_config_variables,
  )
endif

meson.override_dependency(
  meson.project_name(),
  declare_dependency(
    include_directories : include_dirs,
    link_with : this_library,
    compile_args : [ '-std=c++23' ],
    dependencies : deps_public_subproject + deps_public + deps_public_no_pkgconfig,
    variables : extra_pkg_config_variables,
  ),
)

if installed_whole_archive_link_arg != ''
  meson.override_dependency(
    whole_archive_dep_name,
    declare_dependency(
      include_directories : include_dirs,
      link_whole : [ this_static_library ],
      compile_args : [ '-std=c++23' ],
      dependencies : deps_public_subproject + deps_public + deps_public_no_pkgconfig,
      variables : extra_pkg_config_variables,
    ),
  )
endif
