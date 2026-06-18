#pragma once

#include <cstddef>
#include <vector>

namespace aegisub::paste_over_policy {

inline constexpr std::size_t FieldCount = 11;

enum FieldIndex : std::size_t {
	Comment = 0,
	Layer = 1,
	StartTime = 2,
	EndTime = 3,
	Style = 4,
	Actor = 5,
	MarginLeft = 6,
	MarginRight = 7,
	MarginVertical = 8,
	Effect = 9,
	Text = 10,
};

std::vector<bool> NormalizeFields(std::vector<bool> fields);
std::vector<bool> BuildAllFields(bool checked);
std::vector<bool> BuildTimesFields();
std::vector<bool> BuildTextFields();

} // namespace aegisub::paste_over_policy
