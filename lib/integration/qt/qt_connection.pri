# Optional host Qt Session, one existing native Workbench/FIFO. MIT.
isEmpty(TELEMETRY_QT_CONNECTION_PRI_INCLUDED) {
    TELEMETRY_QT_CONNECTION_PRI_INCLUDED = 1
    include($$PWD/qt_tcp_transport.pri)
    include($$PWD/../workbench/workbench.pri)
    CONFIG += telemetry_encoded_backend
    include($$PWD/../integration.pri)
    HEADERS *= $$PWD/ConnectionStorage.hpp $$PWD/ConnectionSession.hpp
    SOURCES *= $$PWD/ConnectionStorage.cpp $$PWD/ConnectionSession.cpp
}
