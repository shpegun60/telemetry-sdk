/**
 * @file telemetry_config.hpp
 * @brief The project's Telemetry switches for this firmware; every line is optional.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 *
 * lib/telemetry/Config.hpp includes this file when the compiler finds it on
 * the include path (CMakeLists.txt adds this directory for every source of
 * the image), then gives each switch not set here its default. Set a switch
 * here or with -D, not both: the compiler reports the redefinition. Every
 * translation unit of the image must see the same values; the structured ABI
 * tag turns a mismatch into a link error where compiled SDK sources are
 * linked, as they are here.
 *
 * This demonstration keeps every default, so the lines below are comments.
 */
#ifndef TELEMETRY_EXAMPLES_NUCLEO_H7S3L8_TELEMETRY_CONFIG_HPP
#define TELEMETRY_EXAMPLES_NUCLEO_H7S3L8_TELEMETRY_CONFIG_HPP
#pragma once

// Type names in the descriptor (v3.1). With the default 1 the Inspector shows
// "enum nucleo_demo::Mode" and "struct nucleo_demo::Probe"; the names are the
// innermost namespace and the type's own name, taken from the code, and a
// TypeReflection<T> specialisation can replace or remove one. 0 leaves every
// name out: the descriptor is then byte for byte the v3.0 one, which hosts
// built before descriptor v3.1 need, and the packed descriptor is 90 bytes
// smaller.
// #define TELEMETRY_TYPE_NAMES 0

// The payload-object budget in bytes (default 32): a value, request or result
// object up to this size may live locally, a larger one goes through the
// caller's Workspace; 0 sends every one through it.
// #define TELEMETRY_STRUCTURED_LOCAL_BYTES 32

#endif
