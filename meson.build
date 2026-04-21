project('s2n', 'c',
  version:          '0.8.0',
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
subdir('doc')

# `meson dist` fixup: bake configure, trim dep test vectors.
meson.add_dist_script('tools/dist_fixup.sh')
