# Installed optional TCP/Qt ReadMany profile and session consumers. MIT.
cmake_minimum_required(VERSION 3.21)
project(InstalledQtReadMany LANGUAGES CXX)
find_package(Telemetry 0.1 CONFIG REQUIRED COMPONENTS qt_connection tcp_server)
add_executable(profile Native.cpp)
target_link_libraries(profile PRIVATE Telemetry::tcp_server)
add_executable(session Session.cpp)
target_link_libraries(session PRIVATE Telemetry::qt_connection Telemetry::tcp_server)
target_compile_definitions(session PRIVATE QT_NO_KEYWORDS)
foreach(target profile session)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    # GCC 15 reports maybe-uninitialized inside libstdc++'s shared_ptr once a
    # readMany completion is inlined through std::optional into a delegate's
    # inline storage: a false positive located in a system header, which no
    # pragma at our call site reaches.
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 15)
        target_compile_options(${target} PRIVATE -Wno-maybe-uninitialized)
    endif()
endforeach()
