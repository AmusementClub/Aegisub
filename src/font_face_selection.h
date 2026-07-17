#pragma once

#include <string_view>

/// Whether accepting the selector should write an explicit \fn. Existing
/// explicit aliases are normalized to the exact displayed name; an inherited
/// style font is not converted into an override when the family is unchanged.
bool ShouldWriteFontFace(
	std::string_view stored_name,
	std::string_view displayed_name,
	bool has_explicit_override,
	std::string_view selected_name);
