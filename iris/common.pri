# common stuff for iris.pro and iris.pri

greaterThan(QT_MAJOR_VERSION, 4) {
    CONFIG += c++11
} else {
    QMAKE_CXXFLAGS += -std=c++11
}

# default build configuration
!iris_build_pri {
    # build appledns on mac
    mac:CONFIG += appledns

    # bundle appledns inside of irisnetcore on mac
    mac:CONFIG += appledns_bundle

    # bundle irisnetcore inside of iris
    CONFIG += irisnetcore_bundle

    # don't build iris, app will include iris.pri
    #CONFIG += iris_bundle
}
