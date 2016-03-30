#! /bin/bash

# Author:  Boris Pek <tehnick-8@mail.ru>
# License: GPLv2 or later
# Created: 2012-02-13
# Updated: 2016-03-31
# Version: N/A

set -e

if [[ ${0} =~ ^/.+$ ]]; then
    export PSIPLUS_DIR="$(dirname ${0})"
else
    export PSIPLUS_DIR="${PWD}/$(dirname ${0})"
fi

export MAIN_DIR="${PSIPLUS_DIR}/.."

# Test Internet connection:
host github.com > /dev/null

cd "${PSIPLUS_DIR}"

if [ "${1}" = "push" ]; then
    git push
    git push --tags
    exit 0
fi

OLD_VER=$(git tag -l | sort -V | tail -n1)
OLD_REVISION=$(echo ${OLD_VER} | sed -e "s/^[0-9]\+\.[0-9]\+\.\([0-9]\+\)$/\1/")

MOD=psi
if [ -d "${MAIN_DIR}/${MOD}" ]; then
    echo "Updating ${MAIN_DIR}/${MOD}"
    cd "${MAIN_DIR}/${MOD}"
    git pull --all
    git submodule update
    echo;
else
    echo "Creating ${MAIN_DIR}/${MOD}"
    cd "${MAIN_DIR}"
    git clone --depth 1 git://github.com/psi-im/${MOD}.git
    cd "${MAIN_DIR}/${MOD}"
    git submodule init
    git submodule update
    echo;
fi

MOD=main
if [ -d "${MAIN_DIR}/${MOD}" ]; then
    echo "Updating ${MAIN_DIR}/${MOD}"
    cd "${MAIN_DIR}/${MOD}"
    git pull --all
    echo;
else
    echo "Creating ${MAIN_DIR}/${MOD}"
    cd "${MAIN_DIR}"
    git clone git://github.com/psi-plus/${MOD}.git
    echo;
fi

MOD=plugins
if [ -d "${MAIN_DIR}/${MOD}" ]; then
    echo "Updating ${MAIN_DIR}/${MOD}"
    cd "${MAIN_DIR}/${MOD}"
    git pull --all
    echo;
else
    echo "Creating ${MAIN_DIR}/${MOD}"
    cd "${MAIN_DIR}"
    git clone --depth 1 git://github.com/psi-plus/${MOD}.git
    echo;
fi

MOD=resources
if [ -d "${MAIN_DIR}/${MOD}" ]; then
    echo "Updating ${MAIN_DIR}/${MOD}"
    cd "${MAIN_DIR}/${MOD}"
    git pull --all
    echo;
else
    echo "Creating ${MAIN_DIR}/${MOD}"
    cd "${MAIN_DIR}"
    git clone --depth 1 git://github.com/psi-plus/${MOD}.git
    echo;
fi

MOD=main
cd "${MAIN_DIR}/${MOD}"

rev=$(git describe --tags | sed -e "s/^.*-\([0-9]\+\)-.*$/\1/")
if [ "${rev}" = "$(git describe --tags)" ]; then rev="0"; fi
REVISION_DATE_LIST="$(cd ${MAIN_DIR}/psi  && git log -n1 --date=short --pretty=format:'%ad')
                    $(cd ${MAIN_DIR}/main && git log -n1 --date=short --pretty=format:'%ad')"
LAST_REVISION_DATE=$(echo "${REVISION_DATE_LIST}" | sort -r | head -n1)

CUR_VER="0.16"
LAST_REVISION=${rev}
NEW_VER="${CUR_VER}.${LAST_REVISION}"

# Fix NEW_VER if manually created tag exists
if [ "$(echo -e "${NEW_VER}\\n${OLD_VER}" | sort -V | tail -n1)" != "${NEW_VER}" ]; then
    NEW_VER="${OLD_VER}"
    EXTRA_TAG="true"
fi

echo "OLD_VER = ${OLD_VER}"
echo "NEW_VER = ${NEW_VER}"
echo;

cd "${PSIPLUS_DIR}"
echo "Updating is started."
echo;

find . -type f | \
    grep -v "^\./\.git" | \
    grep -v "^\./generate-single-repo.sh" | \
    grep -v "^\./configure" | \
    grep -v "^\./README" | \
    while read var; do rm "$var"; done
find . -depth -type d -empty -exec rmdir {} \;
echo "Directory is cleaned."
echo;

if [ ! -e "${PSIPLUS_DIR}/configure" ]; then
    wget -c "https://raw.github.com/tehnick/psi-plus/master/configure" || touch configure
fi

if [ ! -e "${PSIPLUS_DIR}/README" ]; then
    wget -c "https://raw.github.com/tehnick/psi-plus/master/README" || touch README
fi

mv "${PSIPLUS_DIR}/configure" "${MAIN_DIR}/configure"
mv "${PSIPLUS_DIR}/README" "${MAIN_DIR}/README"
rsync -a "${MAIN_DIR}/psi/" "${PSIPLUS_DIR}/" \
    --exclude=".git*" \
    --exclude="^configure" \
    --exclude="^README"
mv "${MAIN_DIR}/configure" "${PSIPLUS_DIR}/configure"
mv "${MAIN_DIR}/README" "${PSIPLUS_DIR}/README"
echo "Files from psi project are copied."
echo;

