// Compile-time contract rejections. Authors: Ruslan Kovtun, codexAi (MIT).
// Checks compile-time resource-provider and borrowed-path contracts with positive binding controls.
// Owner identity, noexcept callbacks and exact size types are tested separately from temporary conversion refusal.

#include <resource/FileSystem.hpp>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>

// Stable provider records the exact borrowed owner used by positive controls.
// Public methods:
// - size(): Report provider extent.
// - read(): Record borrowed identity.
struct Provider {
	resource::FileSize value = 0;
	mutable const Provider* lastRead = nullptr;

	resource::FileSize size() const noexcept
	{
		return value;
	}

	resource::ReadResult read(resource::Cursor cursor, resource::Output output) const noexcept
	{
		lastRead = this;
		if (output.empty())
			return {resource::Status::BufferTooSmall, cursor};
		output[0] = static_cast<std::byte>(value);
		return {resource::Status::Ok, cursor + 1, 1, true};
	}
} provider;

// Actual derived provider supplies the valid base-adjustment control.
struct Derived : Provider {
} derived;

// Temporary holder would expose a provider reference to its own subobject.
// Public methods:
// - operator const Provider&(): Expose borrowed subobject.
struct Holder {
	Provider value;

	operator const Provider&() const noexcept
	{
		return value;
	}
};

// Value-conversion proxy would create a provider temporary during binding.
// Public methods:
// - operator Provider(): Manufacture temporary provider.
struct Proxy {
	operator Provider() const noexcept
	{
		return {};
	}
} proxy;

#if CASE == 1
auto bad = resource::file("/temporary", Provider{});
#elif CASE == 2
constexpr auto bad =
    resource::filesystem(resource::file("/same", provider), resource::file("/same", provider));
#elif CASE == 3
constexpr auto bad = resource::file("relative", provider);
#elif CASE == 4
constexpr auto bad = resource::file("/trailing/", provider);
#elif CASE == 5
constexpr auto bad = resource::file("/empty//part", provider);
#elif CASE == 6
constexpr auto bad = resource::file("/a/../b", provider);
#elif CASE == 7
auto bad = resource::filesystem(resource::file("/temporary-view", provider)).view();
#elif CASE == 8
auto bad = resource::file(std::string{"/temporary-path"}, provider);
#elif CASE == 9
// Size-only type lacks the read or write operation required of a provider.
// Public methods:
// - size(): Report empty extent.
struct Missing {
	resource::FileSize size() const noexcept
	{
		return 0;
	}
} missing;

auto bad = resource::file("/missing", missing);
#elif CASE == 10
// Throwing size method violates the provider noexcept contract.
// Public methods:
// - size(): Expose throwing size.
// - read(): Provide read signature.
struct Throwing {
	resource::FileSize size() const
	{
		return 0;
	}

	resource::ReadResult read(resource::Cursor, resource::Output) const noexcept
	{
		return {};
	}
} throwing;

auto bad = resource::file("/throwing", throwing);
#elif CASE == 11
// Wide size result violates the exact provider FileSize contract.
// Public methods:
// - size(): Expose wide size.
// - read(): Provide read signature.
struct Wide {
	std::uint64_t size() const noexcept
	{
		return 0;
	}

	resource::ReadResult read(resource::Cursor, resource::Output) const noexcept
	{
		return {};
	}
} wide;

auto bad = resource::file("/narrowing", wide);
#elif CASE == 12
constexpr auto bad = resource::file("/control\x7f", provider);
#elif CASE == 13
auto bad = resource::file<const Provider>("/converted-temporary", Holder{});
#elif CASE == 14
auto bad = resource::file<const Provider>("/converted-lvalue", proxy);
#elif CASE == 15
auto bad = resource::file<Provider>(std::string{"/explicit-temporary-path"}, provider);
#elif CASE == 16
auto bad = resource::file<const Provider>(std::string{"/explicit-base-path"}, derived);
#elif CASE == 17
std::string prefix = "/prefix";
auto bad = resource::file<const Provider>(prefix + "/name", derived);
#elif CASE == 18
const std::string makePath();
auto bad = resource::file<const Provider>(makePath(), provider);
#elif CASE == 19
std::string path = "/moved-path";
auto bad = resource::file<Provider>(std::move(path), provider);
#elif CASE == 20
auto bad = resource::file<const Provider>("/empty-braces", {});
#elif CASE == 21
auto bad = resource::file<const Provider>("/braced-temporary", {Provider{}});
#elif CASE == 22
auto bad = resource::file<const Provider>("/braced-derived", {Derived{}});
#elif CASE == 23
auto bad = resource::file<const Provider>("/braced-reference-conversion", {Holder{}});
#elif CASE == 24
auto bad = resource::file<const Provider>("/braced-lvalue-conversion", {proxy});
#elif CASE == 25
auto bad = resource::file<const Provider>("/braced-value-conversion", {Proxy{}});
#elif CASE == 26
auto bad = resource::file<const Provider>(std::string{"/braced-provider-path"}, {provider});
#elif CASE == 27
auto bad = resource::file<const Provider>(std::string{"/braced-derived-path"}, {derived});
#elif CASE == 28
auto bad = resource::file<const Provider>("/explicit-temporary", Provider{});
#elif CASE == 29
auto bad = resource::file<const Provider>("/explicit-derived", Derived{});
#elif CASE == 30
auto bad = resource::file({std::string{"/braced-owning-path"}}, provider);
#elif CASE == 31
auto bad = resource::file<Provider>({std::string{"/explicit-braced-path"}}, provider);
#elif CASE == 32
auto bad = resource::file<const Provider>({std::string{"/braced-path-and-base"}}, {derived});
#elif CASE == 33
std::string path = "/braced-lvalue-path";
auto bad = resource::file<Provider>({path}, provider);
#elif CASE == 34
// Converted path view would refer to string storage owned by the temporary proxy.
// Public methods:
// - operator std::string_view(): Expose proxy path.
struct PathProxy {
	std::string path = "/converted-braced-path";

