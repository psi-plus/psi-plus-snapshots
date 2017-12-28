IRIS_BASE = $$PWD
include(common.pri)

CONFIG *= link_prl # doesn't seems to work but at least it's documented unlike dependp_prl
unix {  # most of devs are on Linux anyway
    PRE_TARGETDEPS += $$top_iris_builddir/lib/libiris.a
    PRE_TARGETDEPS += $$top_iris_builddir/lib/libirisnet.a
}

INCLUDEPATH += $$IRIS_BASE/include $$IRIS_BASE/include/iris $$IRIS_BASE/src

iris_bundle:{
    include(src/xmpp/xmpp.pri)
}
else {
    isEmpty(top_iris_builddir):top_iris_builddir = $$PWD
    LIBS += -L$$top_iris_builddir/lib -liris
}

# force on all windows, plus qca ordering workaround
windows {
    DEFINES += IRISNET_STATIC             # from irisnet
    LIBS += -L$$top_iris_builddir/lib -lirisnet   # from iris
    LIBS += -lWs2_32 -lAdvapi32   # from jdns
    contains(LIBS, -lqca) {
        LIBS -= -lqca
        LIBS += -lqca
    }
    contains(LIBS, -lqcad) {
        LIBS -= -lqcad
        LIBS += -lqcad
    }

    contains(LIBS, -lidn) {
        LIBS -= -lidn
        LIBS += -lidn
    }

    contains(LIBS, -lz) {
        LIBS -= -lz
        LIBS += -lz
    }

    contains(LIBS, -lzlib) {
        LIBS -= -lzlib
        LIBS += -lzlib
    }
}
