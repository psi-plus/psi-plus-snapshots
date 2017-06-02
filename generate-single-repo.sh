#! /bin/sh

# Author:  Boris Pek <tehnick-8@yandex.ru>
# License: GPLv2 or later
# Created: 2012-02-13
# Updated: 2017-06-03
# Version: N/A

set -e

export SNAPSHOTS_DIR="$(dirname $(realpath -s ${0}))"
export MAIN_DIR="${SNAPSHOTS_DIR}/.."

PSI_URL=https://github.com/psi-im/psi.git
PLUGINS_URL=https://github.com/psi-im/plugins.git
PATCHES_URL=https://github.com/psi-plus/main.git
RESOURCES_URL=https://github.com/psi-plus/resources.git

# Test Internet connection:
host github.com > /dev/null

cd "${SNAPSHOTS_DIR}"

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
    git clone "${PSI_URL}"
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
    git clone "${PATCHES_URL}"
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
    git clone "${PLUGINS_URL}"
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
    git clone "${RESOURCES_URL}"
    echo;
fi

cd "${SNAPSHOTS_DIR}"
echo "Checking for updates..."

find . -type f | \
    grep -v "^\./\.git" | \
    grep -v "^\./generate-single-repo.sh" | \
    grep -v "^\./configure" | \
    grep -v "^\./README" | \
    while read var; do rm "$var"; done
find . -depth -type d -empty -exec rmdir {} \;
echo "* Directory is cleaned."

# Some paranoid checks:
for FILE in generate-single-repo.sh configure README; do
    if [ ! -e "${SNAPSHOTS_DIR}/${FILE}" ]; then
        wget -c "https://raw.github.com/psi-plus/psi-plus-snapshots/master/${FILE}"
    fi
done
chmod uog+x generate-single-repo.sh configure

mv "${SNAPSHOTS_DIR}/configure" "${MAIN_DIR}/configure"
mv "${SNAPSHOTS_DIR}/README" "${MAIN_DIR}/README"
rsync -a "${MAIN_DIR}/psi/" "${SNAPSHOTS_DIR}/" \
    --exclude=".git*" \
    --exclude="^configure" \
    --exclude="^README"
mv "${MAIN_DIR}/configure" "${SNAPSHOTS_DIR}/configure"
mv "${MAIN_DIR}/README" "${SNAPSHOTS_DIR}/README"
echo "* Files from psi project are copied."

