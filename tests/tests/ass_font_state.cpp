#include "../../src/ass_font_state.h"

#include "../../src/ass_dialogue.h"
#include "../../src/ass_style.h"
#include "../../src/font_matching_common.h"

#include <gtest/gtest.h>

#include <optional>
#include <string_view>

namespace {

using aegisub::ass::AssFontRequest;
using aegisub::ass::AssFontStateEvaluator;
using aegisub::ass::AssFontStyleBaseline;

AssFontStyleBaseline baseline(
	std::string family = "Event",
	int weight = 400,
	bool italic = false,
	int charset = 128,
	double height = 42.0) {
	return {std::move(family), weight, italic, charset, height};
}

AssDialogueBlockOverride block(std::string text) {
	if (text.size() >= 2 && text.front() == '{' && text.back() == '}')
		text = text.substr(1, text.size() - 2);
	AssDialogueBlockOverride result(std::move(text));
	result.ParseTags();
	return result;
}

AssFontRequest evaluate(std::string text, AssFontStyleBaseline event_style = baseline()) {
	auto tags = block(std::move(text));
	AssFontStateEvaluator evaluator(std::move(event_style));
	evaluator.ApplyBlock(tags);
	return evaluator.Request();
}

} // namespace

TEST(ass_font_state, vsfilter_weight_normalization_uses_event_baseline) {
	using aegisub::ass::NormalizeVsfilterAssWeight;

	EXPECT_EQ(400, NormalizeVsfilterAssWeight(0, 700));
	EXPECT_EQ(700, NormalizeVsfilterAssWeight(1, 400));
	EXPECT_EQ(700, NormalizeVsfilterAssWeight(-1, 700));
	EXPECT_EQ(700, NormalizeVsfilterAssWeight(2, 700));
	EXPECT_EQ(700, NormalizeVsfilterAssWeight(99, 700));
	EXPECT_EQ(100, NormalizeVsfilterAssWeight(100, 700));
	EXPECT_EQ(600, NormalizeVsfilterAssWeight(600, 700));
}

TEST(ass_font_state, style_baseline_expands_boolean_bold_to_gdi_weight) {
	AssStyle style;
	style.font = "Example";
	style.bold = true;
	style.italic = true;
	style.encoding = 204;
	style.fontsize = 33.5;

	auto converted = aegisub::ass::MakeAssFontStyleBaseline(style);
	EXPECT_EQ("Example", converted.family);
	EXPECT_EQ(700, converted.weight);
	EXPECT_TRUE(converted.italic);
	EXPECT_EQ(204, converted.charset);
	EXPECT_DOUBLE_EQ(33.5, converted.height);
}

TEST(ass_font_state, rich_request_preserves_the_legacy_ass_bold_argument) {
	EXPECT_EQ(0, aegisub::ass::LegacyAssBoldFromEffectiveWeight(400));
	EXPECT_EQ(1, aegisub::ass::LegacyAssBoldFromEffectiveWeight(700));
	EXPECT_EQ(600, aegisub::ass::LegacyAssBoldFromEffectiveWeight(600));

	AssFontRequest request;
	request.effective_weight = 400;
	EXPECT_EQ(0, aegisub::ass::LegacyAssBoldArgument(request));

	request.effective_weight = 700;
	EXPECT_EQ(1, aegisub::ass::LegacyAssBoldArgument(request));

	request.effective_weight = 600;
	EXPECT_EQ(600, aegisub::ass::LegacyAssBoldArgument(request));

	request.has_explicit_bold = true;
	request.raw_bold_tag = "-1";
	request.effective_weight = 400;
	EXPECT_EQ(-1, aegisub::ass::LegacyAssBoldArgument(request));

	request.raw_bold_tag = "2";
	request.effective_weight = 700;
	EXPECT_EQ(2, aegisub::ass::LegacyAssBoldArgument(request));

	request.raw_bold_tag = "invalid";
	EXPECT_EQ(1, aegisub::ass::LegacyAssBoldArgument(request));

	request.raw_bold_tag.clear();
	EXPECT_EQ(1, aegisub::ass::LegacyAssBoldArgument(request));
}

