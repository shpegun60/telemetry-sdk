/*
 * @file Reflection.hpp
 * @brief One stable entry point for structured reflection facts.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 */

/*
 * Public entry point for aggregate, callable, enum and type-name reflection.
 *
 * Consumers depend on these facades rather than pinned backend implementation
 * types. The combined include supplies structural facts only; it introduces
 * no endpoint values, ownership or transport state.
 */

#ifndef TELEMETRY_REFLECTION_HPP
#define TELEMETRY_REFLECTION_HPP
#pragma once

#include "Aggregate.hpp"
#include "Callable.hpp"
#include "Enum.hpp"
#include "TypeName.hpp"

#endif
