/*
 * @file Workbench.hpp
 * @brief The device's files over the Workbench's existing FIFO.
 * @author Ruslan Kovtun (shpegun60). SPDX-License-Identifier: MIT.
 *
 * Lets files::list, stat, read, write, save, FileReader and FileWriter take a
 * WorkbenchModel. File requests then share the Workbench's one queue and its
 * gate: they are refused as Busy while the Workbench writes, refreshes or
 * executes, and while one of its publications or completions runs. A file
 * WRITE or SAVE passes the same gate as a read and does not move the Workbench
 * into its Writing phase; Field values a file changes are seen at its next
 * refresh.
 */
#ifndef TELEMETRY_INTEGRATION_FILES_WORKBENCH_HPP
#define TELEMETRY_INTEGRATION_FILES_WORKBENCH_HPP
#pragma once

#include <integration/files/Client.hpp>
#include <integration/workbench/WorkbenchModel.hpp>
#include <optional>

namespace telemetry::integration::files {
namespace detail {
struct WorkbenchAccess {
private:
	// Nested callback ownership can occur while an outer publication or
	// completion is already active. Restore that prior state, never clear it.
	struct FlagScope {
		bool& flag;
		bool previous;

		explicit FlagScope(bool& value) noexcept : flag(value), previous(value)
		{
			flag = true;
		}

		~FlagScope()
		{
			flag = previous;
		}
	};

	template<class W, class F>
	struct Completion {
		W* owner;
		std::optional<F> callable;

		template<class G>
		Completion(W& value, G&& callback) noexcept : owner(&value)
		{
			static_assert(std::is_nothrow_constructible_v<F, G&&> &&
			                  std::is_nothrow_move_constructible_v<F> &&
			                  std::is_nothrow_destructible_v<F>,
			              "Workbench file completion ownership must not throw");
			const FlagScope publication{owner->publishing_};
			callable.emplace(std::forward<G>(callback));
		}

		Completion(Completion&& other) noexcept : owner(other.owner)
		{
			const FlagScope publication{owner->publishing_};
			callable.emplace(std::move(*other.callable));
		}

		Completion(const Completion&) = delete;
		Completion& operator=(const Completion&) = delete;
		Completion& operator=(Completion&&) = delete;

		~Completion()
		{
			const FlagScope publication{owner->publishing_};
			callable.reset();
		}

		void operator()(const client::ResultView& result) noexcept
		{
			const FlagScope completion{owner->completionActive_};
			(*callable)(result);
		}
	};

public:
	template<class W>
	[[nodiscard]] static client::Status gate(const W& owner) noexcept
	{
		const auto status = owner.readOnlyGate();
		if (status != client::Status::Ok)
			return status;
		if (owner.publishing_ || owner.completionActive_ ||
		    owner.phase_ == workbench::Phase::Writing ||
		    owner.phase_ == workbench::Phase::Refreshing ||
		    owner.phase_ == workbench::Phase::Executing)
			return client::Status::Busy;
		return client::Status::Ok;
	}

	template<class W, class F>
	[[nodiscard]] static client::Submission submit(W& owner, client::Input request,
	                                               std::uint32_t replyBytes, F&& callback,
	                                               std::uint32_t timeoutMs) noexcept
	{
		static_assert(std::is_nothrow_invocable_v<std::decay_t<F>&, const client::ResultView&>,
		              "Workbench file completion must be noexcept");
		const auto status = gate(owner);
		if (status != client::Status::Ok)
			return {status, {}};
		if (!timeoutMs || timeoutMs > client::maxTimeoutMs)
			return {client::Status::InvalidArgument, {}};
		if constexpr (std::is_pointer_v<std::remove_reference_t<F>>)
			if (!callback)
				return {client::Status::InvalidArgument, {}};
		const FlagScope publication{owner.publishing_};
		Completion<W, std::decay_t<F>> completion{owner, std::forward<F>(callback)};
		return owner.readOnlyClient().submitResource(request, replyBytes, std::move(completion),
		                                             timeoutMs);
	}

	template<class W>
	[[nodiscard]] static bool cancel(W& owner, client::RequestHandle handle) noexcept
	{
		return owner.readOnlyClient().cancel(handle);
	}
};
} // namespace detail

template<std::size_t N, std::size_t P, std::size_t S, std::size_t C>
struct Submitter<workbench::WorkbenchModel<N, P, S, C>> {
	template<class F>
	[[nodiscard]] static client::Submission submit(workbench::WorkbenchModel<N, P, S, C>& owner,
	                                               client::Input request, std::uint32_t replyBytes,
	                                               F&& callback, std::uint32_t timeoutMs) noexcept
	{
		return detail::WorkbenchAccess::submit(owner, request, replyBytes,
		                                       std::forward<F>(callback), timeoutMs);
	}

	static bool cancel(workbench::WorkbenchModel<N, P, S, C>& owner,
	                   client::RequestHandle handle) noexcept
	{
		return detail::WorkbenchAccess::cancel(owner, handle);
	}
};
} // namespace telemetry::integration::files
#endif
