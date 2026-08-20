include(CheckCXXSourceCompiles)

# Bundled QCA tracks psi-im/qca master and is part of the same source stack, so
# its feature set is known before ExternalProject has downloaded/built it.
# System QCA is probed by API, not by major version: downstream QCA 2 builds may
# carry selected backports during the migration period.
if(IRIS_BUNDLED_QCA)
    set(IRIS_QCA_HAS_DTLS ON CACHE INTERNAL "QCA has Datagram TLS API" FORCE)
    set(IRIS_QCA_HAS_CHANNEL_BINDING ON CACHE INTERNAL "QCA has TLS/SASL channel-binding API" FORCE)
else()
    # check_cxx_source_compiles caches results. Re-run probes if the caller
    # switches between QCA 2 and QCA 3 in an existing build directory.
    unset(IRIS_QCA_HAS_DTLS CACHE)
    unset(IRIS_QCA_HAS_CHANNEL_BINDING CACHE)

    set(_iris_saved_required_libraries "${CMAKE_REQUIRED_LIBRARIES}")
    set(_iris_saved_try_compile_target_type "${CMAKE_TRY_COMPILE_TARGET_TYPE}")
    set(CMAKE_REQUIRED_LIBRARIES ${IRIS_QCA_TARGET})

    # These probes only inspect the QCA headers/API.  Compile a static test
    # library instead of linking an executable so callers may provide QCA
    # through an ExternalProject whose binary does not exist yet at configure
    # time (for example AnyKeep's bundled QCA).
    set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

    check_cxx_source_compiles([=[
        #include <QtCrypto>
        #include <type_traits>
        static_assert(std::is_constructible<QCA::TLS, QCA::TLS::Mode>::value,
                      "QCA::TLS datagram constructor is unavailable");
        int main()
        {
            const QCA::TLS::Mode mode = QCA::TLS::Datagram;
            (void)mode;
            return 0;
        }
    ]=] IRIS_QCA_HAS_DTLS)

    check_cxx_source_compiles([=[
        #include <QtCrypto>
        #include <utility>
        using TlsTypes = decltype(std::declval<const QCA::TLS &>().channelBindingTypes());
        using TlsDefault = decltype(std::declval<const QCA::TLS &>().defaultChannelBindingType());
        using TlsBinding = decltype(std::declval<const QCA::TLS &>().channelBinding(std::declval<const QString &>()));
        using SaslSupports = decltype(std::declval<const QCA::SASL &>().supportsChannelBinding());
        using SaslSet = decltype(std::declval<QCA::SASL &>().setChannelBinding(
            std::declval<const QString &>(), std::declval<const QByteArray &>(), false));
        int main() { return 0; }
    ]=] IRIS_QCA_HAS_CHANNEL_BINDING)

    set(CMAKE_REQUIRED_LIBRARIES "${_iris_saved_required_libraries}")
    if(_iris_saved_try_compile_target_type)
        set(CMAKE_TRY_COMPILE_TARGET_TYPE "${_iris_saved_try_compile_target_type}")
    else()
        unset(CMAKE_TRY_COMPILE_TARGET_TYPE)
    endif()
endif()

if(IRIS_QCA_HAS_DTLS)
    set(IRIS_QCA_HAS_DTLS 1 CACHE INTERNAL "QCA has Datagram TLS API" FORCE)
else()
    set(IRIS_QCA_HAS_DTLS 0 CACHE INTERNAL "QCA has Datagram TLS API" FORCE)
endif()
if(IRIS_QCA_HAS_CHANNEL_BINDING)
    set(IRIS_QCA_HAS_CHANNEL_BINDING 1 CACHE INTERNAL "QCA has TLS/SASL channel-binding API" FORCE)
else()
    set(IRIS_QCA_HAS_CHANNEL_BINDING 0 CACHE INTERNAL "QCA has TLS/SASL channel-binding API" FORCE)
endif()

message(STATUS "QCA features: DTLS=${IRIS_QCA_HAS_DTLS}, channel-binding=${IRIS_QCA_HAS_CHANNEL_BINDING}")
