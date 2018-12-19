# Qt5 or above is required.

set(qite_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/qite.cpp
    ${CMAKE_CURRENT_LIST_DIR}/qiteaudio.cpp
    ${CMAKE_CURRENT_LIST_DIR}/qiteaudiorecorder.cpp
    )

set(qite_HEADERS
    ${CMAKE_CURRENT_LIST_DIR}/qite.h
    ${CMAKE_CURRENT_LIST_DIR}/qiteaudio.h
    ${CMAKE_CURRENT_LIST_DIR}/qiteaudiorecorder.h
    )

include_directories(
    ${CMAKE_CURRENT_LIST_DIR}
    )
