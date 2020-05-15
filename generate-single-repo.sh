#! /bin/sh

# Author:  Boris Pek <tehnick-8@yandex.ru>
# License: GPLv2 or later
# Created: 2012-02-13
# Updated: 2020-05-15
# Version: N/A

set -e

export SNAPSHOTS_DIR="$(dirname $(realpath -s ${0}))"
export MAIN_DIR="$(realpath -s ${SNAPSHOTS_DIR}/..)"

PSI_URL=https://github.com/psi-im/psi.git
PLUGINS_URL=https://github.com/psi-im/plugins.git
PSIMEDIA_URL=https://github.com/psi-im/psimedia.git
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

SNAPSHOTS_URL="$(git remote -v | grep '(fetch)')"
if [ "$(echo ${SNAPSHOTS_URL} | grep 'https://' | wc -l)" = "1" ]; then
    echo "Updating ${SNAPSHOTS_DIR}"
    git checkout HEAD .
    git pull --all --prune -f
    echo;
fi

MOD=psi
if [ -d "${MAIN_DIR}/${MOD}" ]; then
    echo "Updating ${MAIN_DIR}/${MOD}"
    cd "${MAIN_DIR}/${MOD}"
    git pull --all --prune -f
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
    git pull --all --prune -f
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
    git pull --all --prune -f
    echo;
else
    echo "Creating ${MAIN_DIR}/${MOD}"
    cd "${MAIN_DIR}"
    git clone "${PLUGINS_URL}"
    echo;
fi

MOD=psimedia
if [ -d "${MAIN_DIR}/${MOD}" ]; then
    echo "Updating ${MAIN_DIR}/${MOD}"
    cd "${MAIN_DIR}/${MOD}"
    git pull --all --prune -f
    echo;
else
    echo "Creating ${MAIN_DIR}/${MOD}"
    cd "${MAIN_DIR}"
    git clone "${PSIMEDIA_URL}"
    echo;
fi

MOD=resources
if [ -d "${MAIN_DIR}/${MOD}" ]; then
    echo "Updating ${MAIN_DIR}/${MOD}"
    cd "${MAIN_DIR}/${MOD}"
    git pull --all --prune -f
    echo;
else
    echo "Creating ${MAIN_DIR}/${MOD}"
    cd "${MAIN_DIR}"
    git clone "${RESOURCES_URL}"
    echo;
fi

cd "${SNAPSHOTS_DIR}"
echo "Checking for updates..."

PSI_OLD_HASH=$(cd "${SNAPSHOTS_DIR}" && git show -s --pretty='format:%B' | sed -ne 's/^\* psi: \(.*\)$/\1/p')
PATCHES_OLD_HASH=$(cd "${SNAPSHOTS_DIR}" && git show -s --pretty='format:%B' | sed -ne 's/^\* patches: \(.*\)$/\1/p')
PLUGINS_OLD_HASH=$(cd "${SNAPSHOTS_DIR}" && git show -s --pretty='format:%B' | sed -ne 's/^\* plugins: \(.*\)$/\1/p')
PSIMEDIA_OLD_HASH=$(cd "${SNAPSHOTS_DIR}" && git show -s --pretty='format:%B' | sed -ne 's/^\* psimedia: \(.*\)$/\1/p')
RESOURCES_OLD_HASH=$(cd "${SNAPSHOTS_DIR}" && git show -s --pretty='format:%B' | sed -ne 's/^\* resources: \(.*\)$/\1/p')

PSI_NEW_HASH=$(cd "${MAIN_DIR}/psi" && git show -s --pretty='format:%h')
PATCHES_NEW_HASH=$(cd "${MAIN_DIR}/main" && git show -s --pretty='format:%h')
PLUGINS_NEW_HASH=$(cd "${MAIN_DIR}/plugins" && git show -s --pretty='format:%h')
PSIMEDIA_NEW_HASH=$(cd "${MAIN_DIR}/psimedia" && git show -s --pretty='format:%h')
RESOURCES_NEW_HASH=$(cd "${MAIN_DIR}/resources" && git show -s --pretty='format:%h')

