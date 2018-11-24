qhttp|qhttp-server{
    HEADERS += \
        $$PWD/qhttp/src/qhttpserverconnection.hpp \
        $$PWD/qhttp/src/qhttpserverrequest.hpp \
        $$PWD/qhttp/src/qhttpserverresponse.hpp \
        $$PWD/qhttp/src/qhttpserver.hpp \
        $$PWD/qhttp/src/private/qhttpserver_private.hpp \
        $$PWD/qhttp/src/private/qhttpserverconnection_private.hpp \
        $$PWD/qhttp/src/private/qhttpserverrequest_private.hpp \
        $$PWD/qhttp/src/private/qhttpserverresponse_private.hpp

    SOURCES += \
        $$PWD/qhttp/src/qhttpserverconnection.cpp \
        $$PWD/qhttp/src/qhttpserverrequest.cpp \
        $$PWD/qhttp/src/qhttpserverresponse.cpp \
        $$PWD/qhttp/src/qhttpserver.cpp

    !qhttp{
        CONFIG += qhttp
    }
}

qhttp|qhttp-client|contains(DEFINES, QHTTP_HAS_CLIENT){
    HEADERS += \
        $$PWD/qhttp/src/qhttpclient.hpp \
        $$PWD/qhttp/src/qhttpclientresponse.hpp \
        $$PWD/qhttp/src/qhttpclientrequest.hpp \
        $$PWD/qhttp/src/private/qhttpclient_private.hpp \
        $$PWD/qhttp/src/private/qhttpclientrequest_private.hpp \
        $$PWD/qhttp/src/private/qhttpclientresponse_private.hpp

    SOURCES += \
        $$PWD/qhttp/src/qhttpclientrequest.cpp \
        $$PWD/qhttp/src/qhttpclientresponse.cpp \
        $$PWD/qhttp/src/qhttpclient.cpp

    !qhttp{
        CONFIG += qhttp
    }
}

qhttp{
    DEFINES += QOMP_OLIVIERMARIDAT_QHTTP

    HEADERS += \
        $$PWD/qhttp/src/qhttpfwd.hpp \
        $$PWD/qhttp/src/qhttpabstracts.hpp \
        $$PWD/qhttp/src/private/httpparser.hxx \
        $$PWD/qhttp/src/private/httpreader.hxx \
        $$PWD/qhttp/src/private/httpwriter.hxx \
        $$PWD/qhttp/src/private/qhttpbase.hpp \
        $$PWD/qhttp/src/private/qsocket.hpp

    SOURCES  += \
        $$PWD/qhttp/src/qhttpabstracts.cpp

    INCLUDEPATH += \
        $$PWD/qhttp \
        $$PWD/qhttp/src

    QT += network
    DEFINES *= QHTTP_MEMORY_LOG=0
    CONFIG += c++11
    CONFIG += http-parser
}
