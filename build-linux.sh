#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
: "${COMPILER:?Set COMPILER to the directory containing riscv64-unknown-linux-musl-g++}"
CXX="$COMPILER/riscv64-unknown-linux-musl-g++"
CC="$COMPILER/riscv64-unknown-linux-musl-gcc"
if [[ ! -x "$CXX" || ! -x "$CC" ]]; then
  echo "Missing cross-compiler in COMPILER=$COMPILER" >&2
  exit 1
fi
if [[ ! -f libs/opencv-mobile-4.10.0-licheerv-nano/lib/cmake/opencv4/OpenCVConfig.cmake ]]; then
  echo "Missing libs/opencv-mobile-4.10.0-licheerv-nano; see README.md" >&2
  exit 1
fi
cmake -S . -B build -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=riscv64 \
   -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" \
   -DOpenCV_DIR="$PWD/libs/opencv-mobile-4.10.0-licheerv-nano/lib/cmake/opencv4"
cmake --build build --parallel 2
file build/lichee_color_stream || true
