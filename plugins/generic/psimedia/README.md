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
