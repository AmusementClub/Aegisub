#pragma once

#include <type_traits>
#include <utility>

namespace agi {
template<typename F>
class scope_exit final {
	F func;
	bool active = true;

public:
	explicit scope_exit(F&& func) noexcept(std::is_nothrow_move_constructible_v<F>)
	: func(std::forward<F>(func)) {
	}

	scope_exit(scope_exit const&) = delete;
	scope_exit& operator=(scope_exit const&) = delete;

	scope_exit(scope_exit&& other) noexcept(std::is_nothrow_move_constructible_v<F>)
	: func(std::move(other.func))
	, active(other.active) {
		other.active = false;
	}

	~scope_exit() {
		if (active)
			func();
	}

	void release() noexcept {
		active = false;
	}
};

template<typename F>
auto make_scope_exit(F&& func) -> scope_exit<std::decay_t<F>> {
	return scope_exit<std::decay_t<F>>(std::forward<F>(func));
}
}
