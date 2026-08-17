#pragma once

#include <limits>
#include <optional>

#include <wx/string.h>

namespace dialog_shift_times_detail {

inline std::optional<int> ParseFrameInput(wxString const& input) {
	// The downstream frame/time APIs use int. Parse through wx's wide integer
	// type so 64-bit Unix builds do not accept a value which is later truncated
	// when it crosses that API boundary.
	wxLongLong_t value = 0;
	if (!input.ToLongLong(&value))
		return std::nullopt;
	if (value < static_cast<wxLongLong_t>(std::numeric_limits<int>::min()) ||
		value > static_cast<wxLongLong_t>(std::numeric_limits<int>::max()))
		return std::nullopt;
	return static_cast<int>(value);
}

inline std::optional<int> ApplyDirection(int value, bool reverse) {
	if (reverse && value == std::numeric_limits<int>::min())
		return std::nullopt;
	return reverse ? -value : value;
}

}
