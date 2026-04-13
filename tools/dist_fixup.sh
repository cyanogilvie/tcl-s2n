#!/bin/sh
# Called by meson.add_dist_script() during `meson dist`.
#
#   MESON_DIST_ROOT    the staging area being assembled into the tarball
#   MESON_BUILD_ROOT   the meson build dir (with generated artifacts)
#   MESON_SOURCE_ROOT  this project's source tree

set -e

dist="$MESON_DIST_ROOT"
build="$MESON_BUILD_ROOT"

# We don't ship .html or .pdf builds of the docs in release tarballs.
# Defensive cleanup in case stale copies were ever re-committed.
rm -f "$dist/doc/s2n.html" "$dist/doc/s2n.pdf"

# Bake the freshly-generated manpage and README so consumers don't need
# pandoc installed.  These are built by `meson compile` (the doc
# alias_target via `build_by_default: true`) and copied here from the
# build dir into the staging area.  README.md overwrites the version
# git-archived from source; doc/s2n.n is added (it isn't tracked).
if [ -f "$build/doc/s2n.n" ]; then
    mkdir -p "$dist/doc"
    cp "$build/doc/s2n.n" "$dist/doc/s2n.n"
fi
if [ -f "$build/doc/README.md" ]; then
    cp "$build/doc/README.md" "$dist/README.md"
fi

# Bake the autotools configure script so consumers using the legacy
# `./configure && make` path don't need autoconf installed themselves.
( cd "$dist" && autoreconf -fi )

# Drop large dep test vectors, language bindings, and fuzz corpora.
# Without this the tarball is ~150 MB; after trimming it's ~64 MB.
if [ -f "$dist/tools/trimdist.tcl" ]; then
    "${TCLSH:-tclsh}" "$dist/tools/trimdist.tcl" "$dist"
fi

# .github dirs (top-level and inside dep submodules) aren't useful in a
# release tarball.
find "$dist" -type d -name .github -exec rm -rf {} +

# Replace symlinks with copies of their targets.  teabase-style projects
# use symlinks under generic/ (and elsewhere) pointing into teabase/, and
# tar tools on Windows can't always follow them.
find "$dist" -type l | while read -r link; do
    target=$(readlink -f "$link")
    if [ -e "$target" ]; then
        rm "$link"
        if [ -d "$target" ]; then
            cp -a "$target" "$link"
        else
            cp "$target" "$link"
        fi
    fi
done
