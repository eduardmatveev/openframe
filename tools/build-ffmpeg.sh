#!/usr/bin/env bash
# Run in MSYS2 with the VS 2022 x64 developer environment in PATH.
set -euo pipefail
if [[ $# -ne 3 ]]; then
    echo 'Usage: build-ffmpeg.sh SOURCE_DIRECTORY BUILD_DIRECTORY INSTALL_DIRECTORY' >&2
    exit 2
fi
source_dir=$(cd "$1" && pwd)
mkdir -p "$2" "$3"
build_dir=$(cd "$2" && pwd)
install_dir=$(cd "$3" && pwd)
cd "$build_dir"
"$source_dir/configure" \
    --prefix="$install_dir" --toolchain=msvc --arch=x86_64 \
    --enable-shared --disable-static \
    --disable-network --disable-protocols --enable-protocol=file,pipe \
    --disable-programs --disable-doc --disable-debug \
    --disable-avdevice --disable-postproc --disable-autodetect \
    --enable-mediafoundation --enable-d3d11va --enable-dxva2
make -j8
make install
