# The device's files over the Workbench's existing FIFO. MIT.
isEmpty(TELEMETRY_FILES_WORKBENCH_PRI_INCLUDED) {
    TELEMETRY_FILES_WORKBENCH_PRI_INCLUDED = 1
    include($$PWD/files_client.pri)
    include($$PWD/../workbench/workbench.pri)
    HEADERS *= $$PWD/Workbench.hpp
}
