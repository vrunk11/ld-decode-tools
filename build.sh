#!/usr/bin/env bash
#
# build.sh - configure and build ld-decode-tools into ./build
#
# Usage: ./build.sh [Release|Debug|RelWithDebInfo] [extra cmake arguments...]
#   The build type defaults to Release. Anything after it is passed to the
#   cmake configure step, e.g. ./build.sh Release -DBUILD_TESTING=OFF
#
# Inside `nix develop` every dependency, ezpwd included, is already provided.
# Outside it, Qt 6, FFTW3 and pkg-config must be installed, and the ezpwd
# Reed-Solomon headers are cloned into external/ on first use at the revision
# flake.lock pins.
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
#

set -euo pipefail

cd "$(dirname "$0")"

# The first argument is the build type only when it names one.
BUILD_TYPE=Release
case "${1:-}" in
    Release|Debug|RelWithDebInfo|MinSizeRel)
        BUILD_TYPE="$1"
        shift
        ;;
esac

EZPWD_REV=62a490c13f6e057fbf2dc6777fde234c7a19098e
EZPWD_LOCAL="$PWD/external/ezpwd-reed-solomon"

if [[ -z "${EZPWD_DIR:-}" ]]; then
    if [[ ! -f "$EZPWD_LOCAL/c++/ezpwd/rs_base" ]]; then
        echo "Fetching ezpwd-reed-solomon headers into external/ ..."
        rm -rf "$EZPWD_LOCAL"
        git clone --quiet https://github.com/pjkundert/ezpwd-reed-solomon "$EZPWD_LOCAL"
        git -C "$EZPWD_LOCAL" checkout --quiet "$EZPWD_REV"
    fi
    export EZPWD_DIR="$EZPWD_LOCAL/c++"
fi

generator=()
if command -v ninja > /dev/null 2>&1; then
    generator=(-G Ninja)
fi

jobs="$(nproc 2> /dev/null || sysctl -n hw.ncpu 2> /dev/null || echo 2)"

cmake -S . -B build "${generator[@]}" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DEZPWD_DIR="$EZPWD_DIR" \
    "$@"
cmake --build build --parallel "$jobs"

echo ""
echo "Build complete: binaries are in build/bin"
