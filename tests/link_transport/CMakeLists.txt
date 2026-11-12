# Installed link transport: the Qt-free framer, a link that is not a socket,
# Resource requests after discovery, and the TCP session traced against a
# golden. MIT.
cmake_minimum_required(VERSION 3.21)
project(InstalledLinkTransport LANGUAGES CXX)
find_package(Telemetry 0.1.0 EXACT CONFIG REQUIRED COMPONENTS tcp_link qt_connection tcp_server)
add_executable(framer Framer.cpp)
target_link_libraries(framer PRIVATE Telemetry::tcp_link)
add_executable(channel Channel.cpp)
target_link_libraries(channel PRIVATE Telemetry::qt_transport Telemetry::tcp_link Telemetry::tcp_server)
add_executable(resource Resource.cpp)
target_link_libraries(resource PRIVATE Telemetry::qt_tcp_transport Telemetry::encoded_backend
    Telemetry::tcp_server)
add_executable(trace Trace.cpp)
target_link_libraries(trace PRIVATE Telemetry::qt_connection Telemetry::tcp_server)
foreach(target channel resource trace)
    target_compile_definitions(${target} PRIVATE QT_NO_KEYWORDS)
endforeach()
foreach(target framer channel resource trace)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    # GCC 15 reports maybe-uninitialized inside libstdc++'s shared_ptr once a
    # readMany completion is inlined through std::optional into a delegate's
    # inline storage: a false positive located in a system header, which no
    # pragma at our call site reaches.
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 15)
        target_compile_options(${target} PRIVATE -Wno-maybe-uninitialized)
    endif()
endforeach()
