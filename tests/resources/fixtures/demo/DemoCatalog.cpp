/*
 * Advance the simulated measurements used by Resource facade checks.
 *
 * Keep changing sample state separate from the endpoint definitions. The test
 * consumer calls advance() serially, so readers and commands observe ordinary
 * application objects without an internal synchronization layer.
 *
 * Authors: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.
 */

#include "DemoCatalog.h"

namespace demo {

double Sensor::temperature() const noexcept
{
	return temperature_;
}

bool Sensor::enabled() const noexcept
{
	return enabled_;
}

void Sensor::advance() noexcept
{
	temperature_ = temperature_ >= 26.0 ? 24.0 : temperature_ + 0.25;
	enabled_ = !enabled_;
}

void advance() noexcept
{
	++meter.counter;
	meter.voltage = 230.0f + static_cast<float>(meter.counter % 5) * 0.5f;
	meter.current = 2.0f + static_cast<float>(meter.counter % 3) * 0.1f;
	sensor.advance();
}

} // namespace demo
