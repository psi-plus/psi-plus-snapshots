DataChannel implementation is borrowed from https://github.com/versatica/mediasoup/tree/v3/worker
https://github.com/versatica/mediasoup/commit/d3d369ecf3e255a6da04cc6d35098a3fd6bd6f22
on Mar 17, 2021

Files taken:
* DepUsrSCTP.cpp
* DepUsrSCTP.hpp
* SctpAssociation.cpp
* SctpAssociation.hpp
* MediaSoupErrors.hpp
* Logger.hpp (partially, just to make it compile)

Remaining files are written from scratch.

Changes to aforementioned files:
* removed not needed includes
* fixed printf format lines to avoid warnings
* replaced channel notifier with emit, added QObject stuff correspondingly
* preserve errno in SctpAssociation::SendSctpMessage (possible rewrite by logs handling)
