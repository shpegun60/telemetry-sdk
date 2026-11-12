# Installed public Qt/TCP Mixed Batch consumer; no UX or board. MIT.
cmake_minimum_required(VERSION 3.21)
project(InstalledQtBatch LANGUAGES CXX)
find_package(Telemetry 0.1.0 EXACT CONFIG REQUIRED COMPONENTS qt_connection tcp_encoded_profile)
add_executable(session Session.cpp)
target_link_libraries(session PRIVATE Telemetry::qt_connection Telemetry::tcp_encoded_profile)
target_compile_definitions(session PRIVATE QT_NO_KEYWORDS)
target_compile_options(session PRIVATE -Wall -Wextra -Werror)

add_executable(profile Native.cpp)
target_link_libraries(profile PRIVATE Telemetry::tcp_encoded_profile)
add_executable(workbench Workbench.cpp)
target_link_libraries(workbench PRIVATE Telemetry::batch_workbench Telemetry::encoded_backend Telemetry::batch_server)
add_executable(noheap NoHeap.cpp)
target_link_libraries(noheap PRIVATE Telemetry::batch_workbench Telemetry::encoded_backend Telemetry::batch_server)
if(UNIX)
    target_compile_definitions(noheap PRIVATE BATCH_WRAP_ALLOCATIONS=1)
    target_link_options(noheap PRIVATE -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc -Wl,--wrap=free)
endif()
foreach(target IN ITEMS profile workbench noheap)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
endforeach()
# GCC 15 reports maybe-uninitialized inside libstdc++'s shared_ptr once a
# completion is inlined through std::optional into a delegate's inline storage:
# a false positive located in a system header, which no pragma here reaches.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 15)
    foreach(target IN ITEMS session profile workbench noheap)
        target_compile_options(${target} PRIVATE -Wno-maybe-uninitialized)
    endforeach()
endif()

add_executable(legacy Legacy.cpp)
target_link_libraries(legacy PRIVATE Telemetry::tcp_server)
target_compile_options(legacy PRIVATE -Wall -Wextra -Werror)
