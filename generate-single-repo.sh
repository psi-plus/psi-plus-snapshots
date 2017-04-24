#! /bin/sh

# Author:  Boris Pek <tehnick-8@yandex.ru>
# License: GPLv2 or later
# Created: 2012-02-13
# Updated: 2017-04-24
# Version: N/A

set -e

export PSIPLUS_DIR="$(dirname $(realpath -s ${0}))"
export MAIN_DIR="${PSIPLUS_DIR}/.."

# Test Internet connection:
host github.com > /dev/null

cd "${PSIPLUS_DIR}"

if [ "${1}" = "push" ]; then
    git push
    git push --tags
    exit 0
fi

MOD=psi
if [ -d "${MAIN_DIR}/${MOD}" ]; then
    echo "Updating ${MAIN_DIR}/${MOD}"
    cd "${MAIN_DIR}/${MOD}"
    git pull --all --prune
    git submodule init
    git submodule update
    echo;
else
    echo "Creating ${MAIN_DIR}/${MOD}"
    cd "${MAIN_DIR}"
    git clone https://github.com/psi-im/${MOD}.git
    cd "${MAIN_DIR}/${MOD}"
    git submodule init
    git submodule update
    echo;
fi

MOD=main
if [ -d "${MAIN_DIR}/${MOD}" ]; then
    echo "Updating ${MAIN_DIR}/${MOD}"
    cd "${MAIN_DIR}/${MOD}"
    git pull --all --prune
    echo;
else
    echo "Creating ${MAIN_DIR}/${MOD}"
    cd "${MAIN_DIR}"
    git clone https://github.com/psi-plus/${MOD}.git
    echo;
fi

MOD=plugins
if [ -d "${MAIN_DIR}/${MOD}" ]; then
    echo "Updating ${MAIN_DIR}/${MOD}"
    cd "${MAIN_DIR}/${MOD}"
    git pull --all --prune
    echo;
else
    echo "Creating ${MAIN_DIR}/${MOD}"
    cd "${MAIN_DIR}"
    git clone https://github.com/psi-im/${MOD}.git
    echo;
fi

MOD=resources
if [ -d "${MAIN_DIR}/${MOD}" ]; then
    echo "Updating ${MAIN_DIR}/${MOD}"
    cd "${MAIN_DIR}/${MOD}"
    git pull --all --prune
    echo;
else
    echo "Creating ${MAIN_DIR}/${MOD}"
    cd "${MAIN_DIR}"
    git clone https://github.com/psi-plus/${MOD}.git
    echo;
fi

MOD=psi-plus-cmake
if [ -d "${MAIN_DIR}/${MOD}" ]; then
    echo "Updating ${MAIN_DIR}/${MOD}"
    cd "${MAIN_DIR}/${MOD}"
    git pull --all --prune
    echo;
else
    echo "Creating ${MAIN_DIR}/${MOD}"
    cd "${MAIN_DIR}"
    git clone https://github.com/psi-plus/${MOD}.git
    echo;
fi

REVISION_DATE_LIST="$(cd ${MAIN_DIR}/psi        && git log -n1 --date=short --pretty=format:'%ad')
                    $(cd ${MAIN_DIR}/main       && git log -n1 --date=short --pretty=format:'%ad')
                    $(cd ${MAIN_DIR}/plugins    && git log -n1 --date=short --pretty=format:'%ad')
                    $(cd ${MAIN_DIR}/resources  && git log -n1 --date=short --pretty=format:'%ad')"
LAST_REVISION_DATE=$(echo "${REVISION_DATE_LIST}" | sort -r | head -n1)

MAIN_REVISION=$(cd ${MAIN_DIR}/main && git describe --tags | sed -e "s/^\(.\+\)-g.*$/\1/" | sed -e "s/-/\./")
if [ "$(cd ${MAIN_DIR}/psi && git describe --tags | sed -e "s/-/\n/g" | wc -l)" = "3" ]; then
    if [ "${MAIN_REVISION}" = "$(cd ${MAIN_DIR}/main && git describe --tags)" ]; then
        PSI_REVISION=".0.$(cd ${MAIN_DIR}/psi && git describe --tags | sed -e 's/^.*-\([0-9]\+\)-.*$/\1/')"
    else
        PSI_REVISION=".$(cd ${MAIN_DIR}/psi && git describe --tags | sed -e 's/^.*-\([0-9]\+\)-.*$/\1/')"
    fi
else
    PSI_REVISION=""
fi
NEW_VER="${MAIN_REVISION}${PSI_REVISION}"
OLD_VER=$(cd "${PSIPLUS_DIR}" && git tag -l | sort -V | tail -n1)

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

