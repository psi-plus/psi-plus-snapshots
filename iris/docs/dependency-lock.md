# Dependency locks and CI installation

The root `dependencies.lock.json` is the **only source of truth** for Iris's selected QCA build revision and its supported runtime baseline.

```json
{
  "qca": {
    "tag": "v3.1.2",
    "commit": "c9c56c2c131c897e807cb417f986c071a9c53ac7",
    "minimum_version": "3.0.10",
    "minimum_version_windows": "3.1.2"
  }
}
```

- `tag` and `commit` identify the **exact QCA release** used by CI builds and SDK packaging. They must refer to the same source revision. Prefer advancing them together.
- `minimum_version` is the **minimum compatible QCA version** for Linux, macOS and Android, exported to installed CMake consumers and DEB dependencies. Updating the CI build to a newer release does not automatically raise this minimum.
- `minimum_version_windows` raises the effective minimum on Windows to cover the native WinCrypto/Schannel provider. The build's CMake config and the shared dependency loader select the minimum by target platform. The Windows minimum must not be lower than the generic minimum. QCA 2 remains a separate supported provider choice where applicable.
- The bundled-QCA CMake build already reads `qca.commit` from this file and checks out that exact SHA, independently of any CI environment overrides.

## GitHub Actions

Use `.github/actions/load-dependencies` after checkout. The action exports `QCA_TAG`, `QCA_COMMIT` and `QCA_MIN_VERSION` and accepts one of three modes:

- `install-mode: none` — metadata only (Qt5/QCA2 builds and version jobs).
- `install-mode: apt` plus `qca-asset: qca3-deb-ubuntu-24.04.zip` or another supported distribution asset — download the locked release and install Debian packages.
- `install-mode: sdk` plus `qca-asset: qca3-qt6-windows-x64.zip`, `qca3-qt6-macos-arm64.zip` or an Android SDK asset — unpack to `qca-directory` (defaults to `qca-sdk`).

Before downloading, the action verifies that the release tag resolves to the pinned Git commit. Workflows choose only the **platform/asset variant**, never a competing QCA version.

When called from the Psi repository, after initializing its Iris submodule, use `./iris/.github/actions/load-dependencies` with `lock-path: iris/dependencies.lock.json`. This allows Psi workflows to install the **same QCA version that their Iris gitlink requires** without another hardcoded tag.

Package builds should check out their chosen Iris source SHA **before** calling the action, so releases built from older source tags retain the dependency revision belonging to that tag.

## JDNS

`jdns.commit` pins the bundled JDNS source; `jdns.minimum_version` is the QJDns
API baseline (3.1.1). `jdns.tag` records the corresponding published release.
After the JDNS release is merged and tagged, update the commit and tag together;
do not use a moving branch or a tag that has not been published. Source builds
always use the exact commit unless an explicit `IRIS_JDNS_GIT_TAG` override is
provided. Old cached defaults advance with the lock; explicit overrides remain.

JDNS is enabled by default on all platforms; Android needs it for its
platform resolver. Non-Android builds may disable it with
`-DIRIS_ENABLE_JDNS=OFF`. When enabled,
`IRIS_SYSTEM_JDNS` selects its provider:

- `AUTO` (default): use an installed QJDns >= 3.1.1 with the same Qt major, or
  build the locked source if no compatible package is available.
- `ON`: require a compatible installed package; report a configuration error
  instead of silently building another provider.
- `OFF`: build the bundled source, statically with PIC and without installation
  rules leaking into the Iris SDK.

The installed QJDns package exports its Qt major. A pre-existing incompatible
target is an error, because replacing a target already used by another part of
the build would be unsafe. For local development, CMake's standard
`FETCHCONTENT_SOURCE_DIR_IRIS_JDNS` override can select a JDNS checkout.

Do not duplicate `gh release download` or QCA pin constants in individual workflows. `tests/workflows/test_qca_dependency_lock.py` enforces this convention.
