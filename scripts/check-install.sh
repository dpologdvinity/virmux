#!/usr/bin/env bash
#
# scripts/check-install.sh - Verify an installed virmux finds its interceptor.
#
# Installs virmux into a throwaway prefix using meson's default libdir (on
# Debian/Ubuntu that is lib/<multiarch-triplet>), then runs the installed
# binary from / so no source tree or build directory is reachable, and checks
# that sandboxed commands get the installed libvirmux_intercept.so preloaded.
# Without it, reported memory (/proc/meminfo) and sysconf() core counts are
# not changed.
#
# Usage: scripts/check-install.sh [BUILD_DIR]   (default: build-install-check)

set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
build_dir=${1:-build-install-check}
prefix=$(mktemp -d "${TMPDIR:-/tmp}/virmux-prefix.XXXXXX")
trap 'rm -rf -- "$prefix"' EXIT

cd "$repo_root"
setup_args=(--buildtype=release --prefix="$prefix")
# Each run uses a fresh prefix, so an existing build dir must be wiped. Only
# pass --wipe then: meson < 1.0 (Ubuntu 22.04) rejects it for a new directory.
if [[ -d "$build_dir/meson-private" ]]; then
  setup_args+=(--wipe)
fi
meson setup "$build_dir" "${setup_args[@]}" >/dev/null
ninja -C "$build_dir" >/dev/null
meson install -C "$build_dir" --quiet

lib=$(find "$prefix" -name libvirmux_intercept.so -print -quit)
if [[ -z "$lib" ]]; then
  echo "FAIL: libvirmux_intercept.so was not installed under the prefix" >&2
  exit 1
fi

preload=$(cd / && "$prefix/bin/virmux" run -- printenv LD_PRELOAD 2>/dev/null || true)
if [[ "$preload" != *"$lib"* ]]; then
  echo "FAIL: installed virmux did not preload $lib (LD_PRELOAD='$preload')" >&2
  exit 1
fi
echo "OK: installed virmux preloads $lib"