# Some paranoid checks:
for FILE in generate-single-repo.sh configure README; do
    if [ ! -e "${PSIPLUS_DIR}/${FILE}" ]; then
        wget -c "https://raw.github.com/psi-plus/psi-plus-snapshots/master/${FILE}"
    fi
done
chmod uog+x generate-single-repo.sh configure

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

rsync -a "${MAIN_DIR}/psi-plus-cmake/" "${PSIPLUS_DIR}/" \
    --exclude=".git*" \
    --exclude="README.md"
cp -a "${MAIN_DIR}/psi-plus-cmake/README.md" "${PSIPLUS_DIR}/README.cmake.md"
echo "Files from psi-plus-cmake repo are copied."
echo;

rm configure.exe
rm iris/configure.exe
rm -r win32/*
rm -r src/libpsi/tools/idle/win32/
echo "Trash is removed."
echo;

cp -a "${MAIN_DIR}/psi/win32"/*.rc "${PSIPLUS_DIR}/win32/"
cp -a "${MAIN_DIR}/psi/win32"/*.manifest "${PSIPLUS_DIR}/win32/"
cp -a "${MAIN_DIR}/main/app.ico" "${PSIPLUS_DIR}/win32/"
echo "Some files for MS Windows build are copied."
echo;


# Update repo and make analysis
git add -A


STATUS="$(git status)"

TEST_ALL=$(echo "${STATUS}" | grep "   " |
             grep -v " src/applicationinfo.cpp" | \
             grep -v " generate-single-repo.sh" | \
             grep -v " configure" | \
             grep -v " README" | \
             wc -l)
TEST_SRC=$(echo "${STATUS}"     | grep " src/"          | grep -iv "cmake" | wc -l)
TEST_IRIS=$(echo "${STATUS}"    | grep " iris/"         | grep -iv "cmake" | wc -l)
TEST_LIBPSI=$(echo "${STATUS}"  | grep " src/libpsi/"   | grep -iv "cmake" | wc -l)
TEST_PLUGINS=$(echo "${STATUS}" | grep " src/plugins/"  | grep -iv "cmake" | wc -l)
TEST_PATCHES=$(echo "${STATUS}" | grep " patches/"      | grep -iv "cmake" | wc -l)
TEST_CMAKE=$(echo "${STATUS}"   | grep -i "cmake"       | wc -l)

echo "TEST_ALL = ${TEST_ALL}"
echo "TEST_IRIS = ${TEST_IRIS}"
echo "TEST_LIBPSI = ${TEST_LIBPSI}"
echo "TEST_PLUGINS = ${TEST_PLUGINS}"
echo "TEST_PATCHES = ${TEST_PATCHES}"
echo "TEST_CMAKE = ${TEST_CMAKE}"
echo "TEST_SRC = ${TEST_SRC}"
echo;


if [ "${TEST_ALL}" -eq 0 ]; then
    echo "Updating is not required.";
    exit 0;
fi

COMMENT="Current version is ${NEW_VER}:\n"

if [ "${TEST_SRC}" -gt "$((${TEST_LIBPSI}+${TEST_PLUGINS}+0))" ]; then
    COMMENT="${COMMENT}Psi+ is updated.\n"
fi

if [ "${TEST_IRIS}" -gt 0 ]; then
    COMMENT="${COMMENT}iris is updated.\n"
fi

if [ "${TEST_LIBPSI}" -gt 0 ]; then
    COMMENT="${COMMENT}libpsi is updated.\n"
fi

if [ "${TEST_PLUGINS}" -eq 1 ]; then
    COMMENT="${COMMENT}One plugin is updated.\n"
elif [ "${TEST_PLUGINS}" -gt 1 ]; then
    COMMENT="${COMMENT}Plugins are updated.\n"
fi

if [ "${TEST_PATCHES}" -eq 1 ]; then
    COMMENT="${COMMENT}One extra patch is updated.\n"
elif [ "${TEST_PATCHES}" -gt 1 ]; then
    COMMENT="${COMMENT}Extra patches are updated.\n"
fi

if [ "${TEST_CMAKE}" -eq 1 ]; then
    COMMENT="${COMMENT}Cmake script is updated.\n"
elif [ "${TEST_CMAKE}" -gt 1 ]; then
    COMMENT="${COMMENT}Cmake scripts are updated.\n"
fi

echo "${COMMENT}"

git cm -a -m "$(echo ${COMMENT})" 2>&1 > \
    "${MAIN_DIR}/git-commit_${NEW_VER}.log"

if [ "${NEW_VER}" != "${OLD_VER}" ]; then
    git tag "${NEW_VER}"
    echo "Git tag ${NEW_VER} is created."
fi

echo;

