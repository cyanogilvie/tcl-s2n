# Build version-pinned interned static dependencies via the teabase
# interned_deps.py driver.  cmake's standard install layout (lib/ /
# include/ / cmake/) lives under prefix/, and the .a files we actually
# need to link are then copied to the flat custom_target output names
# meson requires for build-tracking.

py = import('python').find_installation()

interned_prefix = meson.current_build_dir() / 'prefix'
interned_build  = meson.current_build_dir() / 'build'

interned = custom_target('interned-deps',
  output: [
    'libs2n.a',
    'libcrypto.a',
    'interned-deps.stamp',
  ],
  command: [py,
    meson.project_source_root() / 'teabase/tools/interned_deps.py',
    '--manifest',    meson.project_source_root() / 'tools/interned_deps.json',
    '--source-root', meson.project_source_root(),
    '--prefix',      interned_prefix,
    '--build',       interned_build,
    '--copy-artifact', 'lib/libs2n.a:'    + meson.current_build_dir() / 'libs2n.a',
    '--copy-artifact', 'lib/libcrypto.a:' + meson.current_build_dir() / 'libcrypto.a',
    '--stamp',       meson.current_build_dir() / 'interned-deps.stamp',
  ],
  # ninja re-fires this target whenever the manifest or the driver
  # script changes.  Submodule SHA bumps in deps/ are NOT automatically
  # detected — run `meson compile -C <build> deps-clean` to force a
  # rebuild after bumping a submodule.
  depend_files: files(
    meson.project_source_root() / 'tools/interned_deps.json',
    meson.project_source_root() / 'teabase/tools/interned_deps.py',
  ),
  console:          true,
  build_by_default: true,
)

interned_deps_dep = declare_dependency(
  sources:      interned,
  compile_args: ['-I' + interned_prefix / 'include'],
  link_args: [
    interned[0].full_path(),
    interned[1].full_path(),
  ],
)

# Convenience wipe target.  Removes the install prefix, the per-dep
# build trees, the stamp, and the flat .a copies.
run_target('deps-clean',
  command: [py, '-c',
    'import os, shutil, sys\n' +
    'for p in sys.argv[1:]:\n' +
    '    if os.path.isdir(p): shutil.rmtree(p, ignore_errors=True)\n' +
    '    else:\n' +
    '        try: os.remove(p)\n' +
    '        except FileNotFoundError: pass',
    interned_prefix,
    interned_build,
    meson.current_build_dir() / 'interned-deps.stamp',
    meson.current_build_dir() / 'libs2n.a',
    meson.current_build_dir() / 'libcrypto.a',
  ],
)
