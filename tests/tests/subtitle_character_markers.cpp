#include <main.h>

#include "../../src/subtitle_character_markers.h"

#include <string>
#include <string_view>

using namespace aegisub;

namespace {

std::string Utf8(char32_t cp) {
	// Minimal UTF-8 encode for test literals.
	std::string out;
	if (cp <= 0x7F) {
		out.push_back(static_cast<char>(cp));
	} else if (cp <= 0x7FF) {
		out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
		out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
	} else if (cp <= 0xFFFF) {
		out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
		out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
		out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
	} else {
		out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
		out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
		out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
		out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
	}
	return out;
}

CharacterMarkerErrorConfig EnabledDefaults() {
	auto cfg = DefaultCharacterMarkerErrorConfig();
	cfg.enabled = true;
	return cfg;
}

} // namespace

TEST(subtitle_character_markers, ascii_plain_text_has_no_spans) {
	auto spans = ScanCharacterMarkers("Hello, world!");
	// Space at index 6 is a marker.
	ASSERT_EQ(1u, spans.size());
	EXPECT_EQ(CharacterMarkerKind::Space, spans[0].kind);
	EXPECT_EQ(6u, spans[0].byte_start);
}

TEST(subtitle_character_markers, plain_without_spaces_empty) {
	auto spans = ScanCharacterMarkers("Hello");
	EXPECT_TRUE(spans.empty());
}

TEST(subtitle_character_markers, ordinary_and_ideographic_space) {
	std::string text = "A" + Utf8(0x0020) + "B" + Utf8(0x3000) + "C";
	auto spans = ScanCharacterMarkers(text);
	ASSERT_EQ(2u, spans.size());
	EXPECT_EQ(CharacterMarkerKind::Space, spans[0].kind);
	EXPECT_EQ(1u, spans[0].byte_start);
	EXPECT_EQ(1u, spans[0].byte_length);
	EXPECT_EQ(CharacterMarkerKind::IdeographicSpace, spans[1].kind);
	EXPECT_EQ(3u, spans[1].byte_start);
	EXPECT_EQ(3u, spans[1].byte_length);

	auto cfg = EnabledDefaults();
	EXPECT_FALSE(IsCharacterMarkerError(spans[0], cfg));
	EXPECT_FALSE(IsCharacterMarkerError(spans[1], cfg));
}

TEST(subtitle_character_markers, nbsp_is_error_by_default) {
	std::string text = "A" + Utf8(0x00A0) + "B";
	auto spans = ScanCharacterMarkers(text);
	ASSERT_EQ(1u, spans.size());
	EXPECT_EQ(CharacterMarkerKind::NoBreakSpace, spans[0].kind);
	EXPECT_EQ(0x00A0u, spans[0].codepoint);

	auto cfg = EnabledDefaults();
	EXPECT_TRUE(IsCharacterMarkerError(spans[0], cfg));
	cfg.no_break_space = false;
	EXPECT_FALSE(IsCharacterMarkerError(spans[0], cfg));
}

TEST(subtitle_character_markers, cr_lf_and_crlf_byte_ranges) {
	std::string cr = "A\rB";
	auto spans_cr = ScanCharacterMarkers(cr);
	ASSERT_EQ(1u, spans_cr.size());
	EXPECT_EQ(CharacterMarkerKind::CarriageReturn, spans_cr[0].kind);
	EXPECT_EQ(1u, spans_cr[0].byte_start);
	EXPECT_EQ(1u, spans_cr[0].byte_length);

	std::string lf = "A\nB";
	auto spans_lf = ScanCharacterMarkers(lf);
	ASSERT_EQ(1u, spans_lf.size());
	EXPECT_EQ(CharacterMarkerKind::LineFeed, spans_lf[0].kind);

	std::string crlf = "A\r\nB";
	auto spans = ScanCharacterMarkers(crlf);
	ASSERT_EQ(2u, spans.size());
	EXPECT_EQ(CharacterMarkerKind::CarriageReturn, spans[0].kind);
	EXPECT_EQ(1u, spans[0].byte_start);
	EXPECT_EQ(1u, spans[0].byte_length);
	EXPECT_EQ(CharacterMarkerKind::LineFeed, spans[1].kind);
	EXPECT_EQ(2u, spans[1].byte_start);
	EXPECT_EQ(1u, spans[1].byte_length);

	auto cfg = EnabledDefaults();
	EXPECT_TRUE(IsCharacterMarkerError(spans[0], cfg));
	EXPECT_TRUE(IsCharacterMarkerError(spans[1], cfg));
}

