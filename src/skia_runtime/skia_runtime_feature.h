#pragma once

#include <cctype>

namespace aegisub::skia {

// A non-empty environment value is an automation/diagnostic override. Normal
// interactive use comes from the persistent option passed by the caller.
inline bool ResolveRuntimeFeatureEnabled(
	char const *environment_value,
	bool configured_value) noexcept {
	if (!environment_value || !*environment_value)
		return configured_value;

	auto const first = static_cast<char>(
		std::tolower(static_cast<unsigned char>(*environment_value)));
	return first != '0' && first != 'f' && first != 'n';
}

}
