# Explicit integration selection; dependency modules remain independent. MIT.
contains(CONFIG, telemetry_qt_transport) {
    include($$PWD/qt/qt_transport.pri)
}
contains(CONFIG, telemetry_qt_serial_transport) {
    include($$PWD/qt/qt_serial_transport.pri)
}
contains(CONFIG, telemetry_qt_tcp_transport) {
    include($$PWD/qt/qt_tcp_transport.pri)
}
contains(CONFIG, telemetry_qt_connection) {
    include($$PWD/qt/qt_connection.pri)
}
contains(CONFIG, telemetry_batch_server) {
    include($$PWD/batch/batch_server.pri)
}
contains(CONFIG, telemetry_batch_client) {
    include($$PWD/batch/batch.pri)
}
contains(CONFIG, telemetry_files_client) {
    include($$PWD/files/files_client.pri)
}
contains(CONFIG, telemetry_files_workbench) {
    include($$PWD/files/files_workbench.pri)
}
contains(CONFIG, telemetry_batch_workbench) {
    include($$PWD/batch/batch_workbench.pri)
}
contains(CONFIG, telemetry_tcp_encoded_profile) {
    include($$PWD/tcp/tcp_encoded_profile.pri)
}
contains(CONFIG, telemetry_batch_static_client) {
    include($$PWD/batch/static_client.pri)
}
contains(CONFIG, telemetry_read_many_server) {
    CONFIG += resource_telemetry_encoded
}
contains(CONFIG, telemetry_read_many_client) {
    include($$PWD/read_many/read_many.pri)
}
contains(CONFIG, telemetry_read_many_static_client) {
    include($$PWD/read_many/static_client.pri)
}
contains(CONFIG, telemetry_read_many_workbench) {
    include($$PWD/read_many/read_many_workbench.pri)
}
contains(CONFIG, telemetry_tcp_server) {
    include($$PWD/tcp/tcp.pri)
}
contains(CONFIG, telemetry_encoded_backend) {
    include($$PWD/../client/client.pri)
    include($$PWD/../wire/wire.pri)
    HEADERS *= $$PWD/encoded/Backend.hpp $$PWD/encoded/RetryingBackend.hpp
}
contains(CONFIG, telemetry_rpc_backend) {
    CONFIG += telemetry_client_rpc
    include($$PWD/../client/client.pri)
    HEADERS *= $$PWD/rpc/Backend.hpp
}
contains(CONFIG, resource_telemetry):isEmpty(TELEMETRY_RESOURCE_ADAPTER_PRI_INCLUDED) {
    TELEMETRY_RESOURCE_ADAPTER_PRI_INCLUDED = 1
    include($$PWD/../telemetry/telemetry.pri)
    include($$PWD/../resource/resource.pri)
    HEADERS += $$PWD/telemetry/v3/DescriptorFile.hpp \
        $$PWD/telemetry/v3/ValuesFile.hpp \
        $$PWD/telemetry/v3/ValuesFormat.hpp \
        $$files($$PWD/telemetry/v3/detail/*.hpp)
    SOURCES += $$PWD/telemetry/v3/detail/Values.cpp
}
contains(CONFIG, resource_telemetry_rpc)|contains(CONFIG, resource_telemetry_rpc_sessions) {
    isEmpty(TELEMETRY_RPC_SERVER_PRI_INCLUDED) {
        TELEMETRY_RPC_SERVER_PRI_INCLUDED = 1
        include($$PWD/../telemetry/telemetry.pri)
        include($$PWD/../resource/resource.pri)
        include($$PWD/../wire/wire.pri)
        HEADERS += $$PWD/telemetry/v3/RpcMailbox.hpp
        SOURCES += $$PWD/telemetry/v3/RpcMailbox.cpp
    }
}
contains(CONFIG, resource_telemetry_rpc_sessions) {
    HEADERS *= $$PWD/telemetry/v3/RpcSessionPool.hpp
}
contains(CONFIG, resource_telemetry_encoded):isEmpty(TELEMETRY_ENCODED_SERVER_PRI_INCLUDED) {
    TELEMETRY_ENCODED_SERVER_PRI_INCLUDED = 1
    include($$PWD/../telemetry/telemetry.pri)
    include($$PWD/../wire/wire.pri)
    HEADERS += $$PWD/encoded/Dispatcher.hpp
    SOURCES *= $$PWD/encoded/Dispatcher.cpp
}
# Optional server sources also support opt-in after Encoded was already included.
contains(CONFIG, telemetry_read_many_server) {
    include($$PWD/../telemetry/telemetry.pri)
    SOURCES *= $$PWD/encoded/ReadManyDispatcher.cpp
}