cat "${MAIN_DIR}/main/patches"/*.diff | \
    patch -d "${PSIPLUS_DIR}" -p1 2>&1 > \
    "${MAIN_DIR}/applying-patches_${NEW_VER}.log"
echo "${NEW_VER} ($(echo ${LAST_REVISION_DATE}))" > version
echo "Patches from Psi+ project are applied."
echo;

mkdir -p "${PSIPLUS_DIR}/patches"
rsync -a "${MAIN_DIR}/main/patches/dev" "${PSIPLUS_DIR}/patches/"
rsync -a "${MAIN_DIR}/main/patches/mac" "${PSIPLUS_DIR}/patches/"
rsync -a "${MAIN_DIR}/main/patches/haiku" "${PSIPLUS_DIR}/patches/"
echo "Extra patches from Psi+ project are copied."
echo;

rsync -a "${MAIN_DIR}/plugins" "${PSIPLUS_DIR}/src/" \
    --exclude=".git*"
echo "Plugins from Psi+ project are copied."
echo;

rsync -a "${MAIN_DIR}/main/iconsets/system/" "${PSIPLUS_DIR}/iconsets/system/"
rsync -a "${MAIN_DIR}/main/iconsets/affiliations/" "${PSIPLUS_DIR}/iconsets/affiliations/"
echo "Iconsets from Psi+ project are copied."
echo;

rsync -a "${MAIN_DIR}/resources/sound/" "${PSIPLUS_DIR}/sound/"
echo "Sound files from Psi+ project are copied."
echo;

mkdir -p "${PSIPLUS_DIR}/skins/"
rsync -a "${MAIN_DIR}/resources/skins/" "${PSIPLUS_DIR}/skins/"
echo "Skins from Psi+ project are copied."
echo;

rsync -a "${MAIN_DIR}/resources/themes/" "${PSIPLUS_DIR}/themes/"
echo "Themes from Psi+ project are copied."
echo;

cp "${MAIN_DIR}/main/changelog.txt" "${PSIPLUS_DIR}/ChangeLog"
echo "ChangeLog from Psi+ project is copied."
echo;


rm configure.exe
rm iris/configure.exe
rm -r win32/
rm -r src/libpsi/tools/idle/win32/
echo "Trash is removed."
echo;


# Update repo and make analysis
git add -A


COMMENT=""
STATUS="$(git status)"

TEST_ALL=$(echo "${STATUS}" | grep "   " |
             grep -v " src/applicationinfo.cpp" | \
             grep -v " generate-single-repo.sh" | \
             grep -v " configure" | \
             grep -v " README" | \
             wc -l)
TEST_SRC=$(echo "${STATUS}" | grep " src/" | wc -l)
TEST_IRIS=$(echo "${STATUS}" | grep " iris/" | wc -l)
TEST_LIBPSI=$(echo "${STATUS}" | grep " src/libpsi/" | wc -l)
TEST_PLUGINS=$(echo "${STATUS}" | grep " src/plugins/" | wc -l)
TEST_PATCHES=$(echo "${STATUS}" | grep " patches/" | wc -l)

echo "TEST_ALL = ${TEST_ALL}"
echo "TEST_IRIS = ${TEST_IRIS}"
echo "TEST_LIBPSI = ${TEST_LIBPSI}"
echo "TEST_PLUGINS = ${TEST_PLUGINS}"
echo "TEST_PATCHES = ${TEST_PATCHES}"
echo "TEST_SRC = ${TEST_SRC}"
echo;


if [ "${TEST_ALL}" -eq 0 ]; then
    echo "Updating is not required.";
    exit 0;
fi

if [ "${NEW_VER}" = "${OLD_VER}" ]; then
    if [ -z "${EXTRA_TAG}" ]; then
        COMMENT+="Sources are sync with upstream:\n"
    else
        COMMENT+="Psi+ is updated:\n"
    fi
    if [ "${TEST_SRC}" -gt "$((${TEST_LIBPSI}+${TEST_PLUGINS}+0))" ]; then
        COMMENT+="Psi is updated.\n"
    fi
else
    COMMENT+="Current version is ${NEW_VER}:\n"
    if [ "${TEST_SRC}" -gt "$((${TEST_LIBPSI}+${TEST_PLUGINS}+0))" ]; then
        COMMENT+="Psi+ is updated.\n"
    fi
fi

if [ "${TEST_IRIS}" -gt 0 ]; then
    COMMENT+="iris is updated.\n"
fi

if [ "${TEST_LIBPSI}" -gt 0 ]; then
    COMMENT+="libpsi is updated.\n"
fi

if [ "${TEST_PLUGINS}" -eq 1 ]; then
    COMMENT+="One plugin is updated.\n"
elif [ "${TEST_PLUGINS}" -gt 1 ]; then
    COMMENT+="Plugins are updated.\n"
fi

if [ "${TEST_PATCHES}" -gt 0 ]; then
    COMMENT+="Extra patches are updated.\n"
fi

echo -e "${COMMENT}"

git cm -a -m "$(echo -e ${COMMENT})" 2>&1 > \
    "${MAIN_DIR}/git-commit_${NEW_VER}.log"

if [ "${NEW_VER}" != "${OLD_VER}" ]; then
    git tag "${NEW_VER}"
    echo "Git tag ${NEW_VER} is created."
    echo;
fi

