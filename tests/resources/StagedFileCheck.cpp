// The staged writable resource: pieces staged, committed whole on the final one.
// Author: Ruslan Kovtun (shpegun60). SPDX-License-Identifier: MIT.
// Checks that a write changes the file only when it completes the whole file,
// that every incomplete, oversized, misplaced or refused write keeps the old
// bytes, and that the commit hook sees the new bytes first and may refuse them.

#include <resource/Resource.hpp>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>

namespace {
static_assert(resource::ReadableProvider<resource::StagedFile>);
static_assert(resource::WritableProvider<resource::StagedFile>);

unsigned checks = 0;
#define CHECK(...)                                                                                 \
	do {                                                                                           \
		++checks;                                                                                  \
		if (!(__VA_ARGS__))                                                                        \
			std::abort();                                                                          \
	} while (false)

void checkWrite(resource::WriteResult result, resource::Status status, resource::Cursor next,
                std::uint32_t consumed, bool complete)
{
	CHECK(result.status == status && result.next == next && result.consumed == consumed &&
	      result.complete == complete);
}

template<std::size_t N>
std::array<std::byte, N> filled(unsigned seed)
{
	std::array<std::byte, N> out{};
	for (std::size_t i = 0; i < N; ++i)
		out[i] = std::byte((seed + i * 3) & 0xffu);
	return out;
}

unsigned seen = 0;
resource::Status refuseFirstFf(void*, resource::Input bytes) noexcept
{
	++seen;
	return bytes.empty() || bytes[0] != std::byte{0xff} ? resource::Status::Ok : resource::Status::InvalidData;
}
} // namespace

int main()
{
	auto bytes = filled<6>(1);
	const auto old = bytes;
	std::array<std::byte, 6> stage{};
	resource::StagedFile file{bytes, stage};
	CHECK(file.size() == 6 && file.staged() == 0);

	// Through a flat table: readable and writable.
	auto files = resource::filesystem(resource::file("/cfg/staged.bin", file));
	const auto stat = files.stat(0);
	CHECK(stat.status == resource::Status::Ok && stat.size == 6 &&
	      stat.flags == (resource::FileFlag::Readable | resource::FileFlag::Writable));

	// No transfer, or another cursor than the one staged: InvalidCursor.
	const auto next = filled<6>(40);
	checkWrite(file.write(3, resource::Input(next).first(2), false), resource::Status::InvalidCursor, 3, 0, false);
	checkWrite(file.write(0, resource::Input(next).first(2), false), resource::Status::Ok, 2, 2, false);
	checkWrite(file.write(3, resource::Input(next).subspan(2, 2), false), resource::Status::InvalidCursor, 3, 0,
	           false);
	// While staged, reads return the old bytes.
	std::array<std::byte, 6> out{};
	CHECK(file.read(0, out).written == 6 && out == old && file.staged() == 2);
	// The rest, final: the new bytes replace the old.
	checkWrite(file.write(2, resource::Input(next).subspan(2), true), resource::Status::Ok, 6, 4, true);
	CHECK(bytes == next && file.staged() == 0);
	CHECK(file.read(0, out).written == 6 && out == next);

	// A transfer that stops part way leaves the file whole; the next from 0 drops it.
	const auto third = filled<6>(90);
	checkWrite(file.write(0, resource::Input(third).first(3), false), resource::Status::Ok, 3, 3, false);
	CHECK(bytes == next && file.staged() == 3);
	checkWrite(file.write(0, resource::Input(third).first(4), false), resource::Status::Ok, 4, 4, false);
	checkWrite(file.write(4, resource::Input(third).subspan(4), true), resource::Status::Ok, 6, 2, true);
	CHECK(bytes == third);

	// Too long: refused, nothing staged from it, the old bytes kept.
	const auto seven = filled<7>(5);
	checkWrite(file.write(0, seven, true), resource::Status::InvalidData, 0, 0, false);
	CHECK(bytes == third && file.staged() == 0);
	checkWrite(file.write(0, resource::Input(seven).first(4), false), resource::Status::Ok, 4, 4, false);
	checkWrite(file.write(4, resource::Input(seven).subspan(4), false), resource::Status::InvalidData, 4, 0, false);
	CHECK(bytes == third && file.staged() == 0);
	// Too short: refused at the final piece, the old bytes kept.
	checkWrite(file.write(0, resource::Input(next).first(5), true), resource::Status::InvalidData, 0, 0, false);
	CHECK(bytes == third && file.staged() == 0);
	// An empty piece that does not end the stream is no progress: refused.
	checkWrite(file.write(0, resource::Input(next).first(2), false), resource::Status::Ok, 2, 2, false);
	checkWrite(file.write(2, {}, false), resource::Status::InvalidData, 2, 0, false);
	// An empty final piece after every byte completes it.
	checkWrite(file.write(2, resource::Input(next).subspan(2), false), resource::Status::Ok, 6, 4, false);
	checkWrite(file.write(6, {}, true), resource::Status::Ok, 6, 0, true);
	CHECK(bytes == next);

	// The commit hook sees the new bytes first; a refusal keeps the old ones.
	auto guardedBytes = filled<4>(2);
	const auto guardedOld = guardedBytes;
	std::array<std::byte, 4> guardedStage{};
	resource::StagedFile guarded{guardedBytes, guardedStage, refuseFirstFf};
	const std::array<std::byte, 4> bad{std::byte{0xff}, std::byte{1}, std::byte{2}, std::byte{3}};
	checkWrite(guarded.write(0, bad, true), resource::Status::InvalidData, 0, 0, false);
	CHECK(guardedBytes == guardedOld && seen == 1 && guarded.staged() == 0);
	const std::array<std::byte, 4> good{std::byte{7}, std::byte{1}, std::byte{2}, std::byte{3}};
	checkWrite(guarded.write(0, good, true), resource::Status::Ok, 4, 4, true);
	CHECK(guardedBytes == good && seen == 2);

	// A stage smaller than the file is a setup error: InternalError, nothing kept.
	auto small = filled<4>(3);
	const auto smallOld = small;
	std::array<std::byte, 2> tooSmall{};
	resource::StagedFile broken{small, tooSmall};
	checkWrite(broken.write(0, resource::Input(good).first(2), false), resource::Status::InternalError, 0, 0, false);
	CHECK(small == smallOld && broken.staged() == 0);

	// An empty file: one empty final piece completes it.
	std::array<std::byte, 0> none{};
	std::array<std::byte, 0> noneStage{};
	resource::StagedFile empty{none, noneStage};
	checkWrite(empty.write(0, {}, true), resource::Status::Ok, 0, 0, true);
	std::array<std::byte, 1> one{};
	CHECK(empty.read(0, one).eof && empty.read(1, one).status == resource::Status::InvalidCursor);

	// Through the flat table, as the protocol calls it.
	const auto viaTable = filled<6>(120);
	checkWrite(files.write(0, 0, viaTable, true), resource::Status::Ok, 6, 6, true);
	CHECK(bytes == viaTable);

	std::printf("StagedFile: %u checks passed\n", checks);
}