TEST(subtitle_character_markers, tab_priority_over_whitespace) {
	auto spans = ScanCharacterMarkers("A\tB");
	ASSERT_EQ(1u, spans.size());
	EXPECT_EQ(CharacterMarkerKind::Tab, spans[0].kind);
	EXPECT_NE(CharacterMarkerKind::UnicodeWhitespace, spans[0].kind);

	auto cfg = EnabledDefaults();
	EXPECT_TRUE(IsCharacterMarkerError(spans[0], cfg));
	cfg.tab = false;
	EXPECT_FALSE(IsCharacterMarkerError(spans[0], cfg));
}

TEST(subtitle_character_markers, control_characters) {
	// ESC, DEL, and a C1 control (U+0085 NEL is white space — use U+0080).
	std::string text = std::string("A") + '\x1B' + '\x7F' + Utf8(0x0080) + "B";
	auto spans = ScanCharacterMarkers(text);
	ASSERT_EQ(3u, spans.size());
	EXPECT_EQ(CharacterMarkerKind::Control, spans[0].kind);
	EXPECT_EQ(0x1Bu, spans[0].codepoint);
	EXPECT_EQ(CharacterMarkerKind::Control, spans[1].kind);
	EXPECT_EQ(0x7Fu, spans[1].codepoint);
	EXPECT_EQ(CharacterMarkerKind::Control, spans[2].kind);
	EXPECT_EQ(0x80u, spans[2].codepoint);
	EXPECT_EQ("ESC", CharacterMarkerCompactLabel(0x1B, CharacterMarkerKind::Control));
	EXPECT_EQ("DEL", CharacterMarkerCompactLabel(0x7F, CharacterMarkerKind::Control));
}

TEST(subtitle_character_markers, known_invisible_and_bidi_join) {
	struct Case {
		char32_t cp;
		CharacterMarkerKind kind;
		char const* label;
	};
	Case cases[] = {
		{0xFEFF, CharacterMarkerKind::Invisible, "BOM"},
		{0x200B, CharacterMarkerKind::Invisible, "ZWSP"},
		{0x200E, CharacterMarkerKind::BidiControl, "LRM"},
		{0x200F, CharacterMarkerKind::BidiControl, "RLM"},
		{0x200C, CharacterMarkerKind::JoinControl, "ZWNJ"},
		{0x200D, CharacterMarkerKind::JoinControl, "ZWJ"},
		{0x2060, CharacterMarkerKind::Invisible, "WJ"},
		{0x202A, CharacterMarkerKind::BidiControl, "LRE"},
	};

	for (auto const& c : cases) {
		auto spans = ScanCharacterMarkers(Utf8(c.cp));
		ASSERT_EQ(1u, spans.size()) << "U+" << std::hex << static_cast<unsigned>(c.cp);
		EXPECT_EQ(c.kind, spans[0].kind) << "U+" << std::hex << static_cast<unsigned>(c.cp);
		EXPECT_EQ(c.label, CharacterMarkerCompactLabel(c.cp, c.kind));
	}
}

TEST(subtitle_character_markers, variation_selector_context_and_error_policy) {
	auto cfg = EnabledDefaults();

	auto standalone = ScanCharacterMarkers(Utf8(0xFE0F));
	ASSERT_EQ(1u, standalone.size());
	EXPECT_EQ(CharacterMarkerKind::Invisible, standalone[0].kind);
	EXPECT_EQ(CharacterContextAssessment::Suspicious, standalone[0].context);
	EXPECT_EQ(0u, standalone[0].byte_start);
	EXPECT_EQ(3u, standalone[0].byte_length);
	EXPECT_TRUE(IsCharacterMarkerError(standalone[0], cfg));
	EXPECT_EQ("VS16", CharacterMarkerCompactLabel(standalone[0].codepoint, standalone[0].kind));

	auto after_space = ScanCharacterMarkers(" " + Utf8(0xFE0F));
	ASSERT_EQ(2u, after_space.size());
	EXPECT_EQ(CharacterMarkerKind::Invisible, after_space[1].kind);
	EXPECT_EQ(CharacterContextAssessment::Suspicious, after_space[1].context);
	EXPECT_TRUE(IsCharacterMarkerError(after_space[1], cfg));

	auto valid = ScanCharacterMarkers("A" + Utf8(0xFE0F));
	ASSERT_EQ(1u, valid.size());
	EXPECT_EQ(CharacterMarkerKind::Invisible, valid[0].kind);
	EXPECT_EQ(CharacterContextAssessment::RecognizedValid, valid[0].context);
	EXPECT_EQ(1u, valid[0].byte_start);
	EXPECT_TRUE(IsCharacterMarkerError(valid[0], cfg));

	auto repeated = ScanCharacterMarkers("A" + Utf8(0xFE0F) + Utf8(0xFE0E));
	ASSERT_EQ(2u, repeated.size());
	EXPECT_EQ(CharacterContextAssessment::RecognizedValid, repeated[0].context);
	EXPECT_EQ(CharacterContextAssessment::Suspicious, repeated[1].context);
	EXPECT_TRUE(IsCharacterMarkerError(repeated[0], cfg));
	EXPECT_TRUE(IsCharacterMarkerError(repeated[1], cfg));

	cfg.variation_selector_context_policy = VariationSelectorContextPolicy::ExemptRecognizedValidContext;
	EXPECT_TRUE(IsCharacterMarkerError(standalone[0], cfg));
	EXPECT_TRUE(IsCharacterMarkerError(after_space[1], cfg));
	EXPECT_FALSE(IsCharacterMarkerError(valid[0], cfg));
	EXPECT_FALSE(IsCharacterMarkerError(repeated[0], cfg));
	EXPECT_TRUE(IsCharacterMarkerError(repeated[1], cfg));

	cfg.variation_selector_context_policy = VariationSelectorContextPolicy::NeverError;
	EXPECT_FALSE(IsCharacterMarkerError(standalone[0], cfg));
	EXPECT_FALSE(IsCharacterMarkerError(valid[0], cfg));

	cfg.variation_selector_context_policy = VariationSelectorContextPolicy::AlwaysUseCategory;
	cfg.other_invisible_characters = false;
	EXPECT_FALSE(IsCharacterMarkerError(standalone[0], cfg));
	EXPECT_FALSE(IsCharacterMarkerError(valid[0], cfg));
}

