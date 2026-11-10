/* Run the real client/server lifecycle suite with allocation traps. MIT. */
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <new>
static bool allocationArmed = false;

static void refuseAllocation() noexcept
{
	if (allocationArmed)
		std::_Exit(86);
}
#ifdef BATCH_WRAP_ALLOCATIONS
extern "C" {
void* __real_malloc(std::size_t);
void* __real_calloc(std::size_t, std::size_t);
void* __real_realloc(void*, std::size_t);
void __real_free(void*);

void* __wrap_malloc(std::size_t n)
{
	refuseAllocation();
	return __real_malloc(n);
}

void* __wrap_calloc(std::size_t n, std::size_t s)
{
	refuseAllocation();
	return __real_calloc(n, s);
}

void* __wrap_realloc(void* p, std::size_t n)
{
	refuseAllocation();
	return __real_realloc(p, n);
}

void __wrap_free(void* p)
{
	refuseAllocation();
	__real_free(p);
}
}
#endif
void* operator new(std::size_t n)
{
	refuseAllocation();
	if (auto* p = std::malloc(n ? n : 1))
		return p;
	std::abort();
}

void* operator new[](std::size_t n)
{
	return ::operator new(n);
}

void* operator new(std::size_t, std::align_val_t)
{
	std::_Exit(86);
}

void* operator new[](std::size_t n, std::align_val_t a)
{
	return ::operator new(n, a);
}

void operator delete(void* p) noexcept
{
	if (p)
		refuseAllocation();
	std::free(p);
}

void operator delete[](void* p) noexcept
{
	::operator delete(p);
}

void operator delete(void* p, std::size_t) noexcept
{
	::operator delete(p);
}

void operator delete[](void* p, std::size_t) noexcept
{
	::operator delete(p);
}

void operator delete(void* p, std::align_val_t) noexcept
{
	::operator delete(p);
}

void operator delete[](void* p, std::align_val_t) noexcept
{
	::operator delete(p);
}

void operator delete(void* p, std::size_t, std::align_val_t) noexcept
{
	::operator delete(p);
}

void operator delete[](void* p, std::size_t, std::align_val_t) noexcept
{
	::operator delete(p);
}

#define main batchClientMain
#include "Workbench.cpp"
#undef main

int main(int argc, char** argv)
{
	void* freeControl = std::malloc(8);
	allocationArmed = true;
	if (argc == 2) {
		const char* control = argv[1];
		// Volatile function pointers prevent optimizer removal of the controls.
		if (std::strcmp(control, "new") == 0) {
			void* (*volatile fn)(std::size_t) =
			    static_cast<void* (*)(std::size_t)>(&::operator new);
			(void)fn(8);
		} else if (std::strcmp(control, "aligned_new") == 0) {
			void* (*volatile fn)(std::size_t, std::align_val_t) = &::operator new;
			(void)fn(64, std::align_val_t{64});
		} else if (std::strcmp(control, "malloc") == 0) {
			void* (*volatile fn)(std::size_t) = &std::malloc;
			(void)fn(8);
		} else if (std::strcmp(control, "calloc") == 0) {
			void* (*volatile fn)(std::size_t, std::size_t) = &std::calloc;
			(void)fn(1, 8);
		} else if (std::strcmp(control, "realloc") == 0) {
			void* (*volatile fn)(void*, std::size_t) = &std::realloc;
			(void)fn(freeControl, 16);
		} else if (std::strcmp(control, "free") == 0) {
			void (*volatile fn)(void*) = &std::free;
			fn(freeControl);
		}
		return 1;
	}
	const auto result = batchClientMain();
	allocationArmed = false;
	std::free(freeControl);
	return result;
}
