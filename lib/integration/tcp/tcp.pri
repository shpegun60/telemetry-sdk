# Explicit native TCP stream integration; no Qt, OS, socket or RPC dependency. MIT.
isEmpty(TELEMETRY_TCP_PRI_INCLUDED) {
    TELEMETRY_TCP_PRI_INCLUDED = 1
    CONFIG += resource_telemetry_encoded resource_protocol
    include($$PWD/../integration.pri)
    include($$PWD/../../resource/resource.pri)
    HEADERS *= $$PWD/Framing.hpp $$PWD/Server.hpp $$PWD/ReadManyProfile.hpp
}
