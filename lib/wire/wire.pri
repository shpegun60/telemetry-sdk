# Shared byte contracts; no client, server or filesystem implementation. MIT.
isEmpty(TELEMETRY_WIRE_PRI_INCLUDED) {
    TELEMETRY_WIRE_PRI_INCLUDED = 1
    CONFIG -= c++11 c++14 c++17
    CONFIG += c++20
    INCLUDEPATH += $$PWD/..
    HEADERS += $$PWD/EncodedWire.hpp $$PWD/ReadManyWire.hpp $$PWD/BatchWire.hpp $$PWD/IdentityWire.hpp $$PWD/RpcWire.hpp
}
