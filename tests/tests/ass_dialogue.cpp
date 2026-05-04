#include <gtest/gtest.h>

#include "../../src/ass_dialogue.h"
#include "../../src/ass_karaoke.h"
#include "../../src/ass_parse_error.h"
#include "../../src/ass_time_projection.h"

#include <libaegisub/color.h>
#include <libaegisub/vfr.h>

TEST(ass_time_projection, legacy_output_uses_symmetric_rounding_for_ass_storage) {
	AssDialogue line;
	line.Comment = false;
	line.Layer = 2;
	line.Start = 14189;
	line.End = 1059268;
	line.Style = "Default";
	line.Actor = "Actor";
	line.Margin = { { 1, 2, 3 } };
	line.Effect = "Effect";
	line.Text = "Hello";

	EXPECT_EQ(
		"Dialogue: 2,0:00:14.19,0:17:39.27,Default,Actor,1,2,3,Effect,Hello",
		SerializeAssDialogueForOutput(line, AssTimeOutputMode::LegacyRounding));
}

TEST(ass_time_projection, legacy_output_can_be_requested_when_projection_would_expand_interval) {
	AssDialogue line;
	line.Start = 18;
	line.End = 19;
	line.Style = "Default";
	line.Text = "short";

	EXPECT_EQ(
		"Dialogue: 0,0:00:00.02,0:00:00.02,Default,,0,0,0,,short",
		SerializeAssDialogueForOutput(line, AssTimeOutputMode::LegacyRounding));
	EXPECT_EQ(
		"Dialogue: 0,0:00:00.01,0:00:00.02,Default,,0,0,0,,short",
		SerializeAssDialogueForOutput(line, AssTimeOutputMode::FrameSafeProjection));
}

TEST(ass_time_projection, frame_safe_times_preserve_100fps_snap_semantics) {
	auto const fps = agi::vfr::Framerate(100.0);

	EXPECT_EQ(10, ProjectAssTimeForStorage(5, AssStorageTimeBoundary::Start, &fps));
	EXPECT_EQ(20, ProjectAssTimeForStorage(15, AssStorageTimeBoundary::End, &fps));
}

TEST(ass_time_projection, non_canonical_times_preserve_100fps_frame_semantics_when_possible) {
	auto const fps = agi::vfr::Framerate(100.0);

	EXPECT_EQ(20, ProjectAssTimeForStorage(19, AssStorageTimeBoundary::Start, &fps));
	EXPECT_EQ(20, ProjectAssTimeForStorage(17, AssStorageTimeBoundary::End, &fps));
}

TEST(ass_time_projection, frame_safe_ties_use_symmetric_rounding_candidate) {
	auto const fps = agi::vfr::Framerate(50.0);

	EXPECT_EQ(20, ProjectAssTimeForStorage(15, AssStorageTimeBoundary::Start, &fps));
	EXPECT_EQ(40, ProjectAssTimeForStorage(35, AssStorageTimeBoundary::End, &fps));
}

TEST(ass_time_projection, serializes_frame_safe_dialogue_using_projection_when_fps_is_available) {
	auto const fps = agi::vfr::Framerate(100.0);

	AssDialogue line;
	line.Comment = false;
	line.Layer = 0;
	line.Start = 5;
	line.End = 15;
	line.Style = "Default";
	line.Text = "frame";

	EXPECT_EQ(
		"Dialogue: 0,0:00:00.01,0:00:00.02,Default,,0,0,0,,frame",
		SerializeAssDialogueForOutput(line, AssTimeOutputMode::FrameSafeProjection, &fps));
}

TEST(ass_time_projection, short_unrepresentable_intervals_expand_to_a_non_empty_ass_bucket) {
	AssDialogue line;
	line.Start = 14;
	line.End = 15;
	line.Style = "Default";
	line.Text = "short";

	EXPECT_EQ(
		"Dialogue: 0,0:00:00.01,0:00:00.02,Default,,0,0,0,,short",
		SerializeAssDialogueForOutput(line, AssTimeOutputMode::FrameSafeProjection));
}

