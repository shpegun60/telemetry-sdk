# Independent C++20 modules and relocatable installation. MIT.
cmake_minimum_required(VERSION 3.20)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" TELEMETRY_SDK_VERSION LIMIT_COUNT 1)
project(Telemetry VERSION ${TELEMETRY_SDK_VERSION} LANGUAGES CXX)
set(TELEMETRY_SOURCE_REVISION "" CACHE STRING "Optional immutable source revision supplied by the SDK packager")
if(TELEMETRY_SOURCE_REVISION AND NOT TELEMETRY_SOURCE_REVISION MATCHES "^[0-9a-fA-F]+$")
    message(FATAL_ERROR "TELEMETRY_SOURCE_REVISION must be empty or a 40-digit Git revision")
endif()
string(LENGTH "${TELEMETRY_SOURCE_REVISION}" _revision_length)
if(TELEMETRY_SOURCE_REVISION AND NOT _revision_length EQUAL 40)
    message(FATAL_ERROR "TELEMETRY_SOURCE_REVISION must be empty or a 40-digit Git revision")
endif()
# Give GNUInstallDirs and the generated install/export rules the same lexical
# paths. Filesystem symlinks must not reinterpret a later uncollapsed "..".
foreach(_directory CMAKE_INSTALL_PREFIX CMAKE_INSTALL_INCLUDEDIR CMAKE_INSTALL_LIBDIR
        CMAKE_INSTALL_DATAROOTDIR CMAKE_INSTALL_DATADIR)
    if(DEFINED ${_directory})
        cmake_path(SET ${_directory} NORMALIZE "${${_directory}}")
    endif()
endforeach()
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
if(IS_ABSOLUTE "${CMAKE_INSTALL_INCLUDEDIR}" OR IS_ABSOLUTE "${CMAKE_INSTALL_LIBDIR}")
    message(FATAL_ERROR "Relocatable Telemetry modules require relative include and library install directories")
endif()

# Select only the modules an application needs. Dependencies are added below.
if(NOT DEFINED TELEMETRY_COMPONENTS)
    set(TELEMETRY_COMPONENTS "core;runtime;dynamic_client;read_many_client;read_many_static_client;batch_client;batch_static_client;inspector;workbench;read_many_workbench;resource_protocol;client;resource_adapter;encoded_server;batch_server;rpc_sessions;encoded_backend;rpc_backend;tcp_server"
        CACHE STRING "Telemetry modules to build and install")
endif()
include(cmake/Modules.cmake)

install(EXPORT TelemetryTargets NAMESPACE Telemetry:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Telemetry)
configure_package_config_file(cmake/TelemetryConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/TelemetryConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Telemetry)
set(_version_architecture ARCH_INDEPENDENT)
foreach(_component IN LISTS _telemetry_selected)
    get_target_property(_component_type telemetry_${_component} TYPE)
    if(NOT _component_type STREQUAL "INTERFACE_LIBRARY")
        set(_version_architecture "")
    endif()
endforeach()
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/TelemetryConfigVersion.cmake"
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMinorVersion ${_version_architecture})
configure_file(cmake/SDKMetadata.json.in "${CMAKE_CURRENT_BINARY_DIR}/SDKMetadata.json" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/TelemetryConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/TelemetryConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Telemetry)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/SDKMetadata.json" DESTINATION ${CMAKE_INSTALL_DATADIR}/telemetry)
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/telemetry)
if(TARGET telemetry_delegate)
    install(FILES lib/delegate/LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/telemetry/dependencies/delegate)
endif()
if(TARGET telemetry_reflection)
    install(FILES lib/boost_pfr/LICENSE_1_0.txt DESTINATION ${CMAKE_INSTALL_DATADIR}/telemetry/dependencies/boost_pfr)
    install(FILES lib/magic_enum/LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/telemetry/dependencies/magic_enum)
endif()
