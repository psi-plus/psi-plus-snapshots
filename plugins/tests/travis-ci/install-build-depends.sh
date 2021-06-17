#!/bin/sh

# Author:  Boris Pek
# Version: N/A
# License: Public Domain

set -e
set -x

if [ "${TARGET}" = "linux64" ]
then
    # Add official Psi+ PPA with extra packages:
    sudo add-apt-repository ppa:psi-plus/ppa -ysu

    sudo apt-get update  -qq
    sudo apt-get install -qq cmake \
                             libhttp-parser-dev \
                             libhunspell-dev \
                             libidn11-dev \
                             libminizip-dev \
                             libotr5-dev \
                             libqca-qt5-2-dev \
                             libqt5svg5-dev \
                             libqt5webkit5-dev \
                             libqt5x11extras5-dev \
                             libsignal-protocol-c-dev \
                             libsm-dev \
                             libssl-dev \
                             libtidy-dev \
                             libxss-dev \
                             qt5keychain-dev \
                             qtmultimedia5-dev \
                             zlib1g-dev
elif [ "${TARGET}" = "macos64" ]
then
    export HOMEBREW_NO_AUTO_UPDATE=1
    export PACKAGES="ccache \
                     libidn \
                     qtkeychain \
                     qca \
                     minizip \
                     hunspell \
                     tidy-html5 \
                     libgpg-error \
                     libotr \
                     libsignal-protocol-c \
                    "
    brew install ${PACKAGES}
elif [ "${TARGET}" = "windows64" ]
then
    # Add MXE repository:
    sudo apt-get -y install software-properties-common lsb-release
    sudo apt-key adv --keyserver keyserver.ubuntu.com --recv-keys 86B72ED9
    sudo add-apt-repository \
        "deb [arch=amd64] https://pkg.mxe.cc/repos/apt `lsb_release -sc` main"

    export PREFIX="mxe-x86-64-w64-mingw32.shared"
    sudo apt-get update  -qq
    sudo apt-get install -qq cmake \
                             ${PREFIX}-hunspell \
                             ${PREFIX}-minizip \
                             ${PREFIX}-libotr \
                             ${PREFIX}-libsignal-protocol-c \
                             ${PREFIX}-tidy-html5 \
                             ${PREFIX}-qtbase \
                             ${PREFIX}-qttools \
                             ${PREFIX}-qttranslations \
                             ${PREFIX}-qtmultimedia \
                             ${PREFIX}-qtwebkit \
                             ${PREFIX}-gstreamer \
                             ${PREFIX}-gst-plugins-bad \
                             ${PREFIX}-gst-plugins-good
else
    echo "Unknown target!"
    exit 1
fi