TEST(ass_time_projection, short_boundary_crossing_intervals_project_to_a_single_ass_bucket) {
	auto const projected = ProjectAssDialogueTimesForStorage(agi::Time(19), agi::Time(21));

	EXPECT_EQ(20, projected.first);
	EXPECT_EQ(30, projected.second);
}

TEST(ass_time_projection, projected_visibility_uses_storage_interval_not_original_ms_interval) {
	EXPECT_TRUE(IsAssDialogueVisibleAtTimeForOutput(agi::Time(18), agi::Time(19), 19, AssTimeOutputMode::FrameSafeProjection));
	EXPECT_FALSE(IsAssDialogueVisibleAtTimeForOutput(agi::Time(18), agi::Time(19), 20, AssTimeOutputMode::FrameSafeProjection));
}

TEST(ass_time_projection, legacy_visibility_uses_legacy_rounded_output_interval) {
	EXPECT_FALSE(IsAssDialogueVisibleAtTimeForOutput(agi::Time(18), agi::Time(19), 19, AssTimeOutputMode::LegacyRounding));
}

TEST(ass_time_projection, legacy_visibility_matches_saved_ass_for_cursor_snap_boundaries) {
	auto exact_millisecond_visible = [](AssDialogue const& line, int time_ms) {
		return !(line.Start.GetMillisecond() > time_ms || line.End.GetMillisecond() <= time_ms);
	};

	AssDialogue issue_boundary;
	issue_boundary.Start = 1057540;
	issue_boundary.End = 1059268;
	issue_boundary.Style = "Default";
	issue_boundary.Text = "snap";

	EXPECT_EQ(
		"Dialogue: 0,0:17:37.54,0:17:39.27,Default,,0,0,0,,snap",
		SerializeAssDialogueForOutput(issue_boundary, AssTimeOutputMode::LegacyRounding));
	EXPECT_FALSE(exact_millisecond_visible(issue_boundary, 1059268));
	EXPECT_TRUE(IsAssDialogueVisibleAtTimeForOutput(
		issue_boundary.Start,
		issue_boundary.End,
		1059268,
		AssTimeOutputMode::LegacyRounding));
	EXPECT_FALSE(IsAssDialogueVisibleAtTimeForOutput(
		issue_boundary.Start,
		issue_boundary.End,
		1059270,
		AssTimeOutputMode::LegacyRounding));

	AssDialogue rounded_before_cursor;
	rounded_before_cursor.Start = 11140;
	rounded_before_cursor.End = 14181;
	rounded_before_cursor.Style = "Default";
	rounded_before_cursor.Text = "snap";

	EXPECT_EQ(
		"Dialogue: 0,0:00:11.14,0:00:14.18,Default,,0,0,0,,snap",
		SerializeAssDialogueForOutput(rounded_before_cursor, AssTimeOutputMode::LegacyRounding));
	EXPECT_FALSE(IsAssDialogueVisibleAtTimeForOutput(
		rounded_before_cursor.Start,
		rounded_before_cursor.End,
		14181,
		AssTimeOutputMode::LegacyRounding));
}

TEST(ass_time_projection, exact_cursor_snap_end_uses_saved_ass_cutoff) {
	AssDialogue line;
	line.Start = 1057540;
	line.End = ProjectAssTimeForExactCursorSnap(1059268, AssStorageTimeBoundary::End);
	line.Style = "Default";
	line.Text = "snap";

	EXPECT_EQ(1059260, line.End.GetMillisecond());
	EXPECT_EQ(
		"Dialogue: 0,0:17:37.54,0:17:39.26,Default,,0,0,0,,snap",
		SerializeAssDialogueForOutput(line, AssTimeOutputMode::LegacyRounding));
	EXPECT_FALSE(IsAssDialogueVisibleAtTimeForOutput(
		line.Start,
		line.End,
		1059268,
		AssTimeOutputMode::LegacyRounding));
}

