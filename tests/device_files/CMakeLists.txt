# Installed device-files consumers: the reader without a network, and the
# files of a live session over TCP. MIT.
cmake_minimum_required(VERSION 3.21)
project(InstalledDeviceFiles LANGUAGES CXX)
find_package(Telemetry 0.1.0 EXACT CONFIG REQUIRED COMPONENTS files_client resource_protocol qt_connection
    tcp_server tcp_encoded_profile resource_adapter values_reader)
add_executable(reader Reader.cpp)
# Writing whole files in pieces and saving them, without a network.
add_executable(writer Writer.cpp)
# values.bin written as the device serves it, read back on the host.
add_executable(values Values.cpp)
target_link_libraries(values PRIVATE Telemetry::values_reader Telemetry::resource_adapter)
target_link_libraries(reader PRIVATE Telemetry::files_client Telemetry::resource_protocol Telemetry::encoded_backend)
target_link_libraries(writer PRIVATE Telemetry::files_client Telemetry::resource_protocol)
add_executable(session Session.cpp)
target_link_libraries(session PRIVATE Telemetry::qt_connection Telemetry::tcp_server Telemetry::tcp_encoded_profile
    Telemetry::resource_adapter)
add_executable(files_only FilesOnly.cpp)
target_link_libraries(files_only PRIVATE Telemetry::qt_connection Telemetry::tcp_server
    Telemetry::resource_adapter)
foreach(target session files_only)
    target_compile_definitions(${target} PRIVATE QT_NO_KEYWORDS)
endforeach()
foreach(target reader writer values session files_only)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
endforeach()
# GCC 13 at -O3 reports the always-emplaced std::optional inside the ReadMany
# Workbench completion as maybe-uninitialized once this test inlines it; the
# same code builds clean in tests/qt_read_many. GCC's known false positives on
# std::optional (GCC bug 80635). Keep it visible, not fatal, and drop this once
# the CI compiler no longer reports it.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(session PRIVATE -Wno-error=maybe-uninitialized)
    target_compile_options(files_only PRIVATE -Wno-error=maybe-uninitialized)
endif()
