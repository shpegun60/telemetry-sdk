/*
 * @file Config.hpp
 * @brief Every project-wide SDK switch with its default, in one file.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 *
 * A project sets a switch in its own telemetry_config.hpp, found on the
 * include path like lwipopts.h or FreeRTOSConfig.h. The file is optional:
 * without it every default below applies. Each default is #ifndef-guarded,
 * so a compile definition (-D, CMake or CubeIDE symbols) still works; setting
 * one switch both ways is the project's error and the compiler reports the
 * redefinition. Every value joins the structured ABI tag, so translation
 * units built with different settings fail to link where compiled SDK code
 * is linked. A unit whose include path misses telemetry_config.hpp silently
 * takes the defaults.
 */
#ifndef TELEMETRY_CONFIG_HPP
#define TELEMETRY_CONFIG_HPP
#pragma once

#if __has_include(<telemetry_config.hpp>)
#include <telemetry_config.hpp>
#endif

// One setting for every translation unit in an executable. Zero forces all
// value/request/result payload objects through caller-owned Workspace.
// This is a payload-object budget, not the total call-chain stack bound.
#ifndef TELEMETRY_STRUCTURED_LOCAL_BYTES
#define TELEMETRY_STRUCTURED_LOCAL_BYTES 32
#endif

// 1 emits the optional name of every enum and struct (descriptor v3.1);
// 0 compiles no name into the image and the descriptor bytes are v3.0's.
// Hosts always read names; this switch only decides what a device emits.
#ifndef TELEMETRY_TYPE_NAMES
#define TELEMETRY_TYPE_NAMES 1
#endif

// TELEMETRY_VALUE_LIMITS joins here with the value-limits item.

#if TELEMETRY_TYPE_NAMES != 0 && TELEMETRY_TYPE_NAMES != 1
#error "TELEMETRY_TYPE_NAMES must be 0 or 1"
#endif

#endif