TEST(ass_time_projection, exact_cursor_snap_start_uses_saved_ass_start) {
	AssDialogue line;
	line.Start = ProjectAssTimeForExactCursorSnap(1059268, AssStorageTimeBoundary::Start);
	line.End = 1059300;
	line.Style = "Default";
	line.Text = "snap";

	EXPECT_EQ(1059260, line.Start.GetMillisecond());
	EXPECT_TRUE(IsAssDialogueVisibleAtTimeForOutput(
		line.Start,
		line.End,
		1059268,
		AssTimeOutputMode::LegacyRounding));
	EXPECT_FALSE(IsAssDialogueVisibleAtTimeForOutput(
		line.Start,
		line.End,
		1059259,
		AssTimeOutputMode::LegacyRounding));
}

TEST(ass_time_projection, exact_cursor_snap_does_not_use_frame_safe_projection_for_issue_421) {
	auto const fps = agi::vfr::Framerate(24000, 1001);
	int const cursor_ms = 1059268;

	int const cursor_start = ProjectAssTimeForExactCursorSnap(cursor_ms, AssStorageTimeBoundary::Start);
	int const cursor_end = ProjectAssTimeForExactCursorSnap(cursor_ms, AssStorageTimeBoundary::End);
	int const frame_safe_start = ProjectAssTimeForStorage(cursor_ms, AssStorageTimeBoundary::Start, &fps);
	int const frame_safe_end = ProjectAssTimeForStorage(cursor_ms, AssStorageTimeBoundary::End, &fps);

	EXPECT_EQ(1059260, cursor_start);
	EXPECT_EQ(1059260, cursor_end);
	EXPECT_EQ(1059270, frame_safe_start);
	EXPECT_EQ(1059270, frame_safe_end);

	AssDialogue exact_cursor_line;
	exact_cursor_line.Start = 1057540;
	exact_cursor_line.End = cursor_end;
	exact_cursor_line.Style = "Default";
	exact_cursor_line.Text = "snap";
	EXPECT_FALSE(IsAssDialogueVisibleAtTimeForOutput(
		exact_cursor_line.Start,
		exact_cursor_line.End,
		cursor_ms,
		AssTimeOutputMode::LegacyRounding));

	AssDialogue frame_safe_line = exact_cursor_line;
	frame_safe_line.End = frame_safe_end;
	EXPECT_TRUE(IsAssDialogueVisibleAtTimeForOutput(
		frame_safe_line.Start,
		frame_safe_line.End,
		cursor_ms,
		AssTimeOutputMode::LegacyRounding));
}

TEST(ass_time_projection, frame_boundary_snap_uses_storage_projection_to_preserve_frame_semantics) {
	auto const fps = agi::vfr::Framerate(24000, 1001);
	int const frame = 25397;
	int const start_ms = fps.TimeAtFrame(frame, agi::vfr::START);
	int const end_ms = fps.TimeAtFrame(frame, agi::vfr::END);

	int const projected_start = ProjectAssTimeForStorage(start_ms, AssStorageTimeBoundary::Start, &fps);
	int const projected_end = ProjectAssTimeForStorage(end_ms, AssStorageTimeBoundary::End, &fps);

	EXPECT_EQ(0, projected_start % 10);
	EXPECT_EQ(0, projected_end % 10);
	EXPECT_EQ(fps.FrameAtTime(start_ms, agi::vfr::START), fps.FrameAtTime(projected_start, agi::vfr::START));
	EXPECT_EQ(fps.FrameAtTime(end_ms, agi::vfr::END), fps.FrameAtTime(projected_end, agi::vfr::END));
}