TEST(subtitle_character_markers, supplementary_variation_selector_offsets_and_label) {
	auto spans = ScanCharacterMarkers("A" + Utf8(0xE0100));
	ASSERT_EQ(1u, spans.size());
	EXPECT_EQ(CharacterMarkerKind::Invisible, spans[0].kind);
	EXPECT_EQ(CharacterContextAssessment::RecognizedValid, spans[0].context);
	EXPECT_EQ(1u, spans[0].byte_start);
	EXPECT_EQ(4u, spans[0].byte_length);
	EXPECT_EQ("VS17", CharacterMarkerCompactLabel(spans[0].codepoint, spans[0].kind));
}

TEST(subtitle_character_markers, multibyte_offsets) {
	// "日" is 3 UTF-8 bytes, then space, then "本".
	std::string text = Utf8(0x65E5) + " " + Utf8(0x672C);
	auto spans = ScanCharacterMarkers(text);
	ASSERT_EQ(1u, spans.size());
	EXPECT_EQ(3u, spans[0].byte_start);
	EXPECT_EQ(1u, spans[0].byte_length);
	EXPECT_EQ(CharacterMarkerKind::Space, spans[0].kind);
}

TEST(subtitle_character_markers, consecutive_spaces_are_separate_spans) {
	auto spans = ScanCharacterMarkers("A   B");
	ASSERT_EQ(3u, spans.size());
	EXPECT_EQ(1u, spans[0].byte_start);
	EXPECT_EQ(2u, spans[1].byte_start);
	EXPECT_EQ(3u, spans[2].byte_start);
}

TEST(subtitle_character_markers, consecutive_spaces_get_alternating_indicator_styles) {
	// Data stays unmerged (one indicator per space). Styles alternate so the UI
	// does not paint "   " as a single continuous Scintilla decoration run.
	CharacterMarkerShowConfig show{};
	show.space = true;
	CharacterMarkerErrorConfig error{};

	auto plan = BuildCharacterMarkerRenderPlan(ScanCharacterMarkers("A   B"), show, error);
	ASSERT_EQ(3u, plan.indicators.size());
	EXPECT_EQ(1u, plan.indicators[0].byte_start);
	EXPECT_EQ(1u, plan.indicators[0].byte_length);
	EXPECT_EQ(2u, plan.indicators[1].byte_start);
	EXPECT_EQ(1u, plan.indicators[1].byte_length);
	EXPECT_EQ(3u, plan.indicators[2].byte_start);
	EXPECT_EQ(1u, plan.indicators[2].byte_length);
	EXPECT_EQ(CharacterMarkerIndicatorStyle::Space, plan.indicators[0].style);
	EXPECT_EQ(CharacterMarkerIndicatorStyle::SpaceAlt, plan.indicators[1].style);
	EXPECT_EQ(CharacterMarkerIndicatorStyle::Space, plan.indicators[2].style);
}