TEST(ass_font_state, libass_weight_normalization_remains_distinct) {
	EXPECT_EQ(700, NormalizeLibassAssWeight(-1));
	EXPECT_EQ(400, NormalizeLibassAssWeight(0));
	EXPECT_EQ(700, NormalizeLibassAssWeight(1));
	EXPECT_EQ(2, NormalizeLibassAssWeight(2));
	EXPECT_EQ(99, NormalizeLibassAssWeight(99));
	EXPECT_EQ(600, NormalizeLibassAssWeight(600));

	auto request = NormalizeLibassFontRequest("@Example", -1, true);
	EXPECT_EQ("Example", request.facename);
	EXPECT_EQ(700, request.requested_weight);
	EXPECT_TRUE(request.requested_italic);
}

TEST(ass_font_state, evaluates_raw_bold_values_without_collapsing_numeric_weights) {
	auto request = evaluate("{\\b600}");
	EXPECT_EQ(600, request.effective_weight);
	EXPECT_TRUE(request.has_explicit_bold);
	EXPECT_EQ("600", request.raw_bold_tag);

	request = evaluate("{\\b2}", baseline("Event", 700));
	EXPECT_EQ(700, request.effective_weight);
	EXPECT_TRUE(request.has_explicit_bold);
	EXPECT_EQ("2", request.raw_bold_tag);

	request = evaluate("{\\bnope}", baseline("Event", 700));
	EXPECT_EQ(700, request.effective_weight);
	EXPECT_TRUE(request.has_explicit_bold);
	EXPECT_EQ("nope", request.raw_bold_tag);
}

TEST(ass_font_state, explicit_empty_tags_are_distinct_from_absent_tags) {
	auto request = evaluate("{\\b\\i\\fe\\fn\\fs}", baseline("Event", 700, true, 134, 36.0));
	EXPECT_EQ(700, request.effective_weight);
	EXPECT_TRUE(request.italic);
	EXPECT_EQ(134, request.charset);
	EXPECT_EQ("Event", request.family);
	EXPECT_DOUBLE_EQ(36.0, request.height);
	EXPECT_TRUE(request.has_explicit_bold);
	EXPECT_TRUE(request.has_explicit_italic);
	EXPECT_TRUE(request.has_explicit_charset);
	EXPECT_TRUE(request.has_explicit_family);
	EXPECT_TRUE(request.has_explicit_height);
	EXPECT_TRUE(request.raw_bold_tag.empty());
	EXPECT_TRUE(request.raw_italic_tag.empty());

	request = evaluate("{}");
	EXPECT_FALSE(request.has_explicit_bold);
	EXPECT_FALSE(request.has_explicit_italic);
	EXPECT_FALSE(request.has_explicit_charset);
	EXPECT_FALSE(request.has_explicit_family);
	EXPECT_FALSE(request.has_explicit_height);
}

TEST(ass_font_state, italic_accepts_only_zero_and_one) {
	EXPECT_FALSE(evaluate("{\\i0}", baseline("Event", 400, true)).italic);
	EXPECT_TRUE(evaluate("{\\i1}").italic);
	EXPECT_TRUE(evaluate("{\\i2}", baseline("Event", 400, true)).italic);
	EXPECT_FALSE(evaluate("{\\i-1}").italic);
	EXPECT_TRUE(evaluate("{\\ix}", baseline("Event", 400, true)).italic);
}

TEST(ass_font_state, charset_empty_invalid_and_negative_follow_vsfilter_rules) {
	EXPECT_EQ(134, evaluate("{\\fe}", baseline("Event", 400, false, 134)).charset);
	EXPECT_EQ(134, evaluate("{\\fenope}", baseline("Event", 400, false, 134)).charset);
	EXPECT_EQ(1, evaluate("{\\fe-1}", baseline("Event", 400, false, 134)).charset);
	EXPECT_EQ(0, evaluate("{\\fe0}", baseline("Event", 400, false, 134)).charset);
	EXPECT_EQ(204, evaluate("{\\fe204}", baseline("Event", 400, false, 134)).charset);
}

