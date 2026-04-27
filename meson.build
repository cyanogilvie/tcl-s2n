project('s2n', 'c',
  version:          '0.8.2',
  default_options:  ['c_std=c17', 'warning_level=2', 'werror=true'],
  meson_version:    '>=1.3.0',
)

cc = meson.get_compiler('c')
fs = import('fs')
add_project_arguments('-DHAVE_CONFIG_H',   language: 'c')

if get_option('debug')
  add_project_arguments(cc.get_supported_arguments(['-ggdb3']), language: 'c')
endif

# clogs's keyword-arg macros expand to designated-initializer overrides.
add_project_arguments(cc.get_supported_arguments([
  '-Wno-missing-field-initializers',
  '-Wno-override-init',
]), language: 'c')

conf = configuration_data()

conf.set10('UNLOAD', get_option('unload'))

deps = []
inc = [include_directories('generic')]

pkg_sources = files(
  'generic/s2n.c',
)

tea_want_shims = ['tip445']
tea_want_defer = true

# Teabase init: provides pkg_dir, tclsh, tcl_dep, teaconf
#subdir('teabase/init')

# Interned static deps (libs2n.a + libcrypto.a) built from the
# version-pinned submodules under deps/.  Exports interned_deps_dep.
subdir('interned_deps')
deps += interned_deps_dep

# clogs is a git submodule at subprojects/clogs with its own meson
# build.  Forced static so it ends up baked into our .so.
deps += dependency('clogs',
  fallback:        ['clogs', 'clogs_dep'],
  default_options: ['default_library=static'],
)

subdir('teabase')

# pkg-config metadata so downstream consumers can locate this install
# uniformly. See parse_args/meson.build for the same pattern.
pc_conf = configuration_data()
pc_conf.set('PACKAGE_NAME',     meson.project_name())
pc_conf.set('PACKAGE_VERSION',  meson.project_version())
pc_conf.set('PKG_DIR_BASENAME', meson.project_name() + '-' + meson.project_version())
configure_file(
  input:          'tcl-' + meson.project_name() + '.pc.in',
  output:         'tcl-' + meson.project_name() + '.pc',
  configuration:  pc_conf,
  install:        not meson.is_subproject(),
  install_dir:    tcl_dep.get_variable('libdir') / 'pkgconfig',
)

s2n_dep = declare_dependency(
  variables: {
    'tcl_pkg_path': fs.parent(pkg_so.full_path()),
    'version':      meson.project_version(),
  },
)
meson.override_dependency(meson.project_name(), s2n_dep)

subdir('doc')

# `meson dist` fixup: bake configure, trim dep test vectors.
meson.add_dist_script('tools/dist_fixup.sh')
