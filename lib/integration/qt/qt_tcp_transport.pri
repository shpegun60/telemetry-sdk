# Optional Qt Network Resource/Encoded TCP carrier. MIT.
isEmpty(TELEMETRY_QT_TCP_TRANSPORT_PRI_INCLUDED) {
    TELEMETRY_QT_TCP_TRANSPORT_PRI_INCLUDED = 1
    include($$PWD/qt_transport.pri)
    QT += network
    HEADERS *= $$PWD/TcpTransport.hpp $$PWD/../tcp/Framing.hpp
    SOURCES *= $$PWD/TcpTransport.cpp
}