TEST(ass_font_state, reset_style_changes_current_style_but_invalid_tags_use_event_baseline) {
	auto tags = block("{\\rReset\\b\\i2\\fn\\fe}");
	AssFontStateEvaluator evaluator(baseline("Event", 700, true, 128, 42.0));
	evaluator.ApplyBlock(tags, [](std::string_view name) -> std::optional<AssFontStyleBaseline> {
		if (name == "Reset")
			return baseline("Reset Family", 400, false, 204, 24.0);
		return std::nullopt;
	});

	auto const& request = evaluator.Request();
	EXPECT_TRUE(request.valid);
	EXPECT_EQ("Event", request.family);
	EXPECT_EQ(700, request.effective_weight);
	EXPECT_TRUE(request.italic);
	EXPECT_EQ(128, request.charset);
	EXPECT_DOUBLE_EQ(24.0, request.height);
}

TEST(ass_font_state, reset_clears_override_provenance) {
	auto tags = block("{\\b1\\i1\\fnOther\\fe204\\fs24\\rReset}");
	AssFontStateEvaluator evaluator(baseline());
	evaluator.ApplyBlock(tags, [](std::string_view) -> std::optional<AssFontStyleBaseline> {
		return baseline("Reset Family", 700, true, 204, 24.0);
	});

	auto const& request = evaluator.Request();
	EXPECT_EQ("Reset Family", request.family);
	EXPECT_EQ(700, request.effective_weight);
	EXPECT_TRUE(request.italic);
	EXPECT_FALSE(request.has_explicit_family);
	EXPECT_FALSE(request.has_explicit_bold);
	EXPECT_FALSE(request.has_explicit_italic);
	EXPECT_FALSE(request.has_explicit_charset);
	EXPECT_FALSE(request.has_explicit_height);
}

TEST(ass_font_state, unknown_reset_invalidates_until_an_event_reset) {
	auto tags = block("{\\rMissing\\fnIgnored\\b1}");
	AssFontStateEvaluator evaluator(baseline());
	evaluator.ApplyBlock(tags, [](std::string_view) -> std::optional<AssFontStyleBaseline> {
		return std::nullopt;
	});
	EXPECT_FALSE(evaluator.IsValid());

	auto recovery = block("{\\r\\b1}");
	evaluator.ApplyBlock(recovery);
	EXPECT_TRUE(evaluator.IsValid());
	EXPECT_EQ("Event", evaluator.Request().family);
	EXPECT_EQ(700, evaluator.Request().effective_weight);
}

TEST(ass_font_state, transform_font_tags_use_the_event_baseline) {
	auto tags = block("{\\rReset\\t(0,100,\\b\\i2\\fn\\fe-1)}");
	AssFontStateEvaluator evaluator(baseline("Event", 700, true, 128, 42.0));
	evaluator.ApplyBlock(tags, [](std::string_view) -> std::optional<AssFontStyleBaseline> {
		return baseline("Reset Family", 400, false, 204, 24.0);
	});

	auto const& request = evaluator.Request();
	EXPECT_EQ("Event", request.family);
	EXPECT_EQ(700, request.effective_weight);
	EXPECT_TRUE(request.italic);
	EXPECT_EQ(1, request.charset);
}

TEST(ass_font_state, blocks_can_be_applied_in_dialogue_order) {
	AssFontStateEvaluator evaluator(baseline());
	auto first = block("{\\fnFirst\\b600}");
	auto second = block("{\\i1}");
	evaluator.ApplyBlock(first);
	evaluator.ApplyBlock(second);

	EXPECT_EQ("First", evaluator.Request().family);
	EXPECT_EQ(600, evaluator.Request().effective_weight);
	EXPECT_TRUE(evaluator.Request().italic);
}