TEST(subtitle_character_markers, invalid_utf8_does_not_hang) {
	// Lone continuation byte and truncated multi-byte sequence.
	std::string text = "A";
	text.push_back(static_cast<char>(0x80));
	text.push_back(static_cast<char>(0xE0));
	text.push_back(static_cast<char>(0x80));
	text.push_back('B');
	auto spans = ScanCharacterMarkers(text);
	// Should not crash; may or may not produce markers depending on recovery.
	for (auto const& span : spans) {
		EXPECT_LT(span.byte_start, text.size());
		EXPECT_LE(span.byte_start + span.byte_length, text.size());
		EXPECT_GT(span.byte_length, 0u);
	}
}

TEST(subtitle_character_markers, error_master_switch) {
	std::string text = Utf8(0x00A0);
	auto spans = ScanCharacterMarkers(text);
	ASSERT_EQ(1u, spans.size());

	auto off = DefaultCharacterMarkerErrorConfig();
	EXPECT_FALSE(IsCharacterMarkerError(spans[0], off));

	auto on = EnabledDefaults();
	EXPECT_TRUE(IsCharacterMarkerError(spans[0], on));
}

TEST(subtitle_character_markers, space_error_opt_in) {
	auto spans = ScanCharacterMarkers(" ");
	ASSERT_EQ(1u, spans.size());
	auto cfg = EnabledDefaults();
	EXPECT_FALSE(IsCharacterMarkerError(spans[0], cfg));
	cfg.space = true;
	EXPECT_TRUE(IsCharacterMarkerError(spans[0], cfg));
	cfg.ideographic_space = true;
	auto ideo = ScanCharacterMarkers(Utf8(0x3000));
	ASSERT_EQ(1u, ideo.size());
	EXPECT_TRUE(IsCharacterMarkerError(ideo[0], cfg));
}

TEST(subtitle_character_markers, join_control_policies) {
	// Bare ZWJ with no valid neighbors → not RecognizedValid.
	auto bare = ScanCharacterMarkers(Utf8(0x200D));
	ASSERT_EQ(1u, bare.size());
	EXPECT_EQ(CharacterMarkerKind::JoinControl, bare[0].kind);
	EXPECT_NE(CharacterContextAssessment::RecognizedValid, bare[0].context);

	auto cfg = EnabledDefaults();
	cfg.join_control_context_policy = JoinControlContextPolicy::AlwaysUseCategory;
	EXPECT_TRUE(IsCharacterMarkerError(bare[0], cfg));

	cfg.join_control_context_policy = JoinControlContextPolicy::NeverError;
	EXPECT_FALSE(IsCharacterMarkerError(bare[0], cfg));

	cfg.join_control_context_policy = JoinControlContextPolicy::ExemptRecognizedValidContext;
	cfg.join_controls = true;
	EXPECT_TRUE(IsCharacterMarkerError(bare[0], cfg));
}

TEST(subtitle_character_markers, emoji_zwj_sequence_recognized) {
	// 👩 U+1F469 + ZWJ + 🚀 U+1F680 is Extended_Pictographic ZWJ pair.
	// Use two Extended_Pictographic code points joined by ZWJ.
	std::string text = Utf8(0x1F469) + Utf8(0x200D) + Utf8(0x1F680);
	auto spans = ScanCharacterMarkers(text);
	ASSERT_EQ(1u, spans.size());
	EXPECT_EQ(CharacterMarkerKind::JoinControl, spans[0].kind);
	EXPECT_EQ(CharacterContextAssessment::RecognizedValid, spans[0].context);

	auto cfg = EnabledDefaults();
	cfg.join_control_context_policy = JoinControlContextPolicy::ExemptRecognizedValidContext;
	EXPECT_FALSE(IsCharacterMarkerError(spans[0], cfg));

	cfg.join_control_context_policy = JoinControlContextPolicy::AlwaysUseCategory;
	EXPECT_TRUE(IsCharacterMarkerError(spans[0], cfg));
}

TEST(subtitle_character_markers, emoji_zwj_with_skin_tone_extend_recognized) {
	// Extended_Pictographic + Emoji_Modifier (skin tone) + ZWJ + Extended_Pictographic.
	// U+1F3FB is EMOJI MODIFIER FITZPATRICK TYPE-1-2.
	std::string text = Utf8(0x1F469) + Utf8(0x1F3FB) + Utf8(0x200D) + Utf8(0x1F680);
	auto spans = ScanCharacterMarkers(text);
	ASSERT_EQ(1u, spans.size());
	EXPECT_EQ(CharacterMarkerKind::JoinControl, spans[0].kind);
	EXPECT_EQ(CharacterContextAssessment::RecognizedValid, spans[0].context);

	auto cfg = EnabledDefaults();
	cfg.join_control_context_policy = JoinControlContextPolicy::ExemptRecognizedValidContext;
	EXPECT_FALSE(IsCharacterMarkerError(spans[0], cfg));
}

