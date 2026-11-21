# A device Model built from branch files, with named handles and path access. MIT.
# The example is header-only on the SDK side: it calls typed native access and
# decodes its own descriptor, so no compiled SDK source is linked.
cmake_minimum_required(VERSION 3.20)
project(telemetry_group_tree LANGUAGES CXX)
set(TELEMETRY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
add_executable(group_tree main.cpp)
target_compile_features(group_tree PRIVATE cxx_std_20)
set_target_properties(group_tree PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(group_tree PRIVATE
    "${TELEMETRY_ROOT}/lib"
    "${TELEMETRY_ROOT}/lib/boost_pfr/include"
    "${TELEMETRY_ROOT}/lib/magic_enum")
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(group_tree PRIVATE -Wall -Wextra -Werror -pedantic-errors)
elseif(MSVC)
    target_compile_options(group_tree PRIVATE /W4 /WX)
endif()
enable_testing()
add_test(NAME group_tree COMMAND group_tree)
