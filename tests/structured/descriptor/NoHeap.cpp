/*
 * @file NoHeap.cpp
 * @brief Runtime construction/read must not require C++ dynamic allocation.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 */
#include "Fixture.hpp"
#include <cstdlib>
#include <new>

void* operator new(std::size_t)
{
	std::abort();
}

void* operator new[](std::size_t)
{
	std::abort();
}

void* operator new(std::size_t, std::align_val_t)
{
	std::abort();
}

void* operator new[](std::size_t, std::align_val_t)
{
	std::abort();
}

void operator delete(void*) noexcept
{
	std::abort();
}

void operator delete[](void*) noexcept
{
	std::abort();
}

void operator delete(void*, std::size_t) noexcept
{
	std::abort();
}

void operator delete[](void*, std::size_t) noexcept
{
	std::abort();
}

void operator delete(void*, std::align_val_t) noexcept
{
	std::abort();
}

void operator delete[](void*, std::align_val_t) noexcept
{
	std::abort();
}

void operator delete(void*, std::size_t, std::align_val_t) noexcept
{
	std::abort();
}

void operator delete[](void*, std::size_t, std::align_val_t) noexcept
{
	std::abort();
}

int main()
{
	const auto d = ::telemetry::descriptor::withoutTypeNames(descriptor_fixture::model);
	std::array<std::byte, descriptor_fixture::edge.size()> out;
	const auto result = d.read(0, out);
	return d.valid() && result.eof && out == descriptor_fixture::edgeBytes ? 0 : 1;
}
