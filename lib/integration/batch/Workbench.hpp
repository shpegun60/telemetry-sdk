/* Optional Mixed Batch over the Workbench's existing FIFO. MIT. */
#ifndef TELEMETRY_INTEGRATION_BATCH_WORKBENCH_HPP
#define TELEMETRY_INTEGRATION_BATCH_WORKBENCH_HPP
#pragma once
#include <integration/batch/Client.hpp>
#include <integration/workbench/WorkbenchModel.hpp>
#include <optional>

namespace telemetry::integration::batch {
namespace detail {
struct WorkbenchAccess {
private:
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
	struct Completion {
		W* owner;
		bool mutation;
		std::optional<F> callable;

		template<class G>
		Completion(W& value, bool changes, G&& callback) noexcept : owner(&value), mutation(changes)
		{
			static_assert(std::is_nothrow_constructible_v<F, G&&> &&
			              std::is_nothrow_move_constructible_v<F> &&
			              std::is_nothrow_destructible_v<F>);
			typename W::Publication publication{*owner};
			callable.emplace(std::forward<G>(callback));
		}

		Completion(Completion&& other) noexcept : owner(other.owner), mutation(other.mutation)
		{
			typename W::Publication publication{*owner};
			callable.emplace(std::move(*other.callable));
		}

		Completion(const Completion&) = delete;
		Completion& operator=(const Completion&) = delete;
		Completion& operator=(Completion&&) = delete;

		~Completion()
		{
			typename W::Publication publication{*owner};
			callable.reset();
		}

		void operator()(const BatchResult& result) noexcept
		{
			if (mutation && owner->phase_ == workbench::Phase::Executing)
				owner->phase_ = workbench::Phase::Idle;
			dynamic::detail::CallbackScope completion{owner->completionActive_};
			(*callable)(result);
		}
	};

public:
	template<class W, class B>
	[[nodiscard]] static BatchBindingResult bind(W& owner, B& backend, wire::Capability capability,
	                                             std::span<const Endpoint> endpoints) noexcept
	{
		const auto status = gate(owner);
		if (status != client::Status::Ok)
			return {status, {}};
		typename W::Publication publication{owner};
		return batch::bindBatch(owner.readOnlyClient(), backend, capability, endpoints);
	}

	template<class W, class F>
	[[nodiscard]] static client::Submission
	submit(W& owner, const BatchBinding& binding, std::span<const client::Input> payloads,
	       client::Output scratch, F&& callback, std::uint32_t timeout) noexcept
	{
		static_assert(std::is_nothrow_invocable_v<std::decay_t<F>&, const BatchResult&>);
		const auto status = gate(owner);
		if (status != client::Status::Ok)
			return {status, {}};
		if (!BatchAccess::current(owner.readOnlyClient(), binding))
			return {client::Status::NotAdmitted, {}};
		bool mutation = false;
		for (const auto& entry : binding.entries())
			mutation = mutation || entry.operation != client::Operation::FieldRead;
		typename W::Publication publication{owner};
		Completion<W, std::decay_t<F>> completion{owner, mutation, std::forward<F>(callback)};
		const auto submitted = batch::batch(owner.readOnlyClient(), binding, payloads, scratch,
		                                    std::move(completion), timeout);
		if (submitted && mutation) {
			// Invalidate before releasing publication: a callable destructor cannot
			// observe accepted mutation alongside an editable old snapshot.
			owner.invalidateData();
			owner.phase_ = workbench::Phase::Executing;
		}
		return submitted;
	}
};
} // namespace detail

// The owner and its supplied buffers outlive every accepted completion/drain.
// Scratch/payloads are stable and disjoint until submission returns; FIFO owns
// its request copy afterwards. Result views borrow only during the callback.
// Explicit CallbackBytes >=1536 accommodates a small host callable and binding;
// portable Workbench defaults and object layout are unchanged.
template<std::size_t N, std::size_t P, std::size_t S, std::size_t C, class B>
    requires std::is_convertible_v<B*, client::Backend*>
[[nodiscard]] BatchBindingResult bindBatch(workbench::WorkbenchModel<N, P, S, C>& owner, B& backend,
                                           wire::Capability capability,
                                           std::span<const Endpoint> endpoints) noexcept
{
	return detail::WorkbenchAccess::bind(owner, backend, capability, endpoints);
}

template<std::size_t N, std::size_t P, std::size_t S, std::size_t C, class F>
[[nodiscard]] client::Submission
batch(workbench::WorkbenchModel<N, P, S, C>& owner, const BatchBinding& binding,
      std::span<const client::Input> payloads, client::Output requestScratch, F&& callback,
      std::uint32_t timeoutMs = 30000) noexcept
{
	return detail::WorkbenchAccess::submit(owner, binding, payloads, requestScratch,
	                                       std::forward<F>(callback), timeoutMs);
}
} // namespace telemetry::integration::batch
#endif
