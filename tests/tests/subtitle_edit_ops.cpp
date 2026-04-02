#include <main.h>

#include "../../src/subtitle_edit_ops.h"

#include <memory>
#include <vector>

namespace {

struct edit_fixture {
	std::vector<std::unique_ptr<AssDialogue>> storage;

	AssDialogue *AddLine(int start, int end, std::string text) {
		storage.push_back(std::make_unique<AssDialogue>());
		auto *line = storage.back().get();
		line->Start = start;
		line->End = end;
		line->Text = std::move(text);
		return line;
	}
};

}

TEST(subtitle_edit_ops, join_selection_into_first_adds_karaoke_tags_and_extends_end) {
	edit_fixture fixture;
	auto *first = fixture.AddLine(100, 1100, "Hello");
	auto *second = fixture.AddLine(1200, 2000, "World");

	ASSERT_TRUE(aegisub::subtitle_edit_ops::JoinSelectionIntoFirst(
		{first, second},
		aegisub::subtitle_edit_ops::JoinMode::Karaoke));

	EXPECT_EQ("{\\k100}Hello{\\k80}World", first->Text.get());
	EXPECT_EQ(2000, static_cast<int>(first->End));
}

TEST(subtitle_edit_ops, join_selection_into_first_can_keep_first_text) {
	edit_fixture fixture;
	auto *first = fixture.AddLine(100, 1100, "Hello");
	auto *second = fixture.AddLine(1200, 2000, "World");

	ASSERT_TRUE(aegisub::subtitle_edit_ops::JoinSelectionIntoFirst(
		{first, second},
		aegisub::subtitle_edit_ops::JoinMode::KeepFirst));

	EXPECT_EQ("Hello", first->Text.get());
	EXPECT_EQ(2000, static_cast<int>(first->End));
}

TEST(subtitle_edit_ops, recombine_selection_trims_edges_and_marks_duplicate_lines_for_removal) {
	edit_fixture fixture;
	auto *first = fixture.AddLine(100, 500, "Hello");
	auto *second = fixture.AddLine(600, 900, " Hello\\N");

	auto result = aegisub::subtitle_edit_ops::RecombineSelection({first, second});

	ASSERT_EQ(1u, result.lines_to_remove.size());
	EXPECT_EQ(first, result.lines_to_remove[0]);
	EXPECT_EQ("Hello", second->Text.get());
	EXPECT_EQ(100, static_cast<int>(second->Start));
	EXPECT_EQ(900, static_cast<int>(second->End));
}

TEST(subtitle_edit_ops, recombine_selection_can_trim_suffix_from_previous_line) {
	edit_fixture fixture;
	auto *first = fixture.AddLine(100, 500, "HelloWorld");
	auto *second = fixture.AddLine(600, 900, "World");

	auto result = aegisub::subtitle_edit_ops::RecombineSelection({first, second});

	EXPECT_TRUE(result.lines_to_remove.empty());
	EXPECT_EQ("Hello", first->Text.get());
	EXPECT_EQ(100, static_cast<int>(second->Start));
	EXPECT_EQ(900, static_cast<int>(second->End));
}

TEST(subtitle_edit_ops, split_text_at_position_trims_both_halves) {
	auto split = aegisub::subtitle_edit_ops::SplitTextAtPosition("alpha beta", 5);

	EXPECT_EQ("alpha", split.first);
	EXPECT_EQ("beta", split.second);
}

TEST(subtitle_edit_ops, estimate_split_time_uses_text_length_ratio) {
	auto split = aegisub::subtitle_edit_ops::EstimateSplitTime(1000, 2000, "abc", "de");

	ASSERT_TRUE(split.has_value());
	EXPECT_EQ(1600, *split);
	EXPECT_FALSE(aegisub::subtitle_edit_ops::EstimateSplitTime(1000, 2000, "", "").has_value());
}

TEST(subtitle_edit_ops, build_tag_only_text_keeps_non_plain_blocks) {
	AssDialogue line;
	line.Text = "{\\i1}Hello{note}";

	EXPECT_EQ("{\\i1}{note}", aegisub::subtitle_edit_ops::BuildTagOnlyText(line));
}

TEST(subtitle_edit_ops, replace_range_with_text_clamps_invalid_ranges) {
	EXPECT_EQ("Z", aegisub::subtitle_edit_ops::ReplaceRangeWithText("abcde", -3, 99, "Z"));
	EXPECT_EQ("aXYde", aegisub::subtitle_edit_ops::ReplaceRangeWithText("abcde", 1, 3, "XY"));
}
