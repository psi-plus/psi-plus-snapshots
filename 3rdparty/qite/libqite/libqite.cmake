if(${Qt5Core_VERSION} VERSION_LESS 5.0.0)
	message(FATAL_ERROR "Qt5 or above is required")
endif()

list(APPEND SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/qite.cpp
    ${CMAKE_CURRENT_LIST_DIR}/qiteaudio.cpp
    ${CMAKE_CURRENT_LIST_DIR}/qiteaudiorecorder.cpp
    )

list(APPEND HEADERS
    ${CMAKE_CURRENT_LIST_DIR}/qite.h
    ${CMAKE_CURRENT_LIST_DIR}/qiteaudio.h
    ${CMAKE_CURRENT_LIST_DIR}/qiteaudiorecorder.h
    )

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    )
