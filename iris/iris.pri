IRIS_BASE = $$PWD
include(common.pri)

greaterThan(QT_MAJOR_VERSION, 4) {
	CONFIG *= fast_depend_prl
}
else {
	CONFIG *= depend_prl
}

INCLUDEPATH += $$IRIS_BASE/include $$IRIS_BASE/include/iris $$IRIS_BASE/src

iris_bundle:{
	include(src/xmpp/xmpp.pri)
}
else {
	isEmpty(top_iris_builddir):top_iris_builddir = $$PWD
	LIBS += -L$$top_iris_builddir/lib -liris
}

# qt < 4.4 doesn't enable link_prl by default.  we could just enable it,
#   except that in 4.3 or earlier the link_prl feature is too aggressive and
#   pulls in unnecessary deps.  so, for 4.3 and earlier, we'll just explicitly
#   specify the stuff the prl should have given us.
# also, mingw seems to have broken prl support?? (still broken in qt-4.8 (c)rion, QTBUG-12901)
#win32-g++|contains($$list($$[QT_VERSION]), 4.0.*|4.1.*|4.2.*|4.3.*) {
#	DEFINES += IRISNET_STATIC             # from irisnet
#	LIBS += -L$$IRIS_BASE/lib -lirisnet   # from iris
#	windows:LIBS += -lWs2_32 -lAdvapi32   # from jdns
#	PRE_TARGETDEPS += $$IRIS_BASE/lib/libiris.a
#}

# force on all windows, plus qca ordering workaround
windows {
	DEFINES += IRISNET_STATIC             # from irisnet
	LIBS += -L$$IRIS_BASE/lib -lirisnet   # from iris
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
}
