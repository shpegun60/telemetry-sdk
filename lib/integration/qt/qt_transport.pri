# Optional Qt Core transport contract, link transport and Resource bootstrap. MIT.
isEmpty(TELEMETRY_QT_TRANSPORT_PRI_INCLUDED) {
    TELEMETRY_QT_TRANSPORT_PRI_INCLUDED = 1
    QT += core
    include($$PWD/../../client/client.pri)
    include($$PWD/../../wire/wire.pri)
    HEADERS *= $$PWD/DeviceTransport.hpp $$PWD/DescriptorBootstrap.hpp \
        $$PWD/Link.hpp $$PWD/LinkTransport.hpp $$PWD/../link/Framer.hpp \
        $$PWD/../../resource/Types.hpp $$PWD/../../resource/protocol/Wire.hpp \
        $$PWD/../../resource/protocol/Packets.hpp $$PWD/../../telemetry/type/WireTypes.hpp
    SOURCES *= $$PWD/LinkTransport.cpp
}
