pandoc = find_program('pandoc', native: true, required: false)

if pandoc.found()
  doc_md = configure_file(
    input:          's2n.md.in',
    output:         '@BASENAME@',
    configuration:  teaconf,
  )

  doc_targets = []

  doc_targets += custom_target('manpage',
    input:    doc_md,
    output:   meson.project_name() + '.n',
    command:  [pandoc, '--standalone', '--from', 'markdown', '--to', 'man',
      '@INPUT@', '--output', '@OUTPUT@'],
    install_dir:      tcl_dep.get_variable('prefix') / 'share' / 'man' / 'mann',
    build_by_default: true,
    install:          true,
  )

  # Also build README.md by default so dist_fixup.sh can copy a freshly
  # generated copy into the dist tarball.  install: writes back into the
  # source tree only when explicitly requested (`meson install --tags source`)
  # so that committing a fresh README.md is an opt-in step.
  doc_targets += custom_target('readme',
    input:    doc_md,
    output:   'README.md',
    command:  [pandoc, '--from', 'markdown', '--to', 'gfm',
      '@INPUT@', '--output', '@OUTPUT@'],
    install_dir:      meson.project_source_root(),
    install_tag:      'source',
    build_by_default: true,
  )

  alias_target('doc', doc_targets)
endif