if [ "${PSI_OLD_HASH}"       = "${PSI_NEW_HASH}" ] && \
   [ "${PATCHES_OLD_HASH}"   = "${PATCHES_NEW_HASH}" ] && \
   [ "${PLUGINS_OLD_HASH}"   = "${PLUGINS_NEW_HASH}" ] && \
   [ "${PSIMEDIA_OLD_HASH}"   = "${PSIMEDIA_NEW_HASH}" ] && \
   [ "${RESOURCES_OLD_HASH}" = "${RESOURCES_NEW_HASH}" ]; then
    echo "Updating is not required!";
    git checkout HEAD .
    echo;
    exit 0;
fi

echo "Updating is required!";
echo;

cd "${SNAPSHOTS_DIR}"
echo "Starting Psi+ update..."

find . -type f | \
    grep -v "^\./\.git" | \
    grep -v "^\./generate-single-repo.sh$" | \
    grep -v "^\./README$" | \
    while read var; do rm "$var"; done
find . -depth -type d -empty -exec rmdir {} \;
echo "* Directory is cleaned."

# Some paranoid checks:
for FILE in generate-single-repo.sh README .gitignore; do
    if [ ! -e "${SNAPSHOTS_DIR}/${FILE}" ]; then
        wget -c "https://raw.github.com/psi-plus/psi-plus-snapshots/master/${FILE}"
    fi
done
chmod uog+x generate-single-repo.sh

cp -f "${SNAPSHOTS_DIR}/README" "${MAIN_DIR}/README"
cp -f "${SNAPSHOTS_DIR}/.gitignore" "${MAIN_DIR}/.gitignore"
rsync -a "${MAIN_DIR}/psi/" "${SNAPSHOTS_DIR}/" \
    --exclude=".git*" \
    --exclude="/builddir*" \
    --exclude="/README.md" \
    --exclude="/README"
mv "${MAIN_DIR}/README" "${SNAPSHOTS_DIR}/README"
mv "${MAIN_DIR}/.gitignore" "${SNAPSHOTS_DIR}/.gitignore"
echo "* Files from psi project are copied."

failed_to_apply_patches()
{
    remove_trash
    git checkout HEAD .
    echo "* Failed to apply patches from Psi+ project!"
    exit 1
}