TEST(ass_time_projection, saved_ass_visibility_stays_contiguous_after_frame_split) {
	auto const fps = agi::vfr::Framerate(24000, 1001);
	int const split_after_frame = 25397;
	int const split_ms = fps.TimeAtFrame(split_after_frame, agi::vfr::END);

	AssDialogue first;
	first.Start = fps.TimeAtFrame(split_after_frame, agi::vfr::START);
	first.End = split_ms;
	first.Style = "Default";
	first.Text = "first";

	AssDialogue second;
	second.Start = split_ms;
	second.End = fps.TimeAtFrame(split_after_frame + 1, agi::vfr::END);
	second.Style = "Default";
	second.Text = "second";

	EXPECT_EQ(static_cast<int>(first.End), static_cast<int>(second.Start));
	EXPECT_EQ(first.End.GetAssFormatted(), second.Start.GetAssFormatted());

	int const split_frame_time = fps.TimeAtFrame(split_after_frame);
	int const next_frame_time = fps.TimeAtFrame(split_after_frame + 1);
	EXPECT_TRUE(IsAssDialogueVisibleAtTimeForOutput(first.Start, first.End, split_frame_time, AssTimeOutputMode::LegacyRounding, &fps));
	EXPECT_FALSE(IsAssDialogueVisibleAtTimeForOutput(first.Start, first.End, next_frame_time, AssTimeOutputMode::LegacyRounding, &fps));
	EXPECT_FALSE(IsAssDialogueVisibleAtTimeForOutput(second.Start, second.End, split_frame_time, AssTimeOutputMode::LegacyRounding, &fps));
	EXPECT_TRUE(IsAssDialogueVisibleAtTimeForOutput(second.Start, second.End, next_frame_time, AssTimeOutputMode::LegacyRounding, &fps));
}

TEST(ass_dialogue, exact_millisecond_dialogue_text_roundtrips_through_parser) {
	AssDialogue line;
	line.Comment = false;
	line.Layer = 0;
	line.Start = 18497;
	line.End = 20499;
	line.Style = "Default";
	line.Text = "exact";

	AssDialogue parsed(line.GetEntryData(
		line.Start.GetAssFormatted(true),
		line.End.GetAssFormatted(true)));

	EXPECT_EQ(18497, parsed.Start.GetMillisecond());
	EXPECT_EQ(20499, parsed.End.GetMillisecond());
	EXPECT_EQ("exact", parsed.Text.get());
}

TEST(ass_dialogue, parses_file_times_with_multi_digit_hours) {
	AssDialogue parsed("Dialogue: 0,12:00:00.00,12:00:01.23,Default,,0,0,0,,long");

	EXPECT_EQ(12 * 60 * 60 * 1000, parsed.Start.GetMillisecond());
	EXPECT_EQ(12 * 60 * 60 * 1000 + 1230, parsed.End.GetMillisecond());
	EXPECT_EQ(
		"Dialogue: 0,12:00:00.00,12:00:01.23,Default,,0,0,0,,long",
		parsed.GetEntryData());
}

TEST(ass_time_projection, storage_serialization_preserves_times_past_ten_hours) {
	AssDialogue line;
	line.Comment = false;
	line.Layer = 0;
	line.Start = 12 * 60 * 60 * 1000;
	line.End = 12 * 60 * 60 * 1000 + 1230;
	line.Style = "Default";
	line.Text = "long";

	EXPECT_EQ(
		"Dialogue: 0,12:00:00.00,12:00:01.23,Default,,0,0,0,,long",
		SerializeAssDialogueForOutput(line, AssTimeOutputMode::LegacyRounding));
}

TEST(ass_dialogue, parses_aegisub_millisecond_precision_times) {
	AssDialogue parsed("Dialogue: 0,0:00:18.497,0:00:20.499,Default,,0,0,0,,exact");

	EXPECT_EQ(18497, parsed.Start.GetMillisecond());
	EXPECT_EQ(20499, parsed.End.GetMillisecond());
}

TEST(ass_dialogue, serializes_entry_data_with_shared_canonical_formatters) {
	AssDialogue line;
	line.Comment = false;
	line.Layer = -12;
	line.Start = 14189;
	line.End = 20495;
	line.Style = "Default";
	line.Actor = "Actor";
	line.Margin = { { 7, 8, 9 } };
	line.Effect = "Effect";
	line.ExtradataIds = std::vector<uint32_t>{1, 4294967295u};
	line.Text = "Text";

	EXPECT_EQ(
		"Dialogue: -12,0:00:14.19,0:00:20.50,Default,Actor,7,8,9,Effect,{=1=4294967295}Text",
		line.GetEntryData());
}

