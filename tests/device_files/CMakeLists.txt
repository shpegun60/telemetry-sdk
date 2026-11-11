# Installed device-files consumers: the reader without a network, and the
# files of a live session over TCP. MIT.
cmake_minimum_required(VERSION 3.21)
project(InstalledDeviceFiles LANGUAGES CXX)
find_package(Telemetry 0.1.0 EXACT CONFIG REQUIRED COMPONENTS files_client resource_protocol qt_connection
    tcp_server)
add_executable(reader Reader.cpp)
target_link_libraries(reader PRIVATE Telemetry::files_client Telemetry::resource_protocol)
add_executable(session Session.cpp)
target_link_libraries(session PRIVATE Telemetry::qt_connection Telemetry::tcp_server)
target_compile_definitions(session PRIVATE QT_NO_KEYWORDS)
foreach(target reader session)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
endforeach()
# GCC 13 at -O3 reports the always-emplaced std::optional inside the ReadMany
# Workbench completion as maybe-uninitialized once this test inlines it; the
# same code builds clean in tests/qt_read_many. Keep it visible, not fatal.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(session PRIVATE -Wno-error=maybe-uninitialized)
endif()
