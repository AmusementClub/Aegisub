#include <main.h>

#include "../../src/subtitle_edit_ops.h"

#include <libaegisub/ass/dialogue_parser.h>

#include <memory>
#include <utility>
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

struct marked_text {
	std::string text;
	int caret = 0;
};

marked_text UnmarkCaret(std::string marked) {
	auto const pos = marked.find('|');
	EXPECT_NE(std::string::npos, pos);
	marked.erase(pos, 1);
	return {std::move(marked), static_cast<int>(pos)};
}

std::string ApplyAutoClose(std::string marked, aegisub::subtitle_edit_ops::AutoCloseKey key) {
	auto input = UnmarkCaret(std::move(marked));
	auto edit = aegisub::subtitle_edit_ops::BuildAutoCloseEdit(input.text, input.caret, input.caret, key);
	EXPECT_TRUE(edit.handled);

	auto text = aegisub::subtitle_edit_ops::ReplaceRangeWithText(input.text, edit.replace_start, edit.replace_end, edit.replacement);
	text.insert(static_cast<size_t>(edit.caret), "|");
	return text;
}

std::vector<int> WalkHomeBlocks(std::string const& text, int start_pos, bool karaoke_templater = false) {
	auto tokens = agi::ass::TokenizeDialogueBody(text, karaoke_templater);
	agi::ass::SplitWords(text, tokens);

	std::vector<int> positions;
	int pos = start_pos;
	while (true) {
		int next = aegisub::subtitle_edit_ops::GetPreviousBlockStart(tokens, pos);
		if (next == pos && !positions.empty())
			break;
		positions.push_back(next);
		if (next == pos)
			break;
		pos = next;
	}
	return positions;
}

std::vector<int> WalkEndBlocks(std::string const& text, int start_pos, bool karaoke_templater = false) {
	auto tokens = agi::ass::TokenizeDialogueBody(text, karaoke_templater);
	agi::ass::SplitWords(text, tokens);

	std::vector<int> positions;
	int pos = start_pos;
	while (true) {
		int next = aegisub::subtitle_edit_ops::GetNextBlockEnd(tokens, pos);
		if (next == pos && !positions.empty())
			break;
		positions.push_back(next);
		if (next == pos)
			break;
		pos = next;
	}
	return positions;
}

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

TEST(subtitle_edit_ops, split_text_at_position_trims_unicode_boundaries_on_both_halves) {
	std::string const boundaries = "\xC2\xA0\xE3\x80\x80\xEF\xBB\xBF ";
	std::string text = "alpha" + boundaries;
	auto const split_position = static_cast<int>(text.size());
	text += boundaries + "beta";

	auto split = aegisub::subtitle_edit_ops::SplitTextAtPosition(text, split_position);

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

TEST(subtitle_edit_ops, autoclose_inserts_and_skips_override_braces) {
	using aegisub::subtitle_edit_ops::AutoCloseKey;

	EXPECT_EQ("a{|}b", ApplyAutoClose("a|b", AutoCloseKey::OpenBrace));
	EXPECT_EQ("a{}|b", ApplyAutoClose("a{|}b", AutoCloseKey::CloseBrace));
	EXPECT_EQ("|", ApplyAutoClose("{|}", AutoCloseKey::Backspace));
}

TEST(subtitle_edit_ops, autoclose_parentheses_only_inside_override_blocks) {
	using aegisub::subtitle_edit_ops::AutoCloseKey;

	EXPECT_EQ("{\\pos(|)}", ApplyAutoClose("{\\pos|}", AutoCloseKey::OpenParen));
	EXPECT_EQ("{\\pos()|}", ApplyAutoClose("{\\pos(|)}", AutoCloseKey::CloseParen));
	EXPECT_EQ("{\\pos|}", ApplyAutoClose("{\\pos(|)}", AutoCloseKey::Backspace));

	EXPECT_FALSE(aegisub::subtitle_edit_ops::BuildAutoCloseEdit("plain text", 5, 5, AutoCloseKey::OpenParen).handled);
	EXPECT_FALSE(aegisub::subtitle_edit_ops::BuildAutoCloseEdit("plain ) text", 6, 6, AutoCloseKey::CloseParen).handled);
}

TEST(subtitle_edit_ops, autoclose_wraps_selected_text_with_braces) {
	using aegisub::subtitle_edit_ops::AutoCloseKey;

	auto edit = aegisub::subtitle_edit_ops::BuildAutoCloseEdit("abc", 0, 1, AutoCloseKey::OpenBrace);
	EXPECT_TRUE(edit.handled);
	EXPECT_EQ(0, edit.replace_start);
	EXPECT_EQ(1, edit.replace_end);
	EXPECT_EQ("{a}", edit.replacement);
	EXPECT_EQ(3, edit.caret);

	edit = aegisub::subtitle_edit_ops::BuildAutoCloseEdit("abc", 1, 3, AutoCloseKey::OpenBrace);
	EXPECT_TRUE(edit.handled);
	EXPECT_EQ(1, edit.replace_start);
	EXPECT_EQ(3, edit.replace_end);
	EXPECT_EQ("{bc}", edit.replacement);
	EXPECT_EQ(5, edit.caret);
}

TEST(subtitle_edit_ops, autoclose_ignores_selected_text_for_parentheses) {
	using aegisub::subtitle_edit_ops::AutoCloseKey;

	EXPECT_FALSE(aegisub::subtitle_edit_ops::BuildAutoCloseEdit("{\\pos}", 1, 5, AutoCloseKey::OpenParen).handled);
}

TEST(subtitle_edit_ops, home_blocks_step_over_ass_blocks) {
	std::string const text = "hello{\\i1}world";

	EXPECT_EQ((std::vector<int>{10, 5, 0}), WalkHomeBlocks(text, text.size()));
	EXPECT_EQ(5, aegisub::subtitle_edit_ops::GetPreviousBlockStart(
		agi::ass::TokenizeDialogueBody(text),
		10));
}

TEST(subtitle_edit_ops, home_blocks_treat_plain_text_as_one_block) {
	EXPECT_EQ((std::vector<int>{0}), WalkHomeBlocks("hello there", 11));
}

TEST(subtitle_edit_ops, home_blocks_stop_at_zero) {
	EXPECT_EQ((std::vector<int>{0}), WalkHomeBlocks("hello", 0));
}

TEST(subtitle_edit_ops, home_blocks_include_line_breaks) {
	EXPECT_EQ((std::vector<int>{7, 5, 0}), WalkHomeBlocks("hello\\Nthere", 12));
}

TEST(subtitle_edit_ops, end_blocks_step_over_ass_blocks) {
	std::string const text = "hello{\\i1}world";

	EXPECT_EQ((std::vector<int>{5, 10, 15}), WalkEndBlocks(text, 0));
	EXPECT_EQ(10, aegisub::subtitle_edit_ops::GetNextBlockEnd(
		agi::ass::TokenizeDialogueBody(text),
		5));
}

TEST(subtitle_edit_ops, end_blocks_treat_plain_text_as_one_block) {
	EXPECT_EQ((std::vector<int>{11}), WalkEndBlocks("hello there", 0));
}

TEST(subtitle_edit_ops, end_blocks_stay_at_end) {
	EXPECT_EQ((std::vector<int>{5}), WalkEndBlocks("hello", 5));
}

TEST(subtitle_edit_ops, end_blocks_include_line_breaks) {
	EXPECT_EQ((std::vector<int>{5, 7, 12}), WalkEndBlocks("hello\\Nthere", 0));
}
