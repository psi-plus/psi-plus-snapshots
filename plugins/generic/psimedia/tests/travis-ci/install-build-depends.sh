#!/bin/sh

# Author:  Boris Pek
# Version: N/A
# License: Public Domain

set -e
set -x

if [ "${TARGET}" = "linux64" ]
then
    sudo apt-get update  -qq
    sudo apt-get install -qq cmake \
                             libgstreamer-plugins-bad1.0-dev \
                             libgstreamer-plugins-base1.0-dev \
                             libgstreamer1.0-dev \
                             qtbase5-dev
fi

if [ "${TARGET}" = "macos64" ]
then
    HOMEBREW_NO_AUTO_UPDATE=1
    PACKAGES="ccache \
              gstreamer \
              gst-plugins-base \
              gst-plugins-bad \
              gst-plugins-good"
    for PKG in ${PACKAGES}
    do
        brew ls --versions "${PKG}" > /dev/null || brew install "${PKG}"
    done
fi

