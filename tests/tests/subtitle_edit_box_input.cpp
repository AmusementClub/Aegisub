#include <main.h>

#include "../../src/subtitle_edit_box_input.h"

namespace {

TEST(SubtitleEditBoxInput, blank_margin_becomes_zero_without_reading_spin_value) {
	bool read = false;
	auto const input = aegisub::subtitle_edit_box_input::ResolveMarginInput(true, [&] {
		read = true;
		return -10000;
	});

	EXPECT_EQ(0, input.value);
	EXPECT_TRUE(input.normalize_control);
	EXPECT_FALSE(read);
}

TEST(SubtitleEditBoxInput, explicit_margin_preserves_spin_value) {
	auto const input = aegisub::subtitle_edit_box_input::ResolveMarginInput(
		false, [] { return -10000; });

	EXPECT_EQ(-10000, input.value);
	EXPECT_FALSE(input.normalize_control);
}

}