remove_trash()
{
    rm -rf *.exe
    rm -rf */*.exe
    rm -rf src/libpsi/tools/idle/win32/
    find . -type f -name "*.orig" -delete
}

# cat "${MAIN_DIR}/main/patches"/*.diff | \
#     patch -d "${SNAPSHOTS_DIR}" -p1 2>&1 > \
#     "${MAIN_DIR}/applying-patches.log" || failed_to_apply_patches
FROM_STR="option(PSI_PLUS .*$"
TO_STR="option(PSI_PLUS \"Build Psi+ client instead of Psi\" ON)"
sed -i "s|${FROM_STR}|${TO_STR}|g" CMakeLists.txt
echo "* Patches from Psi+ project are applied."

mkdir -p "${SNAPSHOTS_DIR}/patches"
rsync -a "${MAIN_DIR}/main/patches/dev" "${SNAPSHOTS_DIR}/patches/"
rsync -a "${MAIN_DIR}/main/patches/mac" "${SNAPSHOTS_DIR}/patches/"
echo "* Extra patches from Psi+ project are copied."

rsync -a "${MAIN_DIR}/plugins" "${SNAPSHOTS_DIR}/" \
    --exclude=".git*" \
    --exclude="/builddir*"
rsync -a "${MAIN_DIR}/psimedia" "${SNAPSHOTS_DIR}/plugins/generic/" \
    --exclude=".git*" \
    --exclude="/builddir*" \
    --exclude="/demo*" \
    --exclude="/gstplugin*"
echo "* Plugins from Psi project are copied."

rsync -a "${MAIN_DIR}/main/admin/" "${SNAPSHOTS_DIR}/admin/"
echo "* Extra scripts from Psi+ project are copied."

rsync -a "${MAIN_DIR}/resources/sound/" "${SNAPSHOTS_DIR}/sound/"
echo "* Sound files from Psi+ project are copied."

mkdir -p "${SNAPSHOTS_DIR}/skins/"
rsync -a "${MAIN_DIR}/resources/skins/" "${SNAPSHOTS_DIR}/skins/"
echo "* Skins from Psi+ project are copied."

cp "${MAIN_DIR}/main/ChangeLog.Psi+.txt" "${SNAPSHOTS_DIR}/ChangeLog.Psi+.txt"
echo "* ChangeLog from Psi+ project is copied."

remove_trash
echo "* Trash is removed."

cp -a "${MAIN_DIR}/psi/win32"/*.rc* "${SNAPSHOTS_DIR}/win32/"
cp -a "${MAIN_DIR}/psi/win32"/*.cmake "${SNAPSHOTS_DIR}/win32/"
cp -a "${MAIN_DIR}/psi/win32"/*.manifest "${SNAPSHOTS_DIR}/win32/"
echo "* Some files for MS Windows builds are copied."

cp -a "${SNAPSHOTS_DIR}/mac/application-plus.icns" \
      "${SNAPSHOTS_DIR}/mac/application.icns"
echo "* Some files for macOS builds are copied."

echo;

# Update repo and make analysis
git add -A .

TEST_ALL=$(LC_ALL=C git status | grep ":   " |
             grep -v " generate-single-repo.sh" | \
             grep -v " README" | \
             grep -v " version" | \
             wc -l)

if [ "${TEST_ALL}" = "0" ]; then
    echo "Updating is not required!";
    git checkout HEAD .
    echo;
    exit 0;
fi

REVISION_DATE_LIST="$(cd ${MAIN_DIR}/psi        && git log -n1 --date=short --pretty=format:'%ad')
$(cd ${MAIN_DIR}/main       && git log -n1 --date=short --pretty=format:'%ad')
$(cd ${MAIN_DIR}/plugins    && git log -n1 --date=short --pretty=format:'%ad')
$(cd ${MAIN_DIR}/resources  && git log -n1 --date=short --pretty=format:'%ad')"
LAST_REVISION_DATE=$(echo "${REVISION_DATE_LIST}" | sort -r | head -n1)

PATCHES_VERSION="$(cd ${MAIN_DIR}/main && git describe --tags | cut -d - -f1)"
PATCHES_NUM="$(cd ${MAIN_DIR}/main && git describe --tags | cut -d - -f2)"
PSI_NUM="0"

if [ "$(cd ${MAIN_DIR}/psi && git tag | grep -x "^${PATCHES_VERSION}$" | wc -l)" = "1" ]; then
    PSI_NUM="$(cd ${MAIN_DIR}/psi && git rev-list --count ${PATCHES_VERSION}..HEAD)"
fi

NEW_VER="${PATCHES_VERSION}.$(expr ${PSI_NUM} + ${PATCHES_NUM})"
OLD_VER=$(cd "${SNAPSHOTS_DIR}" && git tag -l | sort -V | tail -n1)

echo "OLD_VER = ${OLD_VER}"
echo "NEW_VER = ${NEW_VER}"
echo;

echo "${NEW_VER} (${LAST_REVISION_DATE}, Psi:${PSI_NEW_HASH}, Psi+:${PATCHES_NEW_HASH})" > version
echo "Version file is created:"
cat  version
echo;

COMMENT="Current version of Psi+ is ${NEW_VER}

It is based on:
* psi: ${PSI_NEW_HASH}
* patches: ${PATCHES_NEW_HASH}
* plugins: ${PLUGINS_NEW_HASH}
* resources: ${RESOURCES_NEW_HASH}
"
echo "${COMMENT}"

git commit -a -m "${COMMENT}" 2>&1 > /dev/null

if [ "${NEW_VER}" != "${OLD_VER}" ]; then
    git tag "${NEW_VER}"
    echo "Git tag \"${NEW_VER}\" is created."
fi

echo;