TEST(ass_dialogue, rejects_malformed_file_times) {
	EXPECT_THROW(
		AssDialogue("Dialogue: 0,1a:b2c3d:e4f5g.!6&7,0:00:01.00,Default,,0,0,0,,bad"),
		SubtitleFormatParseError);
}

TEST(ass_dialogue, rejects_hexadecimal_file_time_components) {
	EXPECT_THROW(
		AssDialogue("Dialogue: 0,0x0A:0x0B:0x0C.0x0D,0:00:01.00,Default,,0,0,0,,bad"),
		SubtitleFormatParseError);
	EXPECT_THROW(
		AssDialogue("Dialogue: 0,&H0A:&H0B:&H0C.&H0D,0:00:01.00,Default,,0,0,0,,bad"),
		SubtitleFormatParseError);
}

TEST(ass_dialogue, parses_compatible_integer_fields_and_normalizes_output) {
	AssDialogue parsed("Dialogue: 0x10junk,0:00:00.00,0:00:01.00,Default,,&H 20px,-15tail,+30more,,text");

	EXPECT_EQ(16, parsed.Layer);
	EXPECT_EQ(32, parsed.Margin[0]);
	EXPECT_EQ(-15, parsed.Margin[1]);
	EXPECT_EQ(30, parsed.Margin[2]);
	EXPECT_EQ(
		"Dialogue: 16,0:00:00.00,0:00:01.00,Default,,32,-15,30,,text",
		parsed.GetEntryData());
}

TEST(ass_dialogue, rejects_integer_fields_without_digits) {
	EXPECT_THROW(
		AssDialogue("Dialogue: nope,0:00:00.00,0:00:01.00,Default,,0,0,0,,text"),
		SubtitleFormatParseError);
}

TEST(ass_dialogue, override_parser_accepts_compatible_signed_fs_kt_and_fsc) {
	AssDialogue line;
	line.Text = "{\\fs+10\\fs-5\\kt50\\fsc}x";

	auto blocks = line.ParseTags();
	ASSERT_EQ(2u, blocks.size());
	ASSERT_EQ(AssBlockType::OVERRIDE, blocks[0]->GetType());

	auto *override_block = dynamic_cast<AssDialogueBlockOverride *>(blocks[0].get());
	ASSERT_NE(nullptr, override_block);
	ASSERT_EQ(4u, override_block->Tags.size());

	EXPECT_TRUE(override_block->Tags[0].IsValid());
	EXPECT_EQ("\\fs", override_block->Tags[0].Name);
	ASSERT_EQ(1u, override_block->Tags[0].Params.size());
	EXPECT_EQ("+10", override_block->Tags[0].Params[0].Get<std::string>());

	EXPECT_TRUE(override_block->Tags[1].IsValid());
	EXPECT_EQ("\\fs", override_block->Tags[1].Name);
	ASSERT_EQ(1u, override_block->Tags[1].Params.size());
	EXPECT_EQ("-5", override_block->Tags[1].Params[0].Get<std::string>());

	EXPECT_TRUE(override_block->Tags[2].IsValid());
	EXPECT_EQ("\\kt", override_block->Tags[2].Name);
	ASSERT_EQ(1u, override_block->Tags[2].Params.size());
	EXPECT_EQ(50, override_block->Tags[2].Params[0].Get<int>());

	EXPECT_TRUE(override_block->Tags[3].IsValid());
	EXPECT_EQ("\\fsc", override_block->Tags[3].Name);
	EXPECT_TRUE(override_block->Tags[3].Params.empty());
	EXPECT_EQ("{\\fs+10\\fs-5\\kt50\\fsc}", override_block->GetText());
}

