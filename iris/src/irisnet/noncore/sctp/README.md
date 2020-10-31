DataChannel implementation is borrowed from https://github.com/versatica/mediasoup/tree/v3/worker
https://github.com/versatica/mediasoup/commit/45dfac41b35d5cd88822fbdf92bbb85e258b4017
on Oct 21, 2020

Files taken:
* DepUsrSCTP.cpp
* DepUsrSCTP.hpp
* SctpAssociation.cpp
* SctpAssociation.hpp
* MediaSoupErrors.hpp
* Logger.hpp (partially, just to make it compile)

Remaining files are writte nfrom scratch.

Changes to aforementioned files:
* removed not needed includes
* fixed printf format lines to avoid warnings
* replaced channel notifier with emit, added QObject stuff correspondingly
