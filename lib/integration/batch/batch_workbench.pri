# Optional Mixed Batch bridge, one existing Workbench/FIFO. MIT.
isEmpty(TELEMETRY_BATCH_WORKBENCH_PRI_INCLUDED) {
    TELEMETRY_BATCH_WORKBENCH_PRI_INCLUDED = 1
    include($$PWD/batch.pri)
    include($$PWD/../workbench/workbench.pri)
    HEADERS *= $$PWD/Workbench.hpp
}
