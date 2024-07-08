#!/bin/bash
set -x
build_dir=$1
prefix=$2

mkdir -p "$prefix/bin"

echo "Installing Cider to ${DESTDIR}..."
echo "Copying compiler binary from $build_dir $prefix/bin"

cp "$build_dir/cider" "$prefix/bin"
cp cider-tags "$prefix/bin"

mkdir -p "$prefix/share/lib"
mkdir -p "$prefix/share/runtime"
find runtime -regex '.*\.c$'      -exec cp '{}' "$prefix/share/runtime" \;
find lib -regex '.*lib/.*\.cider$' -exec cp '{}' "$prefix/share/lib" \;
mkdir -p "$prefix/share/man/man1"
cp cider.1 "$prefix/share/man/man1"
