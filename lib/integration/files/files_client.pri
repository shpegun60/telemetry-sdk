# The device's files over the client's one queue: list, stat, read in pieces. MIT.
isEmpty(TELEMETRY_FILES_CLIENT_PRI_INCLUDED) {
    TELEMETRY_FILES_CLIENT_PRI_INCLUDED = 1
    include($$PWD/../../client/client.pri)
    HEADERS *= $$PWD/Client.hpp $$PWD/../../resource/Types.hpp \
        $$PWD/../../resource/protocol/Wire.hpp $$PWD/../../resource/protocol/Packets.hpp
}
