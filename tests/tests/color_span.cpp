#include <main.h>

#include "../../src/ass_override.h"
#include "../../src/subtitle_edit_ops.h"

#include <libaegisub/ass/dialogue_parser.h>
#include <libaegisub/color.h>

#include <string>
#include <utility>
#include <vector>

namespace {

std::string Utf8(char32_t cp) {
	std::string out;
	if (cp <= 0x7F)
		out.push_back(static_cast<char>(cp));
	else if (cp <= 0x7FF) {
		out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
		out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
	}
	else {
		out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
		out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
		out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
	}
	return out;
}

std::vector<aegisub::subtitle_edit_ops::ColorSpan> Spans(std::string const& text, bool split_words = false) {
	auto tokens = agi::ass::TokenizeDialogueBody(text);
	if (split_words)
		agi::ass::SplitWords(text, tokens);
	return aegisub::subtitle_edit_ops::FindColorSpans(text, tokens);
}

TEST(color_span, finds_basic_color_span) {
	auto spans = Spans("{\\c&HFF0000&}");
	ASSERT_EQ(1u, spans.size());
	EXPECT_EQ(3, spans[0].byte_start);
	EXPECT_EQ(9, spans[0].byte_length);
	EXPECT_EQ(agi::Color(0x00, 0x00, 0xFF), spans[0].color);
	EXPECT_EQ(1, spans[0].slot);
	EXPECT_FALSE(spans[0].is_alpha);
	EXPECT_FALSE(spans[0].nested);
}

TEST(color_span, offsets_are_bytes_with_multibyte_text) {
	// U+767D and U+9ED2 are three UTF-8 bytes each.
	std::string text = Utf8(0x767D) + "{\\1c&H00FF00&}" + Utf8(0x9ED2);
	auto spans = Spans(text);
	ASSERT_EQ(1u, spans.size());
	EXPECT_EQ(7, spans[0].byte_start);
	EXPECT_EQ(9, spans[0].byte_length);
	EXPECT_EQ(agi::Color(0x00, 0xFF, 0x00), spans[0].color);
	EXPECT_EQ(1, spans[0].slot);
}

TEST(color_span, offsets_are_bytes_across_adjacent_blocks) {
	auto spans = Spans("{\\c&H111111&}{\\c&H222222&}");
	ASSERT_EQ(2u, spans.size());
	EXPECT_EQ(3, spans[0].byte_start);
	EXPECT_EQ(agi::Color(0x11, 0x11, 0x11), spans[0].color);
	EXPECT_EQ(16, spans[1].byte_start);
	EXPECT_EQ(agi::Color(0x22, 0x22, 0x22), spans[1].color);
}

TEST(color_span, marks_nested_transform_tags) {
	auto spans = Spans("{\\c&H00FF00&\\t(\\c&HFF0000&\\i1)}");
	ASSERT_EQ(2u, spans.size());
	EXPECT_FALSE(spans[0].nested);
	EXPECT_TRUE(spans[1].nested);
}

TEST(color_span, nested_depth_recovers_after_transform) {
	auto spans = Spans("{\\t(\\i1)\\c&HFF0000&}");
	ASSERT_EQ(1u, spans.size());
	EXPECT_FALSE(spans[0].nested);
}

TEST(color_span, ignores_lookalike_names) {
	auto spans = Spans("{\\an8\\clip(0,0,100,100)\\fscx100\\c&HFFFFFF&}");
	ASSERT_EQ(1u, spans.size());
	EXPECT_EQ(agi::Color(0xFF, 0xFF, 0xFF), spans[0].color);
}

TEST(color_span, looks_alike_names_yield_no_spans) {
	EXPECT_TRUE(Spans("{\\an8\\clip(0,0,100,100)}").empty());
	EXPECT_TRUE(Spans("{\\fscx100\\fscy100}").empty());
	EXPECT_TRUE(Spans("{\\1cd&HFF0000&}").empty());
}

TEST(color_span, short_hex_matches_renderer) {
	// &HFF00& is green in libass/VSFilter: the two digits land in the low
	// (blue) and middle (green) bytes of the BGR value.
	auto spans = Spans("{\\c&HFF00&}");
	ASSERT_EQ(1u, spans.size());
	EXPECT_EQ(agi::Color(0x00, 0xFF, 0x00), spans[0].color);
}

TEST(color_span, pure_black_span_exists) {
	// A black swatch must still produce a span; keeping it visible is the
	// indicator VALUEBIT concern on the paint side.
	auto spans = Spans("{\\c&H000000&}");
	ASSERT_EQ(1u, spans.size());
	EXPECT_EQ(agi::Color(0x00, 0x00, 0x00), spans[0].color);
}

TEST(color_span, unparsable_parameter_yields_no_span) {
	EXPECT_TRUE(Spans("{\\c}").empty());
	EXPECT_TRUE(Spans("{\\c&}").empty());
	EXPECT_TRUE(Spans("{\\c&HZZ&}").empty());
}

TEST(color_span, whitespace_between_name_and_value) {
	auto spans = Spans("{\\c &HFF&}");
	ASSERT_EQ(1u, spans.size());
	EXPECT_EQ(agi::Color(0xFF, 0x00, 0x00), spans[0].color);
}

TEST(color_span, escaped_open_brace_yields_no_spans) {
	// ParseTags treats a backslash-escaped '{' as plain text, so the tag
	// lexed inside this run is not an override and gets no swatch.
	EXPECT_TRUE(Spans("foo\\{\\c&HFF0000&\\}bar").empty());
}

TEST(color_span, real_block_after_escaped_run_still_yields_spans) {
	auto spans = Spans("\\{junk\\} real {\\c&HFF&}");
	ASSERT_EQ(1u, spans.size());
	EXPECT_EQ(17, spans[0].byte_start);
	EXPECT_EQ(5, spans[0].byte_length);
	EXPECT_EQ(agi::Color(0xFF, 0x00, 0x00), spans[0].color);
}

TEST(color_span, classifies_alpha_tags) {
	auto spans = Spans("{\\alpha&H80&\\2a&HFF&}");
	ASSERT_EQ(2u, spans.size());
	EXPECT_TRUE(spans[0].is_alpha);
	EXPECT_EQ(0, spans[0].slot);
	EXPECT_EQ(0x80, spans[0].color.r);
	EXPECT_TRUE(spans[1].is_alpha);
	EXPECT_EQ(2, spans[1].slot);
}

TEST(color_span, slots_map_for_all_color_tags) {
	EXPECT_EQ(1, Spans("{\\c&HFF&}")[0].slot);
	EXPECT_EQ(1, Spans("{\\1c&HFF&}")[0].slot);
	EXPECT_EQ(2, Spans("{\\2c&HFF&}")[0].slot);
	EXPECT_EQ(3, Spans("{\\3c&HFF&}")[0].slot);
	EXPECT_EQ(4, Spans("{\\4c&HFF&}")[0].slot);
}

TEST(color_span, results_survive_split_words) {
	// The edit box tokenizes then splits words; the splitter must not break
	// argument runs.
	auto spans = Spans("hello {\\c&HFF0000&} world", true);
	ASSERT_EQ(1u, spans.size());
	EXPECT_EQ(9, spans[0].byte_start);
	EXPECT_EQ(9, spans[0].byte_length);
}

TEST(color_span, value_bounds_strip_sigils) {
	auto text = std::string("{\\c&HFF0000&}");
	auto spans = Spans(text);
	ASSERT_EQ(1u, spans.size());
	EXPECT_EQ(std::make_pair(5, 6), aegisub::subtitle_edit_ops::GetColorValueBounds(text, spans[0]));
}

TEST(color_span, value_bounds_variants) {
	{
		auto text = std::string("{\\c&HFF00&}");
		auto spans = Spans(text);
		ASSERT_EQ(1u, spans.size());
		EXPECT_EQ(std::make_pair(5, 4), aegisub::subtitle_edit_ops::GetColorValueBounds(text, spans[0]));
	}
	{
		// No &H prefix: the whole run is the value.
		auto text = std::string("{\\cFF0000}");
		auto spans = Spans(text);
		ASSERT_EQ(1u, spans.size());
		EXPECT_EQ(std::make_pair(3, 6), aegisub::subtitle_edit_ops::GetColorValueBounds(text, spans[0]));
	}
	{
		auto text = std::string("{\\1c&H00FF00&}");
		auto spans = Spans(text);
		ASSERT_EQ(1u, spans.size());
		EXPECT_EQ(std::make_pair(6, 6), aegisub::subtitle_edit_ops::GetColorValueBounds(text, spans[0]));
	}
	{
		// Blank kept inside the argument run plus a lowercase sigil.
		auto text = std::string("{\\c &hFF&}");
		auto spans = Spans(text);
		ASSERT_EQ(1u, spans.size());
		EXPECT_EQ(std::make_pair(6, 2), aegisub::subtitle_edit_ops::GetColorValueBounds(text, spans[0]));
	}
}

TEST(color_span, tag_names_match_override_proto_classification) {
	// The span finder's name table is a copy of knowledge that the override
	// proto table in ass_override.cpp already owns. Drift between the two
	// would paint swatches for tags the colour-editing path classifies
	// differently, so pin the agreement here.
	for (auto name : aegisub::subtitle_edit_ops::ColorTagNames) {
		bool const is_alpha_name = name.back() == 'a';
		std::string tag_text = "\\";
		tag_text += name;
		tag_text += is_alpha_name ? "&H80&" : "&HFF0000&";

		AssOverrideTag tag(tag_text);
		ASSERT_TRUE(tag.IsValid()) << tag_text;
		ASSERT_FALSE(tag.Params.empty()) << tag_text;
		EXPECT_EQ(
			is_alpha_name ? AssParameterClass::ALPHA : AssParameterClass::COLOR,
			tag.Params[0].classification)
			<< tag_text;
	}
}
}
