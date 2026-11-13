#!/usr/bin/env bash
# Build the patched Sipeed test_mmf using a previously configured SG2002 SDK.
# Usage: bash native-sdk/build-native-rtsp.sh /path/to/LicheeRV-Nano-Build
set -Eeuo pipefail

REPO_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
SDK_ROOT="$(cd -- "${1:?Usage: bash native-sdk/build-native-rtsp.sh /path/to/configured/LicheeRV-Nano-Build}" && pwd)"
TARGET_DIR="$SDK_ROOT/middleware/v2/sample/test_mmf"
PARAM="$SDK_ROOT/middleware/v2/Makefile.param"
OUT="$REPO_DIR/native-sdk/out"

if [[ ! -f "$SDK_ROOT/build/.config" ]]; then
  echo >&2 "ERROR: $SDK_ROOT/build/.config is missing."
  echo >&2 "First configure the official SDK: source build/cvisetup.sh; defconfig cv1812cp_licheerv_nano_sd"
  exit 1
fi
if [[ ! -f "$PARAM" || ! -f "$TARGET_DIR/Makefile" ]]; then
  echo >&2 "ERROR: expected official Sipeed middleware test_mmf sources not found in $SDK_ROOT"
  exit 1
fi
for lib in "$SDK_ROOT/middleware/v2/lib/libvpu.a" "$SDK_ROOT/middleware/v2/lib/libsys.a" "$TARGET_DIR/libmaix_mmf.a"; do
  if [[ ! -f "$lib" ]]; then
    echo >&2 "ERROR: required native library missing: $lib"
    echo >&2 "Prepare/build official SDK middleware before compiling this patched sample."
    exit 1
  fi
done

echo "[native-rtsp] SDK: $SDK_ROOT"
python3 "$REPO_DIR/native-sdk/fix_mmf_rtsp.py" "$SDK_ROOT"

# Uses target CC/CXX, sysroot and ISA flags exported by the SDK setup.
cd "$SDK_ROOT"
# shellcheck disable=SC1091
source build/cvisetup.sh
cd "$TARGET_DIR"
make clean PARAM_FILE="$PARAM"
make -j"${JOBS:-2}" PARAM_FILE="$PARAM"

if [[ ! -s "$TARGET_DIR/test_mmf" ]]; then
  echo >&2 "ERROR: target test_mmf not produced"
  exit 1
fi
mkdir -p "$OUT"
cp -f "$TARGET_DIR/test_mmf" "$OUT/test_mmf_fixed"
file "$OUT/test_mmf_fixed"
if ! file "$OUT/test_mmf_fixed" | grep -Eqi 'RISC-V|RISC.V'; then
  echo >&2 "ERROR: binary is not built for RISC-V. Check SDK toolchain and .config."
  exit 1
fi
echo "[native-rtsp] Ready: $OUT/test_mmf_fixed"
echo "[native-rtsp] Copy to board; do NOT replace /mnt/system/usr/bin/test_mmf."
