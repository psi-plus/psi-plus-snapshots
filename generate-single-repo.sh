#! /bin/bash

# Author:  Boris Pek <tehnick-8@mail.ru>
# License: GPLv2 or later
# Created: 2012-02-13
# Updated: 2013-01-24
# Version: N/A

if [[ ${0} =~ ^/.+$ ]]; then
    export PSIPLUS_DIR="$(dirname ${0})"
else
    export PSIPLUS_DIR="${PWD}/$(dirname ${0})"
fi

export MAIN_DIR="${PSIPLUS_DIR}/.."

# Test Internet connection:
host github.com > /dev/null || exit 1

cd "${PSIPLUS_DIR}" || exit 1
OLD_VER=$(git tag -l | sort -V | tail -n1)
OLD_REVISION=$(echo ${OLD_VER} | sed -e "s/^[0-9]\+\.[0-9]\+\.\([0-9]\+\)$/\1/")

MOD=psi
if [ -d "${MAIN_DIR}/${MOD}" ]; then
    echo "Updating ${MAIN_DIR}/${MOD}"
    cd "${MAIN_DIR}/${MOD}"
    git pull --all || exit 1
    git submodule update || exit 1
    echo;
else
    echo "Creating ${MAIN_DIR}/${MOD}"
    cd "${MAIN_DIR}"
    git clone --depth 1 git://github.com/psi-im/${MOD}.git || exit 1
    cd "${MAIN_DIR}/${MOD}"
    git submodule init || exit 1
    git submodule update || exit 1
    echo;
fi

MOD=main
if [ -d "${MAIN_DIR}/${MOD}" ]; then
    echo "Updating ${MAIN_DIR}/${MOD}"
    cd "${MAIN_DIR}/${MOD}"
    git pull --all || exit 1
    echo;
else
    echo "Creating ${MAIN_DIR}/${MOD}"
    cd "${MAIN_DIR}"
    git clone git://github.com/psi-plus/${MOD}.git || exit 1
    echo;
fi

MOD=plugins
if [ -d "${MAIN_DIR}/${MOD}" ]; then
    echo "Updating ${MAIN_DIR}/${MOD}"
    cd "${MAIN_DIR}/${MOD}"
    git pull --all || exit 1
    echo;
else
    echo "Creating ${MAIN_DIR}/${MOD}"
    cd "${MAIN_DIR}"
    git clone --depth 1 git://github.com/psi-plus/${MOD}.git || exit 1
    echo;
fi

MOD=resources
if [ -d "${MAIN_DIR}/${MOD}" ]; then
    echo "Updating ${MAIN_DIR}/${MOD}"
    cd "${MAIN_DIR}/${MOD}"
    git pull --all || exit 1
    echo;
else
    echo "Creating ${MAIN_DIR}/${MOD}"
    cd "${MAIN_DIR}"
    git clone --depth 1 git://github.com/psi-plus/${MOD}.git || exit 1
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

echo "OLD_VER = ${OLD_VER}"
echo "NEW_VER = ${NEW_VER}"
echo;

cd "${PSIPLUS_DIR}"
echo "Updating is started."
echo;

find . -type f | \
    grep -v "^\./\.git" | \
    grep -v "^\./generate-single-repo.sh" | \
    grep -v "^\./README" | \
    while read var; do rm "$var"; done
find . -depth -type d -empty -exec rmdir {} \;
echo "Directory is cleaned."
echo;

if !([ -e "${PSIPLUS_DIR}/README" ]); then
    wget -4 -c "https://raw.github.com/tehnick/psi-plus/master/README" || touch README
fi

mv "${PSIPLUS_DIR}/README" "${MAIN_DIR}/README" || exit 1
rsync -a "${MAIN_DIR}/psi/" "${PSIPLUS_DIR}/" \
    --exclude=".git*" || exit 1
mv "${MAIN_DIR}/README" "${PSIPLUS_DIR}/README" || exit 1
echo "Files from psi project were copied."
echo;

