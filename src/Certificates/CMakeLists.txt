unset(HEADERS)
unset(FORMS)
unset(SOURCES)
unset(UI_FORMS)

include_directories(
    ${PROJECT_SOURCE_DIR}/iris/include/iris
    ${PROJECT_SOURCE_DIR}/iris/src
)

list(APPEND FORMS
    CertificateDisplay.ui
)

list(APPEND HEADERS
    CertificateDisplayDialog.h
    CertificateErrorDialog.h

    CertificateHelpers.h
)

list(APPEND SOURCES
    CertificateDisplayDialog.cpp
    CertificateErrorDialog.cpp
    CertificateHelpers.cpp
)

qt_wrap_ui(UI_FORMS ${FORMS})
add_library(Certificates STATIC ${HEADERS} ${SOURCES} ${UI_FORMS})
target_link_libraries(Certificates ${QT_LIBRARIES} ${iris_LIB})
target_include_directories(Certificates PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
