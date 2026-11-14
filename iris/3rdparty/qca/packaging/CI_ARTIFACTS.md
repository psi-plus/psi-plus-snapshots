# QCA3 CI artifacts

The workflows are intentionally split by responsibility.

## Build and test

`.github/workflows/ci.yml` verifies that QCA builds on Linux, Windows, macOS and
Android. The required provider set is fixed to `ossl;cyrus-sasl`. This workflow
does not publish SDK or package artifacts.

Linux uses the distribution Cyrus SASL. Windows, macOS and Android consume the
static dependency bundles described below.

## Cyrus SASL dependency bundles

`.github/workflows/deps-cyrus-sasl.yml` is both a reusable and manually triggered dependency
build. It creates the prerelease tag
`deps-cyrus-sasl-${CYRUS_VERSION}-${CYRUS_REVISION}` and publishes one static
bundle per non-Linux target as release assets.

The bundle contains a static Cyrus SASL with only the mechanisms QCA needs:

- PLAIN
- SCRAM
- DIGEST-MD5

The dependency recipe starts from the official Cyrus SASL 2.1.28 release and
applies Gentoo's `cyrus-sasl-2.1.28-r4-patches` patchset. Both the upstream
source tarball and Gentoo patch archive are verified against the SHA512 values
from Gentoo's `dev-libs/cyrus-sasl/Manifest`. Source preparation mirrors the
relevant Gentoo `src_prepare` steps (`sasldir`, `AC_CONFIG_MACRO_DIRS`,
`autoreconf`).

On macOS and Android the upstream autotools static-plugin support is used with
`-std=gnu17` and `-fPIC`; the build also follows Gentoo's compatibility choices
including `--disable-macos-framework`, `--disable-cmulocal`, `--disable-krb4`,
`--with-dblib=none` and `--with-sphinx-build=no`. Cross builds explicitly set
`CC_FOR_BUILD`. Android is pinned to NDK r27c (27.2.12479018), matching Qt 6.10.
Because every object in these QCA bundles is intentionally PIC, source
preparation reverts the single Gentoo avoid-PIC-overwrite change that would
otherwise append the static mechanisms to a throw-away top-level archive rather
than `lib/.libs/libsasl2.a`. The build checks all six required client/server
mechanism entry points before installation.

OpenSSL is intentionally *not* part of the static Cyrus bundle. The selected
SASL mechanisms keep their OpenSSL references unresolved until the final
`qca-cyrus-sasl` link. Windows uses the dynamic vcpkg OpenSSL triplet, macOS
uses Homebrew's shared OpenSSL, and the Android dependency build uses a private
shared OpenSSL sysroot with its static `libcrypto.a`/`libssl.a` archives removed
before Cyrus is configured and verified. The shipped QCA Android build then
links `OpenSSL::Crypto`/`OpenSSL::SSL` from KDAB `android_openssl` as shared
libraries.

On Windows `packaging/cyrus-sasl/patch-windows-static.py` adds a real
`libsasl2-static.lib` target to the upstream NMake build and explicitly compiles
only `STATIC_PLAIN`, `STATIC_SCRAM` and `STATIC_DIGESTMD5`. It also removes the
unused generated 64-bit `INT8`/`UINT8` aliases that collide with modern Windows
SDK type names, after first verifying that Cyrus does not use those aliases.
Upstream's `NO_STATIC_PLUGINS` define is intentionally retained so its Windows
`config.h` does not implicitly enable unrelated mechanisms.

Existing release assets are never overwritten. Bump `CYRUS_REVISION` whenever
the dependency recipe changes.

Trusted CI/package runs call this workflow automatically. Existing assets are
reused and only missing bundles are built. It can still be run manually when
prebuilding a new dependency revision.

## Packages and SDKs

`.github/workflows/packages.yml` publishes Actions artifacts suitable for
consumer CI:

- Ubuntu 24.04 `.deb` packages:
  - `libqca3-qt6-3`
  - `libqca3-qt6-dev`
  - `libqca3-qt6-plugins`
  - `qca3-qt6-utils`
- Windows x64 QCA3 SDK archive
- macOS arm64 and x86_64 QCA3 SDK archives
- Android QCA3 SDK archives for arm64-v8a, armeabi-v7a, x86_64 and x86

The packaging jobs require both `qca-ossl` and `qca-cyrus-sasl`. Non-Linux
jobs also verify that `qca-cyrus-sasl` has no shared Cyrus SASL runtime
dependency.


## Bootstrap order

The static Cyrus SASL release is intentionally independent of normal QCA CI.
After changing the Cyrus version, Gentoo patchset or bundle recipe, bump `CYRUS_REVISION`.
Trusted `ci.yml`/`packages.yml` runs invoke the reusable dependency workflow first; it creates the release and builds missing assets before desktop/Android jobs start. Pull requests remain read-only and require the corresponding dependency assets to exist already.


## Automatic dependency bootstrap

`deps-cyrus-sasl.yml` is both manually runnable and a reusable workflow.
`packages.yml` calls it before desktop/Android packaging. Existing release assets are reused; only missing assets are built and uploaded.
The normal CI does the same on trusted `push`/manual runs. Pull-request runs intentionally perform a read-only availability check instead of granting release-write permissions to PR code.

The dependency workflow is concurrency-serialized, so simultaneous CI/package runs cannot intentionally rebuild the same dependency set in parallel. Bump `CYRUS_REVISION` when the dependency recipe changes.