cat "${MAIN_DIR}/main/patches"/*.diff | \
    patch -d "${PSIPLUS_DIR}" -p1 2>&1 > \
    "${MAIN_DIR}/applying-patches_${NEW_VER}.log" || exit 1
echo "${NEW_VER} (${LAST_REVISION_DATE})" > version || exit 1
echo "Patches from Psi+ project were applied."
echo;

mkdir -p "${PSIPLUS_DIR}/src/patches"
rsync -a "${MAIN_DIR}/main/patches/dev" "${PSIPLUS_DIR}/src/patches/" || exit 1
rsync -a "${MAIN_DIR}/main/patches/mac" "${PSIPLUS_DIR}/src/patches/" || exit 1
echo "Extra patches from Psi+ project were copied."
echo;

rsync -a "${MAIN_DIR}/plugins" "${PSIPLUS_DIR}/src/" \
    --exclude=".git*" || exit 1
mv  "${PSIPLUS_DIR}/src/plugins/dev/otrplugin" \
    "${PSIPLUS_DIR}/src/plugins/generic" || exit 1
echo "Plugins from Psi+ project were copied."
echo;

rsync -a "${MAIN_DIR}/main/iconsets/system/" "${PSIPLUS_DIR}/iconsets/system/" || exit 1
rsync -a "${MAIN_DIR}/main/iconsets/roster/" "${PSIPLUS_DIR}/iconsets/roster/" || exit 1
echo "Iconsets from Psi+ project were copied."
echo;

rsync -a "${MAIN_DIR}/resources/sound/" "${PSIPLUS_DIR}/sound/" || exit 1
echo "Sound files from Psi+ project were copied."
echo;

mkdir -p "${PSIPLUS_DIR}/skins/"
rsync -a "${MAIN_DIR}/resources/skins/" "${PSIPLUS_DIR}/skins/" || exit 1
echo "Skins from Psi+ project were copied."
echo;

rsync -a "${MAIN_DIR}/resources/themes/" "${PSIPLUS_DIR}/themes/" || exit 1
echo "Themes from Psi+ project were copied."
echo;

cp "${MAIN_DIR}/main/changelog.txt" "${PSIPLUS_DIR}/ChangeLog" || exit 1
echo "ChangeLog from Psi+ project was copied."
echo;


rm configure.exe
rm -r win32/
#rm -r src/libpsi/tools/zip/minizip/win32/
rm -r src/libpsi/tools/idle/win32/
echo "Trash was removed."
echo;


git add .  || exit 1
git add -u || exit 1


COMMENT=""
STATUS="$(git status)"

TEST_ALL=$(echo "${STATUS}" | grep "#	" |
             grep -v " src/applicationinfo.cpp" | \
             grep -v " generate-single-repo.sh" | \
             grep -v " README" | \
             wc -l)
TEST_SRC=$(echo "${STATUS}" | grep " src/" | wc -l)
TEST_IRIS=$(echo "${STATUS}" | grep " iris/" | wc -l)
TEST_LIBPSI=$(echo "${STATUS}" | grep " src/libpsi/" | wc -l)
TEST_PLUGINS=$(echo "${STATUS}" | grep " src/plugins/" | wc -l)
TEST_PATCHES=$(echo "${STATUS}" | grep " src/patches/" | wc -l)

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
    COMMENT+="Sources were sync with upstream:\n"
    if [ "${TEST_SRC}" -gt "$((${TEST_LIBPSI}+${TEST_PLUGINS}+0))" ]; then
        COMMENT+="Psi was updated.\n"
    fi
else
    COMMENT+="Current version is ${NEW_VER}:\n"
    if [ "${TEST_SRC}" -gt "$((${TEST_LIBPSI}+${TEST_PLUGINS}+0))" ]; then
        COMMENT+="Psi+ was updated.\n"
    fi
fi

if [ "${TEST_IRIS}" -gt 0 ]; then
    COMMENT+="Iris was updated.\n"
fi

if [ "${TEST_LIBPSI}" -gt 0 ]; then
    COMMENT+="Libpsi was updated.\n"
fi

if [ "${TEST_PLUGINS}" -eq 1 ]; then
    COMMENT+="One plugin was updated.\n"
elif [ "${TEST_PLUGINS}" -gt 1 ]; then
    COMMENT+="Plugins were updated.\n"
fi

if [ "${TEST_PATCHES}" -gt 0 ]; then
    COMMENT+="Extra patches were updated.\n"
fi

echo -e "${COMMENT}"

git cm -a -m "$(echo -e ${COMMENT})" 2>&1 > \
    "${MAIN_DIR}/git-commit_${NEW_VER}.log" || exit 1

if [ "${NEW_VER}" != "${OLD_VER}" ]; then
    git tag "${NEW_VER}" || exit 1
    echo "Git tag was created."
    echo;
fi

if [ "${1}" = "push" ]; then
    git push
    git push --tags
fi

