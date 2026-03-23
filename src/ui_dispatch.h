#pragma once

#include <libaegisub/dispatch.h>
#include <libaegisub/signal.h>

#include <wx/debug.h>

#include <functional>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace agi::ui {

using Lifetime = std::shared_ptr<void>;
using WeakLifetime = std::weak_ptr<void>;

namespace detail {
template<typename Result>
using InvokeStorage = std::conditional_t<
	std::is_reference_v<Result>,
	std::reference_wrapper<std::remove_reference_t<Result>>,
	std::remove_cv_t<Result>>;
}

inline Lifetime MakeLifetime() {
	return std::make_shared<char>();
}

class UiActivationScope {
	Lifetime lifetime = MakeLifetime();
	std::vector<agi::signal::Connection> scoped_connections;
	std::vector<std::function<void()>> dispose_actions;

public:
	UiActivationScope() = default;
	UiActivationScope(UiActivationScope const&) = delete;
	UiActivationScope& operator=(UiActivationScope const&) = delete;
	UiActivationScope(UiActivationScope&&) = delete;
	UiActivationScope& operator=(UiActivationScope&&) = delete;

	~UiActivationScope() {
		Deactivate();
	}

	WeakLifetime GetLifetime() const {
		return lifetime;
	}

	bool IsActive() const {
		return static_cast<bool>(lifetime);
	}

	void Activate() {
		if (!lifetime)
			lifetime = MakeLifetime();
	}

	void Deactivate() {
		lifetime.reset();
		scoped_connections.clear();

		auto actions = std::move(dispose_actions);
		dispose_actions.clear();
		for (auto it = actions.rbegin(); it != actions.rend(); ++it)
			(*it)();
	}

	void AddConnection(agi::signal::Connection connection) {
		scoped_connections.push_back(std::move(connection));
	}

	void AddConnection(agi::signal::UnscopedConnection connection) {
		scoped_connections.emplace_back(std::move(connection));
	}

	template<typename... Connections>
	void AddConnections(Connections&&... connections) {
		(AddConnection(std::forward<Connections>(connections)), ...);
	}

	template<typename Callback>
	void OnDeactivate(Callback&& callback) {
		dispose_actions.emplace_back(std::forward<Callback>(callback));
	}
};

inline bool CheckAccess() {
	return agi::dispatch::IsMainThread();
}

inline void VerifyAccess() {
	wxASSERT_MSG(CheckAccess(), "UI access must happen on the main thread");
}

template<typename Callback>
auto MainInvoke(Callback&& callback) -> std::invoke_result_t<std::decay_t<Callback>&> {
	using CallbackType = std::decay_t<Callback>;
	using Result = std::invoke_result_t<CallbackType&>;

	if (CheckAccess()) {
		if constexpr (std::is_void_v<Result>) {
			std::forward<Callback>(callback)();
			return;
		}
		else {
			return std::forward<Callback>(callback)();
		}
	}

	auto callback_ptr = std::make_shared<CallbackType>(std::forward<Callback>(callback));
	if constexpr (std::is_void_v<Result>) {
		agi::dispatch::Main().Sync([callback_ptr = std::move(callback_ptr)]() mutable {
			(*callback_ptr)();
		});
		return;
	}
	else {
		using Storage = detail::InvokeStorage<Result>;
		std::optional<Storage> result;
		agi::dispatch::Main().Sync([&result, callback_ptr = std::move(callback_ptr)]() mutable {
			result.emplace((*callback_ptr)());
		});

		if constexpr (std::is_reference_v<Result>)
			return result->get();
		else
			return std::move(*result);
	}
}

template<typename Callback>
void MainAsyncIfAlive(WeakLifetime lifetime, Callback&& callback) {
	auto callback_ptr = std::make_shared<std::decay_t<Callback>>(std::forward<Callback>(callback));
	agi::dispatch::Main().Async([lifetime, callback_ptr = std::move(callback_ptr)]() mutable {
		if (lifetime.lock())
			(*callback_ptr)();
	});
}

template<typename Callback>
void MainInvokeIfAlive(WeakLifetime lifetime, Callback&& callback) {
	auto callback_ptr = std::make_shared<std::decay_t<Callback>>(std::forward<Callback>(callback));
	MainInvoke([lifetime, callback_ptr = std::move(callback_ptr)]() mutable {
		if (lifetime.lock())
			(*callback_ptr)();
	});
}

template<typename Callback>
void MainSyncIfAlive(WeakLifetime lifetime, Callback&& callback) {
	MainInvokeIfAlive(std::move(lifetime), std::forward<Callback>(callback));
}

}