TEST(ass_karaoke, kt_sets_absolute_syllable_start_instead_of_duration) {
	AssDialogue line;
	line.Start = 1000;
	line.End = 4000;
	line.Text = "{\\kt50\\k20}A{\\k30}B";

	AssKaraoke kara(&line, false, false);
	ASSERT_EQ(2u, kara.size());

	auto it = kara.begin();
	EXPECT_EQ(1500, it->start_time);
	EXPECT_EQ(200, it->duration);
	EXPECT_EQ("\\k", it->tag_type);
	EXPECT_TRUE(it->explicit_start);
	EXPECT_EQ(50, it->explicit_start_cs);
	EXPECT_EQ("A", it->text);
	EXPECT_EQ("{\\kt50\\k20}A", it->GetText(true));

	++it;
	EXPECT_EQ(1700, it->start_time);
	EXPECT_EQ(300, it->duration);
	EXPECT_EQ("B", it->text);
	EXPECT_FALSE(it->explicit_start);

	EXPECT_EQ("{\\kt50\\k20}A{\\k30}B", kara.GetText());
}

TEST(ass_dialogue, override_parser_distinguishes_empty_reset_parameters) {
	AssDialogue line;
	line.Text = "{\\fs\\bord()\\move(1,,3,)}x";

	auto blocks = line.ParseTags();
	ASSERT_EQ(2u, blocks.size());

	auto *override_block = dynamic_cast<AssDialogueBlockOverride *>(blocks[0].get());
	ASSERT_NE(nullptr, override_block);
	ASSERT_EQ(3u, override_block->Tags.size());

	ASSERT_EQ(1u, override_block->Tags[0].Params.size());
	EXPECT_EQ("\\fs", override_block->Tags[0].Name);
	EXPECT_FALSE(override_block->Tags[0].Params[0].omitted);
	EXPECT_TRUE(override_block->Tags[0].Params[0].empty);

	ASSERT_EQ(1u, override_block->Tags[1].Params.size());
	EXPECT_EQ("\\bord", override_block->Tags[1].Name);
	EXPECT_FALSE(override_block->Tags[1].Params[0].omitted);
	EXPECT_TRUE(override_block->Tags[1].Params[0].empty);

	ASSERT_EQ(6u, override_block->Tags[2].Params.size());
	EXPECT_EQ("\\move", override_block->Tags[2].Name);
	EXPECT_FALSE(override_block->Tags[2].Params[0].empty);
	EXPECT_TRUE(override_block->Tags[2].Params[1].empty);
	EXPECT_FALSE(override_block->Tags[2].Params[2].empty);
	EXPECT_TRUE(override_block->Tags[2].Params[3].empty);
	EXPECT_TRUE(override_block->Tags[2].Params[4].omitted);
	EXPECT_TRUE(override_block->Tags[2].Params[5].omitted);
	EXPECT_EQ("{\\fs\\bord\\move(1,,3,)}", override_block->GetText());
}

TEST(ass_dialogue, empty_override_parameters_read_as_semantic_defaults) {
	AssDialogue line;
	line.Text = "{\\fn\\fs}x";

	auto blocks = line.ParseTags();
	ASSERT_EQ(2u, blocks.size());

	auto *override_block = dynamic_cast<AssDialogueBlockOverride *>(blocks[0].get());
	ASSERT_NE(nullptr, override_block);
	ASSERT_EQ(2u, override_block->Tags.size());

	EXPECT_TRUE(override_block->Tags[0].Params[0].empty);
	EXPECT_EQ("Arial", override_block->Tags[0].Params[0].Get<std::string>("Arial"));
	EXPECT_TRUE(override_block->Tags[1].Params[0].empty);
	EXPECT_EQ(20.0, override_block->Tags[1].Params[0].Get<double>(20.0));
	EXPECT_EQ("{\\fn\\fs}", override_block->GetText());
}

TEST(ass_dialogue, empty_override_parameters_are_not_processed_as_values) {
	AssDialogue line;
	line.Text = "{\\bord\\shad4}x";

	auto blocks = line.ParseTags();
	ASSERT_EQ(2u, blocks.size());

	auto *override_block = dynamic_cast<AssDialogueBlockOverride *>(blocks[0].get());
	ASSERT_NE(nullptr, override_block);
	ASSERT_EQ(2u, override_block->Tags.size());

	int processed = 0;
	override_block->ProcessParameters([](std::string const&, AssOverrideParameter *param, void *userdata) {
		auto *count = static_cast<int *>(userdata);
		++*count;
		param->Set(param->Get<double>() * 2.0);
	}, &processed);

	EXPECT_EQ(1, processed);
	EXPECT_EQ("{\\bord\\shad8}", override_block->GetText());
}