TEST(subtitle_character_markers, repeated_zwj_is_suspicious) {
	// emoji + ZWJ + ZWJ + emoji: both join controls must be Suspicious.
	std::string text = Utf8(0x1F469) + Utf8(0x200D) + Utf8(0x200D) + Utf8(0x1F680);
	auto spans = ScanCharacterMarkers(text);
	ASSERT_EQ(2u, spans.size());
	EXPECT_EQ(CharacterContextAssessment::Suspicious, spans[0].context);
	EXPECT_EQ(CharacterContextAssessment::Suspicious, spans[1].context);

	auto cfg = EnabledDefaults();
	cfg.join_control_context_policy = JoinControlContextPolicy::ExemptRecognizedValidContext;
	EXPECT_TRUE(IsCharacterMarkerError(spans[0], cfg));
	EXPECT_TRUE(IsCharacterMarkerError(spans[1], cfg));
}

TEST(subtitle_character_markers, join_exempt_error_allows_representation_valid_does_not) {
	CharacterMarkerShowConfig show{};
	auto cfg = EnabledDefaults();
	cfg.join_control_context_policy = JoinControlContextPolicy::ExemptRecognizedValidContext;

	// Bare ZWJ is an error under exempt and must allow a representation so the
	// error unit has non-zero width (blob + indicator + dwell hit).
	auto bare = ScanCharacterMarkers(Utf8(0x200D));
	ASSERT_EQ(1u, bare.size());
	EXPECT_TRUE(IsCharacterMarkerError(bare[0], cfg));
	EXPECT_TRUE(CharacterMarkerNeedsVisual(bare[0], show, cfg));
	EXPECT_TRUE(CharacterMarkerAllowsGlobalRepresentation(bare[0], show, cfg));
	EXPECT_FALSE(CharacterMarkerCompactLabel(bare[0].codepoint, bare[0].kind).empty());

	// Recognized-valid emoji ZWJ: not an error, no rep when show is off.
	std::string emoji = Utf8(0x1F469) + Utf8(0x200D) + Utf8(0x1F680);
	auto valid = ScanCharacterMarkers(emoji);
	ASSERT_EQ(1u, valid.size());
	EXPECT_FALSE(IsCharacterMarkerError(valid[0], cfg));
	EXPECT_FALSE(CharacterMarkerNeedsVisual(valid[0], show, cfg));
	EXPECT_FALSE(CharacterMarkerAllowsGlobalRepresentation(valid[0], show, cfg));

	// Display category still allows a global rep when the user opts in.
	show.invisible_characters = true;
	EXPECT_TRUE(CharacterMarkerAllowsGlobalRepresentation(valid[0], show, cfg));
}

TEST(subtitle_character_markers, context_free_probe_must_not_drive_exempt_join_rep) {
	// ApplyCharacterMarkerSettings must not install ZWJ from a NotApplicable probe
	// under Exempt: that would label every valid emoji ZWJ in the document.
	CharacterMarkerShowConfig show{};
	auto cfg = EnabledDefaults();
	cfg.join_control_context_policy = JoinControlContextPolicy::ExemptRecognizedValidContext;

	CharacterMarkerSpan probe;
	probe.codepoint = 0x200D;
	probe.kind = CharacterMarkerKind::JoinControl;
	probe.context = CharacterContextAssessment::NotApplicable;
	// Probe looks like an error under category settings...
	EXPECT_TRUE(IsCharacterMarkerError(probe, cfg));
	// ...but STC must not use context-free probes for install under exempt.
	// The public API still returns true for any error span; the STC contract is
	// to only pass real scanned spans. Document that Valid-only emoji must not
	// allow rep:
	auto valid = ScanCharacterMarkers(Utf8(0x1F469) + Utf8(0x200D) + Utf8(0x1F680));
	ASSERT_EQ(1u, valid.size());
	EXPECT_FALSE(CharacterMarkerAllowsGlobalRepresentation(valid[0], show, cfg));
}

TEST(subtitle_character_markers, emoji_zwj_does_not_skip_extend_on_right) {
	// Right side must be the immediate next code point (no Extend* on the right).
	struct Case {
		char32_t between;
		char const* name;
	};
	Case cases[] = {
		{0x1F3FB, "emoji modifier skin tone"},
		{0xFE0F, "variation selector-16"},
		{0x0300, "combining grave accent"},
	};
	for (auto const& c : cases) {
		std::string text = Utf8(0x1F469) + Utf8(0x200D) + Utf8(c.between) + Utf8(0x1F680);
		auto spans = ScanCharacterMarkers(text);
		ASSERT_FALSE(spans.empty()) << c.name;
		EXPECT_EQ(CharacterMarkerKind::JoinControl, spans[0].kind) << c.name;
		EXPECT_EQ(CharacterContextAssessment::Suspicious, spans[0].context) << c.name;
	}
}

