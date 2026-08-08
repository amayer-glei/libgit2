#!/bin/sh
# Build gut for Linux/macOS (static libgit2, self-contained binary).
set -e

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
build_dir="$script_dir/build-unix"

cmake -S "$script_dir" -B "$build_dir" -DCMAKE_BUILD_TYPE=Release
cmake --build "$build_dir" --target gut --parallel

echo "Built: $build_dir/gut"
