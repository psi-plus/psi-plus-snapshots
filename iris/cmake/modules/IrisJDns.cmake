include_guard(GLOBAL)

set(IRIS_JDNS_GIT_TAG
    "0b7a9407b9401ed7680817ba3a368ce577ea87f2"
    CACHE STRING "JDNS revision built into Iris")

function(iris_configure_jdns)
    if(TARGET QJDns::QJDns)
        return()
    endif()

    include(FetchContent)

    # JDNS is a private implementation detail of Iris. Keep it static/PIC, use
    # the same Qt major as Iris, and do not let its standalone install/package
    # rules leak into the Iris or embedding application's SDK.
    set(BUILD_SHARED_LIBS OFF)
    set(BUILD_QJDNS ON)
    set(BUILD_JDNS_TOOL OFF)
    set(JDNS_ENABLE_INSTALL OFF)
    set(JDNS_BUILD_TESTS OFF)
    # JDNS declares its Qt major as a cache entry, which hides a normal
    # variable on the first configure (CMP0126). It must be the Qt of Iris.
    set(JDNS_QT_MAJOR_VERSION
        ${QT_DEFAULT_MAJOR_VERSION}
        CACHE STRING "Qt major version used by QJDns (5 or 6)" FORCE)
    set(MULTI_QT OFF)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)

    FetchContent_Declare(
        iris_jdns
        GIT_REPOSITORY https://github.com/psi-im/jdns.git
        GIT_TAG "${IRIS_JDNS_GIT_TAG}"
    )
    FetchContent_MakeAvailable(iris_jdns)

    if(NOT TARGET QJDns::QJDns)
        message(FATAL_ERROR "Iris requires the QJDns::QJDns target")
    endif()
endfunction()