TEST(subtitle_character_markers, emoji_zwj_immediate_ep_on_right_still_valid) {
	// EP + ZWJ + EP with nothing between remains RecognizedValid.
	std::string text = Utf8(0x1F469) + Utf8(0x200D) + Utf8(0x1F680);
	auto spans = ScanCharacterMarkers(text);
	ASSERT_EQ(1u, spans.size());
	EXPECT_EQ(CharacterContextAssessment::RecognizedValid, spans[0].context);
}

TEST(subtitle_character_markers, script_join_context_recognized) {
	// Arabic Beh (U+0628, Dual Joining) + ZWJ + Beh.
	std::string text = Utf8(0x0628) + Utf8(0x200D) + Utf8(0x0628);
	auto spans = ScanCharacterMarkers(text);
	ASSERT_EQ(1u, spans.size());
	EXPECT_EQ(CharacterContextAssessment::RecognizedValid, spans[0].context);
}

TEST(subtitle_character_markers, script_join_beh_zwj_alef_recognized) {
	// BEH (Dual) + ZWJ + ALEF (Right_Joining) — common and ContextJ-valid.
	// U+0628 BEH Dual, U+0627 ALEF Right_Joining.
	std::string text = Utf8(0x0628) + Utf8(0x200D) + Utf8(0x0627);
	auto spans = ScanCharacterMarkers(text);
	ASSERT_EQ(1u, spans.size());
	EXPECT_EQ(CharacterContextAssessment::RecognizedValid, spans[0].context);

	auto cfg = EnabledDefaults();
	cfg.join_control_context_policy = JoinControlContextPolicy::ExemptRecognizedValidContext;
	EXPECT_FALSE(IsCharacterMarkerError(spans[0], cfg));
}

TEST(subtitle_character_markers, script_join_alef_zwj_beh_suspicious) {
	// ALEF (Right_Joining) + ZWJ + BEH (Dual) — wrong side for Right_Joining on the left.
	std::string text = Utf8(0x0627) + Utf8(0x200D) + Utf8(0x0628);
	auto spans = ScanCharacterMarkers(text);
	ASSERT_EQ(1u, spans.size());
	EXPECT_EQ(CharacterContextAssessment::Suspicious, spans[0].context);
}

TEST(subtitle_character_markers, script_join_rejects_non_joining) {
	std::string text = std::string("A") + Utf8(0x200D) + "B";
	auto spans = ScanCharacterMarkers(text);
	ASSERT_EQ(1u, spans.size());
	EXPECT_EQ(CharacterContextAssessment::Suspicious, spans[0].context);
}

TEST(subtitle_character_markers, lrm_rlm_not_context_validated) {
	// LRM/RLM are bidi controls; context assessment is NotApplicable.
	for (char32_t cp : {char32_t{0x200E}, char32_t{0x200F}}) {
		auto spans = ScanCharacterMarkers("A" + Utf8(cp) + "B");
		ASSERT_EQ(1u, spans.size());
		EXPECT_EQ(CharacterMarkerKind::BidiControl, spans[0].kind);
		EXPECT_EQ(CharacterContextAssessment::NotApplicable, spans[0].context);
		auto cfg = EnabledDefaults();
		EXPECT_TRUE(IsCharacterMarkerError(spans[0], cfg));
	}
}

TEST(subtitle_character_markers, find_at_byte_binary_search) {
	auto spans = ScanCharacterMarkers("A  B");
	ASSERT_EQ(2u, spans.size());
	EXPECT_EQ(nullptr, FindCharacterMarkerAtByte(spans, 0));
	ASSERT_NE(nullptr, FindCharacterMarkerAtByte(spans, 1));
	EXPECT_EQ(1u, FindCharacterMarkerAtByte(spans, 1)->byte_start);
	ASSERT_NE(nullptr, FindCharacterMarkerAtByte(spans, 2));
	EXPECT_EQ(2u, FindCharacterMarkerAtByte(spans, 2)->byte_start);
	EXPECT_EQ(nullptr, FindCharacterMarkerAtByte(spans, 3));
}