cat "${MAIN_DIR}/main/patches"/*.diff | \
    patch -d "${SNAPSHOTS_DIR}" -p1 2>&1 > \
    "${MAIN_DIR}/applying-patches_${NEW_VER}.log"
echo "* Patches from Psi+ project are applied."

mkdir -p "${SNAPSHOTS_DIR}/patches"
rsync -a "${MAIN_DIR}/main/patches/dev" "${SNAPSHOTS_DIR}/patches/"
rsync -a "${MAIN_DIR}/main/patches/mac" "${SNAPSHOTS_DIR}/patches/"
rsync -a "${MAIN_DIR}/main/patches/haiku" "${SNAPSHOTS_DIR}/patches/"
echo "* Extra patches from Psi+ project are copied."

rsync -a "${MAIN_DIR}/plugins" "${SNAPSHOTS_DIR}/src/" \
    --exclude=".git*"
echo "* Plugins from Psi project are copied."

rsync -a "${MAIN_DIR}/main/iconsets/system/" "${SNAPSHOTS_DIR}/iconsets/system/"
echo "* Iconsets from Psi+ project are copied."

rsync -a "${MAIN_DIR}/resources/sound/" "${SNAPSHOTS_DIR}/sound/"
echo "* Sound files from Psi+ project are copied."

mkdir -p "${SNAPSHOTS_DIR}/skins/"
rsync -a "${MAIN_DIR}/resources/skins/" "${SNAPSHOTS_DIR}/skins/"
echo "* Skins from Psi+ project are copied."

rsync -a "${MAIN_DIR}/resources/themes/" "${SNAPSHOTS_DIR}/themes/"
echo "* Themes from Psi+ project are copied."

cp "${MAIN_DIR}/main/changelog.txt" "${SNAPSHOTS_DIR}/ChangeLog"
echo "* ChangeLog from Psi+ project is copied."

rm configure.exe
rm iris/configure.exe
rm -r win32/*
rm -r src/libpsi/tools/idle/win32/
find . -type f -name "*.orig" -delete
echo "* Trash is removed."

cp -a "${MAIN_DIR}/psi/win32"/*.rc "${SNAPSHOTS_DIR}/win32/"
cp -a "${MAIN_DIR}/psi/win32"/*.manifest "${SNAPSHOTS_DIR}/win32/"
cp -a "${MAIN_DIR}/main/app.ico" "${SNAPSHOTS_DIR}/win32/"
echo "* Some files for MS Windows build are copied."

echo;

# Update repo and make analysis
git add -A

TEST_ALL=$(LC_ALL=C git status | grep ":   " |
             grep -v " generate-single-repo.sh" | \
             grep -v " configure" | \
             grep -v " README" | \
             grep -v " version" | \
             wc -l)

if [ "${TEST_ALL}" = "0" ]; then
    echo "Updating is not required!";
    git co HEAD version
    echo;
    exit 0;
fi

PSI_HASH="$(cd ${MAIN_DIR}/psi && git show -s --pretty='format:%h')"
PATCHES_HASH="$(cd ${MAIN_DIR}/main && git show -s --pretty='format:%h')"
PLUGINS_HASH="$(cd ${MAIN_DIR}/plugins && git show -s --pretty='format:%h')"
RESOURCES_HASH="$(cd ${MAIN_DIR}/resources && git show -s --pretty='format:%h')"

REVISION_DATE_LIST="$(cd ${MAIN_DIR}/psi        && git log -n1 --date=short --pretty=format:'%ad')
$(cd ${MAIN_DIR}/main       && git log -n1 --date=short --pretty=format:'%ad')
$(cd ${MAIN_DIR}/plugins    && git log -n1 --date=short --pretty=format:'%ad')
$(cd ${MAIN_DIR}/resources  && git log -n1 --date=short --pretty=format:'%ad')"
LAST_REVISION_DATE=$(echo "${REVISION_DATE_LIST}" | sort -r | head -n1)

MAIN_VERSION=$(cd ${MAIN_DIR}/main && git tag | sort -V | grep '^[0-9]\+.[0-9]\+$' | tail -n 1)
LAST_REVISION=$(cd "${SNAPSHOTS_DIR}" && git tag -l | sort -V | tail -n1 | sed -e 's/^[0-9]\+\.[0-9\+]\.\([0-9]\+\).*$/\1/')

PREV_PSI_HASH=$(cd "${SNAPSHOTS_DIR}" && git show -s --pretty='format:%B' | sed -ne 's/^\* psi: \(.*\)$/\1/p')
PREV_PATCHES_HASH=$(cd "${SNAPSHOTS_DIR}" && git show -s --pretty='format:%B' | sed -ne 's/^\* patches: \(.*\)$/\1/p')

if [ "${PSI_HASH}" != "${PREV_PSI_HASH}" ] || [ "${PATCHES_HASH}" != "${PREV_PATCHES_HASH}" ]; then
    CUR_REVISION=$((${LAST_REVISION} + 1))
else
    CUR_REVISION="${LAST_REVISION}"
fi

NEW_VER="${MAIN_VERSION}.${CUR_REVISION}"
OLD_VER=$(cd "${SNAPSHOTS_DIR}" && git tag -l | sort -V | tail -n1)

echo "OLD_VER = ${OLD_VER}"
echo "NEW_VER = ${NEW_VER}"
echo;

echo "${NEW_VER}.${PATCHES_HASH}.${PSI_HASH} (${LAST_REVISION_DATE})" > version
echo "Version file is created."
echo;

COMMENT="Current version of Psi+ is ${NEW_VER}

It is based on:
* psi: ${PSI_HASH}
* patches: ${PATCHES_HASH}
* plugins: ${PLUGINS_HASH}
* resources: ${RESOURCES_HASH}
"
echo "${COMMENT}"

git cm -a -m "${COMMENT}" 2>&1 > \
    "${MAIN_DIR}/git-commit_${NEW_VER}.log"

if [ "${NEW_VER}" != "${OLD_VER}" ]; then
    git tag "${NEW_VER}"
    echo "Git tag \"${NEW_VER}\" is created."
fi

echo;

