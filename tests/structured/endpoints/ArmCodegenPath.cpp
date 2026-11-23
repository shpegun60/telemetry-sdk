/*
 * @file ArmCodegenPath.cpp
 * @brief Path-route native codegen of the mixed reads, in its own unit.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 */
#include <telemetry/model/Model.hpp>
#include "MixedFixture.hpp"
namespace ts = telemetry;

#define PROBE extern "C" __attribute__((noinline))

// The path route of ArmCodegen.cpp's mixed reads. A handle and a path reading
// the tree's table share one read<I>; in one unit GCC 13 at -Os keeps it out
// of line, so each route is compiled where it is the only caller.
#define MIXED_PATH_READ(label, path, Value)                                                        \
	PROBE std::optional<Value> label##_path() noexcept                                             \
	{                                                                                              \
		return ts::read<fixture::model, path>();                                                   \
	}
MIXED_PATH_READ(boolean, "motor/Enabled", bool)
MIXED_PATH_READ(u16, "motor/RPM", std::uint16_t)
MIXED_PATH_READ(f32, "motor/Temperature", float)
MIXED_PATH_READ(f64, "motor/Precise", double)
MIXED_PATH_READ(enumeration, "motor/Mode", fixture::Mode)
using Samples = std::array<std::uint16_t, 3>;
MIXED_PATH_READ(array, "motor/Samples", Samples)
MIXED_PATH_READ(state, "motor/State", fixture::State)
MIXED_PATH_READ(config, "motor/Config", fixture::MotorConfig)