TEST(subtitle_character_markers, show_config_categories) {
	CharacterMarkerShowConfig show{};
	EXPECT_FALSE(ShouldShowCharacterMarker(CharacterMarkerKind::Space, show));
	show.space = true;
	EXPECT_TRUE(ShouldShowCharacterMarker(CharacterMarkerKind::Space, show));
	show.unicode_whitespace = true;
	EXPECT_TRUE(ShouldShowCharacterMarker(CharacterMarkerKind::NoBreakSpace, show));
	EXPECT_TRUE(ShouldShowCharacterMarker(CharacterMarkerKind::UnicodeWhitespace, show));
	show.control_characters = true;
	EXPECT_TRUE(ShouldShowCharacterMarker(CharacterMarkerKind::Tab, show));
	show.invisible_characters = true;
	EXPECT_TRUE(ShouldShowCharacterMarker(CharacterMarkerKind::BidiControl, show));
}

TEST(subtitle_character_markers, needs_visual_forces_error_when_show_off) {
	CharacterMarkerShowConfig show{};
	auto cfg = EnabledDefaults();
	auto spans = ScanCharacterMarkers(Utf8(0x00A0));
	ASSERT_EQ(1u, spans.size());
	EXPECT_TRUE(CharacterMarkerNeedsVisual(spans[0], show, cfg));
	cfg.enabled = false;
	EXPECT_FALSE(CharacterMarkerNeedsVisual(spans[0], show, cfg));
	show.unicode_whitespace = true;
	EXPECT_TRUE(CharacterMarkerNeedsVisual(spans[0], show, cfg));
}

TEST(subtitle_character_markers, naturally_wide_unicode_spaces_keep_native_width) {
	CharacterMarkerShowConfig show{};
	show.unicode_whitespace = true;
	auto error = EnabledDefaults();

	for (char32_t cp : {char32_t{0x00A0}, char32_t{0x1680}, char32_t{0x2000}, char32_t{0x202F}}) {
		auto spans = ScanCharacterMarkers(Utf8(cp));
		ASSERT_EQ(1u, spans.size()) << "U+" << std::hex << static_cast<unsigned>(cp);
		EXPECT_EQ("Zs", CharacterMarkerGeneralCategoryCode(cp));
		EXPECT_FALSE(CharacterMarkerSpanNeedsRepresentation(spans[0]));
		EXPECT_TRUE(CharacterMarkerNeedsVisual(spans[0], show, error));
		EXPECT_FALSE(CharacterMarkerAllowsGlobalRepresentation(spans[0], show, error));

		CharacterMarkerShowConfig hidden{};
		EXPECT_TRUE(IsCharacterMarkerError(spans[0], error));
		EXPECT_TRUE(CharacterMarkerNeedsVisual(spans[0], hidden, error));
		EXPECT_FALSE(CharacterMarkerAllowsGlobalRepresentation(spans[0], hidden, error));
	}
}

TEST(subtitle_character_markers, line_and_paragraph_separators_use_representations) {
	CharacterMarkerShowConfig show{};
	show.unicode_whitespace = true;
	auto error = EnabledDefaults();

	for (char32_t cp : {char32_t{0x2028}, char32_t{0x2029}}) {
		auto spans = ScanCharacterMarkers(Utf8(cp));
		ASSERT_EQ(1u, spans.size()) << "U+" << std::hex << static_cast<unsigned>(cp);
		EXPECT_EQ(CharacterMarkerKind::UnicodeWhitespace, spans[0].kind);
		EXPECT_TRUE(CharacterMarkerSpanNeedsRepresentation(spans[0]));
		EXPECT_TRUE(CharacterMarkerAllowsGlobalRepresentation(spans[0], show, error));
	}
}

TEST(subtitle_character_markers, render_plan_tracks_representation_install_and_cleanup_targets) {
	CharacterMarkerShowConfig show{};
	auto error = EnabledDefaults();
	error.join_control_context_policy = JoinControlContextPolicy::ExemptRecognizedValidContext;

	auto error_spans = ScanCharacterMarkers(Utf8(0x200D));
	auto error_plan = BuildCharacterMarkerRenderPlan(error_spans, show, error);
	ASSERT_EQ(1u, error_plan.representations.size());
	EXPECT_EQ(0x200Du, error_plan.representations[0].codepoint);
	EXPECT_EQ("ZWJ", error_plan.representations[0].label);
	ASSERT_EQ(1u, error_plan.indicators.size());
	EXPECT_EQ(CharacterMarkerIndicatorStyle::Error, error_plan.indicators[0].style);
	EXPECT_EQ(0u, error_plan.indicators[0].byte_start);
	EXPECT_EQ(3u, error_plan.indicators[0].byte_length);

	auto valid_spans = ScanCharacterMarkers(Utf8(0x1F469) + Utf8(0x200D) + Utf8(0x1F680));
	auto valid_plan = BuildCharacterMarkerRenderPlan(valid_spans, show, error);
	// STC reconciles its installed mappings with this exact desired set, so the
	// previously installed ZWJ mapping is removed on this transition.
	EXPECT_TRUE(valid_plan.representations.empty());
	EXPECT_TRUE(valid_plan.indicators.empty());
}

