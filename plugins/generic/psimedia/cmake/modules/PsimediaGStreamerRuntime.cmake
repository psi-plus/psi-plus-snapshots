# Installs the prebuilt GStreamer runtime the Windows media plugin needs next to it: the GStreamer plugins that
# provide the elements psimedia creates, and every DLL those plugins and the media plugin import. Nothing is built:
# the files come from the GStreamer SDK the plugin was built against.
#
# Layout (as the Psi plugin expects it): runtime DLLs in BIN_INSTALL_DIR, GStreamer plugins in
# BIN_INSTALL_DIR/gstreamer-1.0 (psiplugin passes that directory as GST_PLUGIN_SYSTEM_PATH). A manifest,
# gstreamer-runtime.txt, lists the elements, the plugins providing them and the DLLs resolved for them.
#
# The element -> plugin table below is static so that cross builds (MXE) can use it too. CI on Windows checks it
# against gst-inspect-1.0 (ci/windows/check-gstreamer-runtime.ps1).

set(_PSIMEDIA_GSTREAMER_RUNTIME_DIR "${CMAKE_CURRENT_LIST_DIR}")

# Elements psimedia cannot work without (gstthread.cpp checks most of them at start), with their plugins.
set(PSIMEDIA_GSTREAMER_REQUIRED_ELEMENTS
    "queue:coreelements" "tee:coreelements" "capsfilter:coreelements" "identity:coreelements"
    "fakesink:coreelements" "filesrc:coreelements"
    "appsrc:app" "appsink:app"
    "audioconvert:audioconvert" "audioresample:audioresample" "audiomixer:audiomixer"
    "volume:volume" "level:level"
    "videoconvert:videoconvertscale" "videoscale:videoconvertscale" "videorate:videorate"
    "decodebin:playback" "typefind:coreelements"
    "jpegdec:jpeg" "oggmux:ogg" "oggdemux:ogg"
    "opusenc:opus" "opusdec:opus" "vorbisenc:vorbis" "vorbisdec:vorbis" "vp8enc:vpx" "vp8dec:vpx"
    "rtpopuspay:rtp" "rtpopusdepay:rtp" "rtpvp8pay:rtp" "rtpvp8depay:rtp" "rtpvorbispay:rtp" "rtpvorbisdepay:rtp"
    "rtppcmupay:rtp" "rtppcmudepay:rtp" "rtph263ppay:rtp" "rtph263pdepay:rtp"
    "rtpjitterbuffer:rtpmanager" "rtpsession:rtpmanager"
    "mulawenc:mulaw" "mulawdec:mulaw"
    "directsoundsrc:directsoundsrc" "directsoundsink:directsound")
# Elements psimedia uses when present.
set(PSIMEDIA_GSTREAMER_OPTIONAL_ELEMENTS
    # decodebin finds the type of a played file through the typefind functions ("-": no element of its own)
    "-:typefindfunctions"
    # audio devices found by the device monitor
    "wasapisrc:wasapi" "wasapisink:wasapi" "wasapi2src:wasapi2" "wasapi2sink:wasapi2"
    # cameras
    "mfvideosrc:mediafoundation" "ksvideosrc:winks"
    # echo cancellation
    "webrtcdsp:webrtcdsp" "webrtcechoprobe:webrtcdsp"
    # screen sharing: Desktop Duplication / Windows Graphics Capture (Windows 8+), GDI fallback (Windows 7)
    "d3d11screencapturesrc:d3d11" "gdiscreencapsrc:winscreencap"
    # H.264 receive path
    "h264parse:videoparsersbad")

option(PSIMEDIA_INSTALL_GSTREAMER_RUNTIME
       "Install the prebuilt GStreamer runtime (plugins and DLLs from the SDK) next to the Windows media plugin" OFF)

function(psimedia_install_gstreamer_runtime target)
    if(NOT PSIMEDIA_INSTALL_GSTREAMER_RUNTIME)
        return()
    endif()
    if(NOT WIN32)
        message(FATAL_ERROR "PSIMEDIA_INSTALL_GSTREAMER_RUNTIME is meant for Windows packages")
    endif()
    if(CMAKE_VERSION VERSION_LESS 3.16)
        message(FATAL_ERROR "PSIMEDIA_INSTALL_GSTREAMER_RUNTIME needs CMake 3.16 or newer")
    endif()

    if(GST_SDK)
        set(default_bin "${GST_SDK}/bin")
        set(default_plugins "${GST_SDK}/lib/gstreamer-1.0")
    elseif(USE_MXE)
        set(default_bin "${CMAKE_PREFIX_PATH}/bin")
        set(default_plugins "${CMAKE_PREFIX_PATH}/bin/gstreamer-1.0")
    endif()
    set(PSIMEDIA_GSTREAMER_RUNTIME_BIN_DIR "${default_bin}" CACHE PATH "GStreamer SDK directory with runtime DLLs")
    set(PSIMEDIA_GSTREAMER_RUNTIME_PLUGIN_DIR "${default_plugins}" CACHE PATH "GStreamer SDK plugin directory")
    if(NOT IS_DIRECTORY "${PSIMEDIA_GSTREAMER_RUNTIME_BIN_DIR}"
       OR NOT IS_DIRECTORY "${PSIMEDIA_GSTREAMER_RUNTIME_PLUGIN_DIR}")
        message(FATAL_ERROR "GStreamer SDK directories not found: ${PSIMEDIA_GSTREAMER_RUNTIME_BIN_DIR}, "
                            "${PSIMEDIA_GSTREAMER_RUNTIME_PLUGIN_DIR}")
    endif()

    # The camera source the plugin was built for is required.
    set(required ${PSIMEDIA_GSTREAMER_REQUIRED_ELEMENTS})
    set(optional ${PSIMEDIA_GSTREAMER_OPTIONAL_ELEMENTS})
    foreach(entry IN LISTS optional)
        if(entry MATCHES "^${PSIMEDIA_WINDOWS_VIDEO_SOURCE}:")
            list(REMOVE_ITEM optional "${entry}")
            list(APPEND required "${entry}")
        endif()
    endforeach()

    # The built plugin's path is known at generation time only; install(CODE) would expand it only with CMP0087.
    set(location "${CMAKE_CURRENT_BINARY_DIR}/psimedia-gstreamer-runtime")
    file(GENERATE OUTPUT "${location}-$<CONFIG>.cmake" CONTENT "set(psimedia_gst_media_plugin \"$<TARGET_FILE:${target}>\")\n")
    install(CODE "
        set(psimedia_gst_required \"${required}\")
        set(psimedia_gst_optional \"${optional}\")
        set(psimedia_gst_bin_dir \"${PSIMEDIA_GSTREAMER_RUNTIME_BIN_DIR}\")
        set(psimedia_gst_plugin_dir \"${PSIMEDIA_GSTREAMER_RUNTIME_PLUGIN_DIR}\")
        set(psimedia_gst_dest \"${BIN_INSTALL_DIR}\")
        include(\"${location}-\${CMAKE_INSTALL_CONFIG_NAME}.cmake\")
        include(\"${_PSIMEDIA_GSTREAMER_RUNTIME_DIR}/PsimediaGStreamerRuntimeInstall.cmake\")
    " COMPONENT plugins)
endfunction()
