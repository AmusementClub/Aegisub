#include <main.h>

#include "../../src/shift_times_input.h"

#include <limits>
#include <string>

TEST(shift_times_input, parses_values_with_wx_integer_semantics) {
	EXPECT_EQ(0, dialog_shift_times_detail::ParseFrameInput(wxS("0")));
	EXPECT_EQ(42, dialog_shift_times_detail::ParseFrameInput(wxS("42")));
	EXPECT_EQ(-17, dialog_shift_times_detail::ParseFrameInput(wxS("-17")));
}

TEST(shift_times_input, rejects_empty_partial_and_out_of_range_values) {
	EXPECT_FALSE(dialog_shift_times_detail::ParseFrameInput(wxString{}));
	EXPECT_FALSE(dialog_shift_times_detail::ParseFrameInput(wxS("12 frames")));

	for (auto value : {
		static_cast<long long>(std::numeric_limits<int>::min()) - 1,
		static_cast<long long>(std::numeric_limits<int>::max()) + 1
	}) {
		auto text = std::to_string(value);
		EXPECT_FALSE(dialog_shift_times_detail::ParseFrameInput(wxString::FromUTF8(text.c_str())));
	}
}

TEST(shift_times_input, rejects_reversing_minimum_int) {
	EXPECT_FALSE(dialog_shift_times_detail::ApplyDirection(std::numeric_limits<int>::min(), true));
	EXPECT_EQ(std::numeric_limits<int>::min(),
		dialog_shift_times_detail::ApplyDirection(std::numeric_limits<int>::min(), false));
}
