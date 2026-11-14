ENABLE_TESTING()

set(SOURCES "filewatchunittest.cpp")

add_executable(filewatchunittest ${SOURCES})

target_link_qca_test_libraries(filewatchunittest)

add_qca_test(filewatchunittest "FileWatch")

# QFileSystemWatcher notification timing while a file is still open differs on
# Windows/MinGW.  This legacy test requires a change notification immediately
# after flush(), so keep building it but do not use that timing assumption as a
# gate for the Windows Qt5 SDK job.
if(MINGW AND NOT BUILD_WITH_QT6)
    set_tests_properties("FileWatch" PROPERTIES DISABLED TRUE)
endif()
