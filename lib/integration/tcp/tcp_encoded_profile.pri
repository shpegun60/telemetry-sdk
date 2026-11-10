# Explicit optional TRM1/TBM1 Resource/TCP profile. MIT.
isEmpty(TELEMETRY_TCP_ENCODED_PROFILE_PRI_INCLUDED) {
    TELEMETRY_TCP_ENCODED_PROFILE_PRI_INCLUDED = 1
    CONFIG += telemetry_batch_server telemetry_read_many_server
    include($$PWD/tcp.pri)
    include($$PWD/../integration.pri)
    HEADERS *= $$PWD/EncodedProfile.hpp
}
