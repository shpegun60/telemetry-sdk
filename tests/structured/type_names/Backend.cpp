/*
 * @file Backend.cpp
 * @brief Reports whether a compiler could build the std::meta type-name backend.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 *
 * The cross-backend byte test compares today's backend with std::meta on every
 * reflection-capable compiler. A compiler qualifies only when it defines both
 * __cpp_impl_reflection and __cpp_lib_reflection. run.py reports the result
 * as "not established" whenever no compiler qualifies, or when one does but
 * the std::meta backend is not written yet: a visible skip, never a pass.
 */

#include <telemetry/reflection/TypeName.hpp>

#include <cstdio>

#if defined(CROSS_ARM)
extern "C" {
__attribute__((used, section(".probe.backend"))) const unsigned char backend_probe =
    telemetry::reflection::standardReflectionAvailable ? 1 : 0;
}
#else
int main()
{
	std::printf("standardReflectionAvailable=%d\n",
	            telemetry::reflection::standardReflectionAvailable ? 1 : 0);
	return 0;
}
#endif
