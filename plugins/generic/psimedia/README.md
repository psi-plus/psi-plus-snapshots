# PsiMedia

PsiMedia is a thick abstraction layer for providing audio and video RTP services to Psi-like IM clients. The implementation is based on GStreamer.

For more information, see [this article](http://jblog.andbit.net/2008/07/03/introducing-psimedia/).

Currently it is used for video- and audio-calls support in [Psi IM](https://psi-im.org/) and [Psi+](https://psi-plus.com/) projects.

## License

This library is licensed under the Lesser GNU General Public License. See the [COPYING](https://github.com/psi-im/psimedia/blob/master/COPYING) file for more information.

## Versions history

See [CHANGELOG](https://github.com/psi-im/psimedia/blob/master/CHANGELOG) file.

## Build dependencies

* psi (preferably same version as the plugin)
* qtbase >= 5.6
* glib >= 2.0
* gobject >= 2.0
* gthread >= 2.0
* gstreamer >= 1.14
* gst-plugins-base >= 1.14

## Installation

Contents:

```
psimedia/      API and plugin shim
gstprovider/   a common library for all other subporjects
gstplugin/     a legacy plugin still used in demo
psiplugin/     a plugin for Psi
demo/          demonstration GUI program
```

To build the plugins and demo program, run:

```sh
mkdir -p builddir
cd builddir
cmake .. -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_BUILD_TYPE=Release
make
```

```
make install DESTDIR=./out
tree ./out
```

## Windows CI and camera capture

Automatic CI runs on pushes to `master` and on opening, reopening or updating a
pull request. Other branch pushes do not trigger CI. The phrase `skip ci` anywhere
in a commit message, case-insensitively and with or without brackets, skips the
build jobs: push events check the pushed commits, and PR events check the head
commit. The policy check still runs for the unbracketed phrase. Manual workflow
runs ignore this marker.

The reusable `.github/workflows/windows.yml` builds Windows MSVC / Qt6 with
ccache in ordinary CI; it can also be dispatched alone. The manual
`Windows packaging` workflow adds the Win7 / Qt5 profile and uploads plugin
artifacts for both profiles. It does not publish a release.

| Profile | Compiler / Qt | GStreamer | Runs in |
| --- | --- | --- | --- |
| Windows MSVC / Qt6 | Visual Studio / Qt 6.8.3 | Official MSVC runtime and SDK 1.28.7 | CI and packaging |
| Windows MSVC / Qt5 / Win7 | v142 (14.29) / Qt 5.15.2 | Official MSVC runtime and SDK 1.24.13 | Packaging only |

The Win7 profile keeps Qt5 and the older native GStreamer SDK. Its compiler
and SRTP/OpenSSL dependencies target
`_WIN32_WINNT=0x0601` and `WINVER=0x0601`. GStreamer 1.26 and newer standard
Windows binaries target Windows 10, so the Win7 profile must keep its separate
SDK. These CI jobs run on Windows Server 2022; they verify compilation, linking
and media regressions, and do not replace a smoke test on Windows 7 SP1.

Windows capture defaults to `mfvideosrc`. Both profiles check the selected
GStreamer element before building, then run the payload, Opus and SRTP tests.
The MSVC installers are verified against the SHA256 values in the dependency lock. Artifacts contain
the plugin and its import report; deployment still needs the matching Qt and
GStreamer runtimes, including the Media Foundation plugin.

For an older/custom Windows SDK without Media Foundation, configure
`-DPSIMEDIA_WINDOWS_VIDEO_SOURCE=ksvideosrc`. The element is selected at build
time; PsiMedia does not silently choose another backend at runtime. An explicit
`SRTP_ROOT` selects that SDK in preference to a different pkg-config installation.

## Dependency lock

`dependencies.lock.json` is the single source for the Psi API repository/commit
and each Windows profile's Qt version/architecture, MSVC toolset and GStreamer
installer names, URLs and SHA256 values. Update this file when changing a managed
dependency; do not copy source revisions or SDK versions into workflow YAML.
The existing distribution Qt/GStreamer packages and the runner's vcpkg baseline
remain system dependencies rather than exact source locks.

All CI and packaging builds resolve the lock through
`.github/actions/load-dependencies` after checkout. The downloaded Psi tree must
have the exact locked Git HEAD and unmodified `plugins/include` headers.
The optional `psi_ref` input defaults to the lock and rejects a conflicting ref,
including branch names, instead of silently selecting another API revision.
These checks govern CI; normal local builds may still use their selected Psi API.

GStreamer installer hashes were obtained from the official per-file
`.sha256sum` files beside the installers. Successful downloads are verified before
execution and cached separately from compiler objects. The cache key depends
only on the GStreamer coordinates and hashes, so a Psi or Qt update does not
invalidate it. Cache hits are verified again and need no upstream requests.
On a cold cache, transient HTTP errors and interrupted transfers are retried up
to five times with increasing delay. Failed partial downloads and checksum
mismatches never become cached installers. A persistent upstream outage still
fails the build after the bounded retries.

The CI policy job runs dependency and downloader regression checks before media
builds. To run them locally:

```sh
python3 -B -m unittest discover -s tests/dependency_lock -p 'test_*.py' -v
pwsh -NoProfile -File tests/dependency_lock/windows-installers.ps1
```

These tests use small isolated fixtures and fake HTTP/installer calls; they do
not download SDKs or execute Windows installers. Native Windows compilation and
runtime loading remain covered by the Windows build jobs.