TEST(subtitle_character_markers, render_plan_preserves_native_space_width_and_indicator_styles) {
	CharacterMarkerShowConfig show{};
	show.space = true;
	show.ideographic_space = true;
	show.unicode_whitespace = true;
	CharacterMarkerErrorConfig error{};

	auto spans = ScanCharacterMarkers(" " + Utf8(0x3000) + Utf8(0x00A0));
	auto plan = BuildCharacterMarkerRenderPlan(spans, show, error);
	EXPECT_TRUE(plan.representations.empty());
	ASSERT_EQ(3u, plan.indicators.size());
	// U+0020 and U+3000 are adjacent Space-style units → alternate for visual separation.
	EXPECT_EQ(CharacterMarkerIndicatorStyle::Space, plan.indicators[0].style);
	EXPECT_EQ(CharacterMarkerIndicatorStyle::SpaceAlt, plan.indicators[1].style);
	EXPECT_EQ(CharacterMarkerIndicatorStyle::OtherWhitespace, plan.indicators[2].style);

	auto error_config = EnabledDefaults();
	CharacterMarkerShowConfig hidden{};
	auto nbsp_error_plan = BuildCharacterMarkerRenderPlan(
		ScanCharacterMarkers(Utf8(0x00A0)), hidden, error_config);
	EXPECT_TRUE(nbsp_error_plan.representations.empty());
	ASSERT_EQ(1u, nbsp_error_plan.indicators.size());
	EXPECT_EQ(CharacterMarkerIndicatorStyle::Error, nbsp_error_plan.indicators[0].style);
}

TEST(subtitle_character_markers, render_plan_records_global_contextual_representation_limit) {
	CharacterMarkerShowConfig show{};
	auto error = EnabledDefaults();
	error.join_control_context_policy = JoinControlContextPolicy::ExemptRecognizedValidContext;
	error.variation_selector_context_policy = VariationSelectorContextPolicy::ExemptRecognizedValidContext;

	std::string zwj_text = Utf8(0x1F469) + Utf8(0x200D) + Utf8(0x1F680) + Utf8(0x200D);
	auto zwj_plan = BuildCharacterMarkerRenderPlan(ScanCharacterMarkers(zwj_text), show, error);
	ASSERT_EQ(1u, zwj_plan.representations.size());
	EXPECT_EQ(0x200Du, zwj_plan.representations[0].codepoint);
	ASSERT_EQ(1u, zwj_plan.indicators.size());
	EXPECT_EQ(zwj_text.size() - 3, zwj_plan.indicators[0].byte_start);

	std::string vs_text = "A" + Utf8(0xFE0F) + " " + Utf8(0xFE0F);
	auto vs_plan = BuildCharacterMarkerRenderPlan(ScanCharacterMarkers(vs_text), show, error);
	ASSERT_EQ(1u, vs_plan.representations.size());
	EXPECT_EQ(0xFE0Fu, vs_plan.representations[0].codepoint);
	EXPECT_EQ("VS16", vs_plan.representations[0].label);
	ASSERT_EQ(1u, vs_plan.indicators.size());
	EXPECT_EQ(vs_text.size() - 3, vs_plan.indicators[0].byte_start);
}

TEST(subtitle_character_markers, icu_name_and_category) {
	auto name = CharacterMarkerIcuName(0x00A0);
	EXPECT_FALSE(name.empty());
	// Official name is English and stable.
	EXPECT_NE(std::string::npos, name.find("NO-BREAK"));
	EXPECT_EQ("Zs", CharacterMarkerGeneralCategoryCode(0x00A0));
	EXPECT_EQ("Cc", CharacterMarkerGeneralCategoryCode(0x0009));
}

TEST(subtitle_character_markers, disabled_options_do_not_require_marker_scanning) {
	CharacterMarkerShowConfig show{};
	CharacterMarkerErrorConfig error{};
	EXPECT_FALSE(CharacterMarkersEnabled(show, error));

	show.space = true;
	EXPECT_TRUE(CharacterMarkersEnabled(show, error));
	show.space = false;

	error.enabled = true;
	error.space = false;
	error.ideographic_space = false;
	error.no_break_space = false;
	error.other_unicode_whitespace = false;
	error.line_endings = false;
	error.tab = false;
	error.other_control_characters = false;
	error.bidi_controls = false;
	error.join_controls = false;
	error.other_invisible_characters = false;
	EXPECT_FALSE(CharacterMarkersEnabled(show, error));

	error.join_controls = true;
	EXPECT_TRUE(CharacterMarkersEnabled(show, error));
}
