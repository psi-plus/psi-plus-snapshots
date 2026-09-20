include_guard(GLOBAL)

set(IRIS_JDNS_GIT_TAG
    "0357d0eebce7f50521350269d8461580e415a6c3"
    CACHE STRING "JDNS revision used by Iris on Android")

function(iris_configure_jdns)
    if(TARGET QJDns::QJDns)
        return()
    endif()

    include(FetchContent)

    # JDNS is a private implementation detail of Iris on Android. Keep it
    # static/PIC, use the same Qt major as Iris, and do not let its standalone
    # install/package rules leak into the Iris or embedding application's SDK.
    set(BUILD_SHARED_LIBS OFF)
    set(BUILD_QJDNS ON)
    set(BUILD_JDNS_TOOL OFF)
    set(JDNS_ENABLE_INSTALL OFF)
    set(JDNS_QT_MAJOR_VERSION 6)
    set(MULTI_QT OFF)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)

    FetchContent_Declare(
        iris_jdns
        GIT_REPOSITORY https://github.com/psi-im/jdns.git
        GIT_TAG "${IRIS_JDNS_GIT_TAG}"
    )
    FetchContent_MakeAvailable(iris_jdns)

    if(NOT TARGET QJDns::QJDns)
        message(FATAL_ERROR "Android Iris requires the QJDns::QJDns target")
    endif()
endfunction()