TEST(ass_dialogue, override_float_parameters_parse_compatible_numeric_prefixes) {
	AssDialogue line;
	line.Text = "{\\fscx110.25tail\\fsp+1.5px\\bord-2.5em}x";

	auto blocks = line.ParseTags();
	ASSERT_EQ(2u, blocks.size());

	auto *override_block = dynamic_cast<AssDialogueBlockOverride *>(blocks[0].get());
	ASSERT_NE(nullptr, override_block);
	ASSERT_EQ(3u, override_block->Tags.size());

	EXPECT_EQ(110.25, override_block->Tags[0].Params[0].Get<double>());
	EXPECT_EQ(1.5f, override_block->Tags[1].Params[0].Get<float>());
	EXPECT_EQ(-2.5, override_block->Tags[2].Params[0].Get<double>());
	EXPECT_EQ("{\\fscx110.25tail\\fsp+1.5px\\bord-2.5em}", override_block->GetText());
}

TEST(ass_dialogue, override_int_parameters_parse_compatible_numeric_prefixes) {
	AssDialogue line;
	line.Text = "{\\be&H10tail\\q+2junk\\p-3more}x";

	auto blocks = line.ParseTags();
	ASSERT_EQ(2u, blocks.size());

	auto *override_block = dynamic_cast<AssDialogueBlockOverride *>(blocks[0].get());
	ASSERT_NE(nullptr, override_block);
	ASSERT_EQ(3u, override_block->Tags.size());

	EXPECT_EQ(16, override_block->Tags[0].Params[0].Get<int>());
	EXPECT_EQ(2, override_block->Tags[1].Params[0].Get<int>());
	EXPECT_EQ(-3, override_block->Tags[2].Params[0].Get<int>());
	EXPECT_EQ("{\\be&H10tail\\q+2junk\\p-3more}", override_block->GetText());
}

TEST(ass_dialogue, override_color_and_alpha_parameters_parse_compatible_hex_prefixes) {
	AssDialogue line;
	line.Text = "{\\c&H010203&tail\\1a&H123&junk\\alpha&H80&}x";

	auto blocks = line.ParseTags();
	ASSERT_EQ(2u, blocks.size());

	auto *override_block = dynamic_cast<AssDialogueBlockOverride *>(blocks[0].get());
	ASSERT_NE(nullptr, override_block);
	ASSERT_EQ(3u, override_block->Tags.size());

	EXPECT_EQ(agi::Color(0x03, 0x02, 0x01, 0x00), override_block->Tags[0].Params[0].Get<agi::Color>());
	EXPECT_EQ(0x23, override_block->Tags[1].Params[0].Get<int>());
	EXPECT_EQ(0x80, override_block->Tags[2].Params[0].Get<int>());
	EXPECT_EQ("{\\c&H010203&tail\\1a&H123&junk\\alpha&H80&}", override_block->GetText());
}

TEST(ass_dialogue, transform_parser_keeps_style_modifier_commas_together) {
	AssDialogue line;
	line.Text = "{\\t(0,1000,\\fnA,B\\bord5)}x";

	auto blocks = line.ParseTags();
	ASSERT_EQ(2u, blocks.size());

	auto *override_block = dynamic_cast<AssDialogueBlockOverride *>(blocks[0].get());
	ASSERT_NE(nullptr, override_block);
	ASSERT_EQ(1u, override_block->Tags.size());

	auto const& tag = override_block->Tags[0];
	EXPECT_EQ("\\t", tag.Name);
	ASSERT_EQ(4u, tag.Params.size());
	EXPECT_EQ(0, tag.Params[0].Get<int>());
	EXPECT_EQ(1000, tag.Params[1].Get<int>());
	EXPECT_TRUE(tag.Params[2].omitted);
	EXPECT_EQ("\\fnA,B\\bord5", tag.Params[3].Get<std::string>());
	EXPECT_EQ("{\\t(0,1000,\\fnA,B\\bord5)}", override_block->GetText());
}

