#! /bin/sh

# Author:  Boris Pek <tehnick-8@mail.ru>
# License: GPLv2 or later
# Created: 2012-02-13
# Updated: 2012-02-13
# Version: N/A

export PSIPLUS_DIR="${PWD}/$(dirname ${0})"
export MAIN_DIR="${PSIPLUS_DIR}/.."

OLD_VER="0.15.5193"
#OLD_VER=$(git tag -l | tail -n1)
if [ -z "${OLD_VER}" ]; then echo "Something broken..."; exit 1; fi
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

MOD=psi-plus-ru
if [ -d "${MAIN_DIR}/${MOD}" ]; then
    echo "Updating ${MAIN_DIR}/${MOD}"
    cd "${MAIN_DIR}/${MOD}"
    git pull --all || exit 1
    echo;
else
    echo "Creating ${MAIN_DIR}/${MOD}"
    cd "${MAIN_DIR}"
    git clone --depth 1 git://github.com/ivan101/psi-plus-ru.git || exit 1
    echo;
fi

MOD=main
cd "${MAIN_DIR}/${MOD}"

rev=$((`git describe --tags | sed -e "s/^.*-\([0-9]\+\)-.*$/\1/"`+5000))
if [ "${LAST_REVISION}" = "5000" ]; then exit 1; fi

CUR_VER="0.15"
LAST_REVISION=${rev}
NEW_VER="${CUR_VER}.${LAST_REVISION}"

echo "OLD_VER = ${OLD_VER}"
echo "NEW_VER = ${NEW_VER}"
echo;

if [ "${NEW_VER}" = "${OLD_VER}" ]; then
    echo "Updating is not required.";
    exit 0;
fi

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


mv "${PSIPLUS_DIR}/README" "${MAIN_DIR}/README" || exit 1
rsync -a "${MAIN_DIR}/psi/" "${PSIPLUS_DIR}/" \
    --exclude=".git*" || exit 1
mv "${MAIN_DIR}/README" "${PSIPLUS_DIR}/README" || exit 1
echo "Files from psi project were copied."
echo;

cat "${MAIN_DIR}/main/patches"/*.diff | \
    patch -d "${PSIPLUS_DIR}" -p1 2>&1 > \
    "${MAIN_DIR}/applying-patches.log" || exit 1
sed "s/\(.xxx\)/.${rev}/" -i \
    "${PSIPLUS_DIR}/src/applicationinfo.cpp" || exit 1
echo "Patches from psi-dev project were applied."
echo;

rsync -a "${MAIN_DIR}/plugins" "${PSIPLUS_DIR}/src/" \
    --exclude=".git*" || exit 1
mv  "${PSIPLUS_DIR}/src/plugins/dev/otrplugin" \
    "${PSIPLUS_DIR}/src/plugins/generic" || exit 1
# mv  "${PSIPLUS_DIR}/src/plugins/unix"/* \
#     "${PSIPLUS_DIR}/src/plugins/generic" || exit 1
echo "Plugins from psi-dev project were copied."
echo;

rsync -a "${MAIN_DIR}/main/iconsets/" "${PSIPLUS_DIR}/iconsets/" || exit 1
echo "Iconsets from psi-dev project were copied."
echo;

cp "${MAIN_DIR}/main/changelog.txt" "${PSIPLUS_DIR}/ChangeLog" || exit 1
echo "ChangeLog from psi-dev project was copied."
echo;

mkdir -p "${PSIPLUS_DIR}/lang/ru"
rsync -a "${MAIN_DIR}/psi-plus-ru/" "${PSIPLUS_DIR}/lang/ru/" || exit 1
echo "Russian translation for Psi+ project was copied."
echo;


rm configure.exe
rm -r win32/
rm -r src/libpsi/tools/zip/minizip/win32/
rm -r src/libpsi/tools/idle/win32/
echo "Trash was removed."
echo;


exit 0

COMMENT="Sources were synced with upstream. Current version is ${NEW_VER}"
git add .  || exit 1
git add -u || exit 1
git cm -a -m "${COMMENT}" || exit 1
git tag "${NEW_VER}" || exit 1
echo "Git tag was created."

