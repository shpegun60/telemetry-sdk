/**
 * @file Resource.hpp
 * @brief Public generic resource API, without protocol or telemetry adapters.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * License: MIT; see LICENSE.
 *
 * Collect the generic file API for consumers that do not need a packet
 * protocol or structural telemetry providers. Including this header does
 * not select a transport or add compiled implementation code.
 */

#ifndef TELEMETRY_LIB_RESOURCE_RESOURCE_HPP
#define TELEMETRY_LIB_RESOURCE_RESOURCE_HPP
#pragma once

#include "Types.hpp"
#include "File.hpp"
#include "FileView.hpp"
#include "FileSystem.hpp"
#include "BytesFile.hpp"
#include "ChunkWriter.hpp"

#endif // TELEMETRY_LIB_RESOURCE_RESOURCE_HPP