TEST(ass_dialogue, transform_parser_keeps_nested_clip_commas_in_modifier_block) {
	AssDialogue line;
	line.Text = "{\\t(0,1000,2,\\clip(1,2,3,4)\\bord5)}x";

	auto blocks = line.ParseTags();
	ASSERT_EQ(2u, blocks.size());

	auto *override_block = dynamic_cast<AssDialogueBlockOverride *>(blocks[0].get());
	ASSERT_NE(nullptr, override_block);
	ASSERT_EQ(1u, override_block->Tags.size());

	auto const& tag = override_block->Tags[0];
	EXPECT_EQ("\\t", tag.Name);
	ASSERT_EQ(4u, tag.Params.size());
	EXPECT_EQ(0, tag.Params[0].Get<int>());
	EXPECT_EQ(1000, tag.Params[1].Get<int>());
	EXPECT_EQ(2.0, tag.Params[2].Get<double>());
	EXPECT_EQ("\\clip(1,2,3,4)\\bord5", tag.Params[3].Get<std::string>());
	EXPECT_EQ("{\\t(0,1000,2,\\clip(1,2,3,4)\\bord5)}", override_block->GetText());
}

TEST(ass_dialogue, transform_parser_keeps_nested_transform_modifier_intact) {
	AssDialogue line;
	line.Text = "{\\t(0,1000,\\t(0,500,\\bord5)\\shad3)}x";

	auto blocks = line.ParseTags();
	ASSERT_EQ(2u, blocks.size());

	auto *override_block = dynamic_cast<AssDialogueBlockOverride *>(blocks[0].get());
	ASSERT_NE(nullptr, override_block);
	ASSERT_EQ(1u, override_block->Tags.size());

	auto const& tag = override_block->Tags[0];
	EXPECT_EQ("\\t", tag.Name);
	ASSERT_EQ(4u, tag.Params.size());
	EXPECT_EQ(0, tag.Params[0].Get<int>());
	EXPECT_EQ(1000, tag.Params[1].Get<int>());
	EXPECT_TRUE(tag.Params[2].omitted);
	EXPECT_EQ("\\t(0,500,\\bord5)\\shad3", tag.Params[3].Get<std::string>());
	EXPECT_EQ("{\\t(0,1000,\\t(0,500,\\bord5)\\shad3)}", override_block->GetText());
}

TEST(ass_dialogue, transform_parser_keeps_signed_fs_and_fsc_modifier_intact) {
	AssDialogue line;
	line.Text = "{\\t(0,1000,2,\\fs+10\\fsc)}x";

	auto blocks = line.ParseTags();
	ASSERT_EQ(2u, blocks.size());

	auto *override_block = dynamic_cast<AssDialogueBlockOverride *>(blocks[0].get());
	ASSERT_NE(nullptr, override_block);
	ASSERT_EQ(1u, override_block->Tags.size());

	auto const& tag = override_block->Tags[0];
	EXPECT_EQ("\\t", tag.Name);
	ASSERT_EQ(4u, tag.Params.size());
	EXPECT_EQ(0, tag.Params[0].Get<int>());
	EXPECT_EQ(1000, tag.Params[1].Get<int>());
	EXPECT_EQ(2.0, tag.Params[2].Get<double>());
	EXPECT_EQ("\\fs+10\\fsc", tag.Params[3].Get<std::string>());

	auto *modifiers = tag.Params[3].Get<AssDialogueBlockOverride *>();
	ASSERT_EQ(2u, modifiers->Tags.size());
	EXPECT_EQ("\\fs", modifiers->Tags[0].Name);
	EXPECT_EQ(10.0, modifiers->Tags[0].Params[0].Get<double>());
	EXPECT_EQ("\\fsc", modifiers->Tags[1].Name);
	EXPECT_TRUE(modifiers->Tags[1].Params.empty());
	EXPECT_EQ("{\\t(0,1000,2,\\fs+10\\fsc)}", override_block->GetText());
}
