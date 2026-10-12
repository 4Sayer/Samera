#!/bin/sh
set -eu
binary=${1:-build/samera}
if [ ! -x "$binary" ]; then
    printf 'Executable not found: %s\n' "$binary" >&2
    exit 1
fi
check_dir=$(mktemp -d "${TMPDIR:-/tmp}/samera-check.XXXXXXXX")
trap 'rm -rf -- "$check_dir"' EXIT HUP INT TERM
export XDG_CONFIG_HOME="$check_dir/config"
export XDG_CACHE_HOME="$check_dir/cache"
export XDG_DATA_HOME="$check_dir/data"
export XDG_RUNTIME_DIR="$check_dir/runtime"
export SAMERA_TEST_DIR="$check_dir/captures"
export QT_QPA_PLATFORM=offscreen
mkdir -p "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"
"$binary" --test-source --self-test
"$binary" --test-source --ui-copy-check