	operator std::string_view() const noexcept
	{
		return path;
	}
};

auto bad = resource::file<const Provider>({PathProxy{}}, derived);
#elif CASE == 35
auto bad = resource::file("/raw-object", provider).object;
#elif CASE == 36
auto bad = resource::file("/raw-ops", provider).ops;
#elif CASE == 37
auto bad = resource::file("/raw-path", provider).path;
#elif CASE == 38
resource::FileEntry bad{"/raw-construction", &provider, &resource::detail::operations<Provider>};
#elif CASE == 39
// A save() of another shape would leave the file silently not Savable.
struct WrongSave : Provider {
	resource::Status save() noexcept
	{
		return resource::Status::Ok;
	}
} wrongSave;

auto bad = resource::file("/wrong-save", wrongSave);
#else
constexpr auto good = resource::filesystem(resource::file("/good", provider));
static_assert(good.fileCount() == 1);
constexpr auto goodBase = resource::filesystem(resource::file<const Provider>("/base", derived));
static_assert(goodBase.path(0) == "/base");
constexpr auto goodBracedBase =
    resource::filesystem(resource::file<const Provider>("/braced-base", {derived}));
static_assert(goodBracedBase.path(0) == "/braced-base");

int main()
{
	unsigned checks = 0;
	auto check = [&checks](std::string_view path, const Provider& object,
	                       resource::FileEntry entry) {
		const auto files = resource::filesystem(entry);
		const auto file = files[0];
		const auto stat = file.stat();
		object.lastRead = nullptr;
		std::array<std::byte, 1> output{};
		const auto read = file.read(7, output);
		++checks;
		if (file.path() != path || file.path().data() != path.data() || !file.readable() ||
		    file.writable() || stat.status != resource::Status::Ok || stat.size != object.size() ||
		    stat.flags != resource::FileFlag::Readable ||
		    object.lastRead != std::addressof(object) || read.status != resource::Status::Ok ||
		    read.next != 8 || read.written != 1 || !read.eof ||
		    output[0] != static_cast<std::byte>(object.value)) {
			std::abort();
		}
	};
	provider.value = 11;
	derived.value = 23;
	const Provider constant{37};
	const Derived constantDerived{{41}};
	constexpr char literal[] = "/literal";
	const char* pointer = literal;
	char array[] = "/array";
	std::string path = "/lvalue-string-path-with-storage-beyond-the-small-string-buffer";
	const std::string constantPath = "/const-lvalue-string-path-with-stable-storage";
	const std::string_view view = path;

	check(literal, provider, resource::file(literal, provider));
	check(literal, provider, resource::file<Provider>(literal, provider));
	check(literal, provider, resource::file<const Provider>(literal, provider));
	check(pointer, provider, resource::file(pointer, provider));
	check(pointer, provider, resource::file<Provider>(pointer, provider));
	check(pointer, provider, resource::file<Provider>(static_cast<const char*>(literal), provider));
	check(array, provider, resource::file<Provider>(array, provider));
	check(path, provider, resource::file(path, provider));
	check(path, provider, resource::file<Provider>(path, provider));
	check(constantPath, provider, resource::file<Provider>(constantPath, provider));
	check(view, provider, resource::file<Provider>(view, provider));
	check(view, provider, resource::file<Provider>(std::string_view{path}, provider));
	check(view, provider, resource::file<Provider>(std::move(view), provider));
	check(path, derived, resource::file<Provider>(path, derived));
	check(path, derived, resource::file<const Provider>(path, derived));
	check(path, constant, resource::file(path, constant));
	check(path, constant, resource::file<const Provider>(path, constant));
	check(path, constantDerived, resource::file<const Provider>(path, constantDerived));
	check(path, provider, resource::file<Provider>(path, {provider}));
	check(path, provider, resource::file<const Provider>(path, {provider}));
	check(path, constant, resource::file<const Provider>(path, {constant}));
	check(path, derived, resource::file<Provider>(path, {derived}));
	check(path, derived, resource::file<const Provider>(path, {derived}));
	check(path, constantDerived, resource::file<const Provider>(path, {constantDerived}));
	check(literal, provider, resource::file({literal}, provider));
	check(literal, provider, resource::file<Provider>({literal}, provider));
	check(literal, derived, resource::file<const Provider>({literal}, {derived}));
	check(pointer, provider, resource::file<Provider>({pointer}, provider));
	check(pointer, derived, resource::file<const Provider>({pointer}, {derived}));
	check(array, provider, resource::file<Provider>({array}, {provider}));
	check(array, derived, resource::file<const Provider>({array}, {derived}));
	check(view, provider, resource::file<Provider>({view}, provider));
	check(view, provider, resource::file<Provider>({std::string_view{path}}, provider));
	check(view, derived, resource::file<const Provider>({view}, {derived}));
	std::printf("Resource file bindings: %u controls passed\n", checks);
}
#endif
