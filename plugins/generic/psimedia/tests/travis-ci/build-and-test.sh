#!/bin/sh

# Author:  Boris Pek
# Version: N/A
# License: Public Domain

set -e
set -x

if [ ! -d "../psi" ]
then
    git clone https://github.com/psi-im/psi.git
    mv psi ../
fi

if [ "${TARGET}" = "linux64" ]
then
    ./tests/travis-ci/build-in-ubuntu.sh
elif [ "${TARGET}" = "macos64" ]
then
    ./tests/travis-ci/build-in-macos.sh
fi

cd builddir/
make install DESTDIR="${PWD}/../out"

cd ..
echo ;
echo "Installed files:"
find out/ -type f

echo ;
echo "Check mandatory files:"
ls -alp out/usr/lib/psi/plugins/libmediaplugin.*
du -shc out/usr/lib/psi/plugins/libmediaplugin.*
