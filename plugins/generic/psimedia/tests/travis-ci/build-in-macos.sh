#!/bin/sh

# Author:  Boris Pek
# Version: N/A
# License: Public Domain

set -e
set -x

[ -z "${HOMEBREW}" ] && HOMEBREW="/usr/local"

PATH="${HOMEBREW}/bin:${PATH}"
PATH="${HOMEBREW}/opt/ccache/libexec:${PATH}"
CUR_DIR="$(dirname $(realpath -s ${0}))"
TOOLCHAIN_FILE="${CUR_DIR}/homebrew-toolchain.cmake"

[ -z "${BUILD_DEMO}" ] && BUILD_DEMO="OFF"

BUILD_OPTIONS="-DCMAKE_INSTALL_PREFIX=/usr \
               -DCMAKE_BUILD_TYPE=Release \
               -DBUILD_DEMO=${BUILD_DEMO}"

mkdir -p builddir
cd builddir

which nproc > /dev/null && JOBS=$(nproc) || JOBS=4

cmake .. -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}" ${BUILD_OPTIONS}
make -k -j ${JOBS} VERBOSE=1
