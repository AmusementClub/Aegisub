#include "subtitle_character_markers.h"

#include <unicode/uchar.h>
#include <unicode/utf8.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <optional>
#include <set>
#include <utility>

namespace aegisub {
namespace {

struct DecodedCodepoint {
	char32_t value = 0;
	std::size_t length = 0;
	bool valid = false;
};

DecodedCodepoint DecodeUtf8At(std::string_view text, std::size_t offset) {
	DecodedCodepoint result;
	if (offset >= text.size())
		return result;

	int32_t i = static_cast<int32_t>(offset);
	int32_t const length = static_cast<int32_t>(text.size());
	UChar32 codepoint = U_SENTINEL;
	U8_NEXT(reinterpret_cast<uint8_t const*>(text.data()), i, length, codepoint);
	if (codepoint < 0) {
		// Invalid sequence: advance one byte so we never loop forever.
		result.length = 1;
		result.valid = false;
		return result;
	}

	result.value = static_cast<char32_t>(codepoint);
	result.length = static_cast<std::size_t>(i) - offset;
	result.valid = true;
	return result;
}

bool IsBidiControl(char32_t cp) {
	return u_hasBinaryProperty(static_cast<UChar32>(cp), UCHAR_BIDI_CONTROL) != 0;
}

bool IsJoinControl(char32_t cp) {
	return u_hasBinaryProperty(static_cast<UChar32>(cp), UCHAR_JOIN_CONTROL) != 0;
}

bool IsDefaultIgnorable(char32_t cp) {
	return u_hasBinaryProperty(static_cast<UChar32>(cp), UCHAR_DEFAULT_IGNORABLE_CODE_POINT) != 0;
}

bool IsUnicodeWhiteSpace(char32_t cp) {
	return u_isUWhiteSpace(static_cast<UChar32>(cp)) != 0;
}

bool IsExtendedPictographic(char32_t cp) {
	return u_hasBinaryProperty(static_cast<UChar32>(cp), UCHAR_EXTENDED_PICTOGRAPHIC) != 0;
}

bool IsVariationSelector(char32_t cp) {
	return u_hasBinaryProperty(static_cast<UChar32>(cp), UCHAR_VARIATION_SELECTOR) != 0;
}

bool IsGraphemeBase(char32_t cp) {
	return u_hasBinaryProperty(static_cast<UChar32>(cp), UCHAR_GRAPHEME_BASE) != 0;
}

bool IsEmojiModifier(char32_t cp) {
	return u_hasBinaryProperty(static_cast<UChar32>(cp), UCHAR_EMOJI_MODIFIER) != 0;
}

bool IsGraphemeExtend(char32_t cp) {
	return u_hasBinaryProperty(static_cast<UChar32>(cp), UCHAR_GRAPHEME_EXTEND) != 0;
}

UJoiningType JoiningTypeOf(char32_t cp) {
	return static_cast<UJoiningType>(u_getIntPropertyValue(static_cast<UChar32>(cp), UCHAR_JOINING_TYPE));
}

bool IsTransparentJoining(char32_t cp) {
	return JoiningTypeOf(cp) == U_JT_TRANSPARENT;
}

/// Skip transparent joining marks and variation selectors, but never join controls.
/// Repeated ZWJ/ZWNJ must surface as Suspicious, not be skipped past.
bool IsSkippableInJoiningScript(char32_t cp) {
	return IsTransparentJoining(cp) || IsVariationSelector(cp);
}

/// Left of ZWJ in emoji sequences: Extended_Pictographic Extend* ZWJ ...
/// Skip skin tones and other extenders; never join controls.
bool IsSkippableLeftOfEmojiZwj(char32_t cp) {
	return IsTransparentJoining(cp)
		|| IsVariationSelector(cp)
		|| IsEmojiModifier(cp)
		|| IsGraphemeExtend(cp);
}

/// Virama has Canonical_Combining_Class = 9.
bool IsVirama(char32_t cp) {
	return u_getCombiningClass(static_cast<UChar32>(cp)) == 9;
}

/// ICU ContextJ-compatible: character to the left of ZWJ/ZWNJ may be
/// Left_Joining, Dual_Joining, or Join_Causing.
bool CanJoinAsLeftOfControl(char32_t cp) {
	switch (JoiningTypeOf(cp)) {
	case U_JT_LEFT_JOINING:
	case U_JT_DUAL_JOINING:
	case U_JT_JOIN_CAUSING:
		return true;
	default:
		return false;
	}
}

/// ICU ContextJ-compatible: character to the right of ZWJ/ZWNJ may be
/// Right_Joining, Dual_Joining, or Join_Causing.
bool CanJoinAsRightOfControl(char32_t cp) {
	switch (JoiningTypeOf(cp)) {
	case U_JT_RIGHT_JOINING:
	case U_JT_DUAL_JOINING:
	case U_JT_JOIN_CAUSING:
		return true;
	default:
		return false;
	}
}

template <typename SkipPred>
std::optional<char32_t> PrevCodepoint(std::string_view text, std::size_t byte_pos, SkipPred skip) {
	std::size_t pos = byte_pos;
	while (pos > 0) {
		int32_t i = static_cast<int32_t>(pos);
		UChar32 codepoint = U_SENTINEL;
		U8_PREV(reinterpret_cast<uint8_t const*>(text.data()), 0, i, codepoint);
		if (codepoint < 0)
			return std::nullopt;
		pos = static_cast<std::size_t>(i);
		auto const cp = static_cast<char32_t>(codepoint);
		if (skip(cp))
			continue;
		return cp;
	}
	return std::nullopt;
}

template <typename SkipPred>
std::optional<char32_t> NextCodepoint(std::string_view text, std::size_t byte_pos, SkipPred skip) {
	std::size_t pos = byte_pos;
	while (pos < text.size()) {
		auto decoded = DecodeUtf8At(text, pos);
		if (!decoded.valid)
			return std::nullopt;
		pos += decoded.length;
		if (skip(decoded.value))
			continue;
		return decoded.value;
	}
	return std::nullopt;
}

CharacterContextAssessment AssessJoinControlContext(std::string_view text, std::size_t byte_start, std::size_t byte_length, char32_t cp) {
	if (cp != 0x200C && cp != 0x200D)
		return CharacterContextAssessment::Unknown;

	// Neighbors for script joining: skip transparent/VS only — never join controls.
	auto const left = PrevCodepoint(text, byte_start, IsSkippableInJoiningScript);
	auto const right = NextCodepoint(text, byte_start + byte_length, IsSkippableInJoiningScript);

	if (!left || !right)
		return CharacterContextAssessment::Unknown;

	// Repeated join controls (e.g. ZWJ+ZWJ) are always suspicious.
	if (IsJoinControl(*left) || IsJoinControl(*right))
		return CharacterContextAssessment::Suspicious;

	// Adjacent whitespace / C0 controls are never a recognized-valid join site.
	if (IsUnicodeWhiteSpace(*left) || IsUnicodeWhiteSpace(*right))
		return CharacterContextAssessment::Suspicious;
	if ((*left <= 0x1F) || (*right <= 0x1F))
		return CharacterContextAssessment::Suspicious;

	if (cp == 0x200D) {
		// Extended_Pictographic Extend* ZWJ Extended_Pictographic.
		// Extend* is only on the left. On the right, require the immediate next
		// code point to be Extended_Pictographic — skip nothing (no VS, no
		// combining marks, no transparent joining).
		auto const left_emoji = PrevCodepoint(text, byte_start, IsSkippableLeftOfEmojiZwj);
		std::optional<char32_t> right_emoji;
		{
			auto const decoded = DecodeUtf8At(text, byte_start + byte_length);
			if (decoded.valid)
				right_emoji = decoded.value;
		}
		if (left_emoji && right_emoji
			&& IsExtendedPictographic(*left_emoji)
			&& IsExtendedPictographic(*right_emoji)
			&& !IsJoinControl(*left_emoji)
			&& !IsJoinControl(*right_emoji)) {
			return CharacterContextAssessment::RecognizedValid;
		}

		// Script joining (ICU ContextJ sides).
		if (CanJoinAsLeftOfControl(*left) && CanJoinAsRightOfControl(*right))
			return CharacterContextAssessment::RecognizedValid;

		return CharacterContextAssessment::Suspicious;
	}

	// ZWNJ: virama + letter (Indic), or compatible joining letters on both sides.
	if (IsVirama(*left) && !IsUnicodeWhiteSpace(*right) && !IsJoinControl(*right))
		return CharacterContextAssessment::RecognizedValid;
	if (CanJoinAsLeftOfControl(*left) && CanJoinAsRightOfControl(*right))
		return CharacterContextAssessment::RecognizedValid;

	return CharacterContextAssessment::Suspicious;
}

CharacterContextAssessment AssessVariationSelectorContext(std::string_view text, std::size_t byte_start) {
	auto const previous = PrevCodepoint(text, byte_start, [](char32_t) { return false; });
	if (!previous || IsVariationSelector(*previous))
		return CharacterContextAssessment::Suspicious;
	if (IsUnicodeWhiteSpace(*previous) || IsDefaultIgnorable(*previous)
		|| u_charType(static_cast<UChar32>(*previous)) == U_CONTROL_CHAR) {
		return CharacterContextAssessment::Suspicious;
	}

	// ICU's Grapheme_Base property is a structural check only. It avoids
	// flagging normal presentation modifiers without claiming that the exact
	// base-selector pair is a registered standardized variation sequence.
	return IsGraphemeBase(*previous)
		? CharacterContextAssessment::RecognizedValid
		: CharacterContextAssessment::Suspicious;
}

std::optional<CharacterMarkerKind> ClassifyCodepoint(char32_t cp) {
	// Priority order from the implementation plan.
	if (cp == 0x000D)
		return CharacterMarkerKind::CarriageReturn;
	if (cp == 0x000A)
		return CharacterMarkerKind::LineFeed;
	if (cp == 0x0009)
		return CharacterMarkerKind::Tab;

	// Other Cc control characters (including DEL and C1).
	auto const category = u_charType(static_cast<UChar32>(cp));
	if (category == U_CONTROL_CHAR)
		return CharacterMarkerKind::Control;

	if (cp == 0x0020)
		return CharacterMarkerKind::Space;
	if (cp == 0x3000)
		return CharacterMarkerKind::IdeographicSpace;
	if (cp == 0x00A0)
		return CharacterMarkerKind::NoBreakSpace;

	if (IsUnicodeWhiteSpace(cp))
		return CharacterMarkerKind::UnicodeWhitespace;

	if (IsBidiControl(cp))
		return CharacterMarkerKind::BidiControl;

	if (IsJoinControl(cp))
		return CharacterMarkerKind::JoinControl;

	// Other Default Ignorable / format characters that lack natural width.
	if (IsDefaultIgnorable(cp))
		return CharacterMarkerKind::Invisible;

	if (category == U_FORMAT_CHAR)
		return CharacterMarkerKind::Invisible;

	return std::nullopt;
}

char const* C0ShortName(char32_t cp) {
	static constexpr std::array<char const*, 33> names = {{
		"NUL", "SOH", "STX", "ETX", "EOT", "ENQ", "ACK", "BEL",
		"BS",  "TAB", "LF",  "VT",  "FF",  "CR",  "SO",  "SI",
		"DLE", "DC1", "DC2", "DC3", "DC4", "NAK", "SYN", "ETB",
		"CAN", "EM",  "SUB", "ESC", "FS",  "GS",  "RS",  "US",
		"DEL"
	}};
	if (cp <= 0x1F)
		return names[cp];
	if (cp == 0x7F)
		return names[32];
	return nullptr;
}

std::string HexCodepointLabel(char32_t cp) {
	char buf[16];
	if (cp <= 0xFFFF)
		std::snprintf(buf, sizeof(buf), "U+%04X", static_cast<unsigned>(cp));
	else
		std::snprintf(buf, sizeof(buf), "U+%06X", static_cast<unsigned>(cp));
	return buf;
}

bool IsContextualCharacterError(CharacterContextPolicy policy,
	CharacterContextAssessment context, bool category_enabled) {
	switch (policy) {
	case CharacterContextPolicy::NeverError:
		return false;
	case CharacterContextPolicy::ExemptRecognizedValidContext:
		if (context == CharacterContextAssessment::RecognizedValid)
			return false;
		return category_enabled;
	case CharacterContextPolicy::AlwaysUseCategory:
	default:
		return category_enabled;
	}
}

} // namespace

CharacterMarkerErrorConfig DefaultCharacterMarkerErrorConfig() {
	return {};
}

bool CharacterMarkersEnabled(
	CharacterMarkerShowConfig const& show,
	CharacterMarkerErrorConfig const& error) {
	bool const show_enabled =
		show.space
		|| show.ideographic_space
		|| show.unicode_whitespace
		|| show.line_endings
		|| show.control_characters
		|| show.invisible_characters;
	bool const error_enabled = error.enabled
		&& (error.space
			|| error.ideographic_space
			|| error.no_break_space
			|| error.other_unicode_whitespace
			|| error.line_endings
			|| error.tab
			|| error.other_control_characters
			|| error.bidi_controls
			|| error.join_controls
			|| error.other_invisible_characters);
	return show_enabled || error_enabled;
}

std::vector<CharacterMarkerSpan> ScanCharacterMarkers(std::string_view utf8) {
	std::vector<CharacterMarkerSpan> spans;
	std::size_t offset = 0;
	while (offset < utf8.size()) {
		auto decoded = DecodeUtf8At(utf8, offset);
		if (!decoded.valid) {
			offset += decoded.length ? decoded.length : 1;
			continue;
		}

		auto kind = ClassifyCodepoint(decoded.value);
		if (kind) {
			CharacterMarkerSpan span;
			span.byte_start = offset;
			span.byte_length = decoded.length;
			span.codepoint = decoded.value;
			span.kind = *kind;
			if (*kind == CharacterMarkerKind::JoinControl)
				span.context = AssessJoinControlContext(utf8, offset, decoded.length, decoded.value);
			else if (IsVariationSelector(decoded.value))
				span.context = AssessVariationSelectorContext(utf8, offset);
			else
				span.context = CharacterContextAssessment::NotApplicable;
			spans.push_back(span);
		}

		offset += decoded.length;
	}
	return spans;
}

bool IsCharacterMarkerError(CharacterMarkerSpan const& span, CharacterMarkerErrorConfig const& config) {
	if (!config.enabled)
		return false;

	switch (span.kind) {
	case CharacterMarkerKind::Space:
		return config.space;
	case CharacterMarkerKind::IdeographicSpace:
		return config.ideographic_space;
	case CharacterMarkerKind::NoBreakSpace:
		return config.no_break_space;
	case CharacterMarkerKind::UnicodeWhitespace:
		return config.other_unicode_whitespace;
	case CharacterMarkerKind::CarriageReturn:
	case CharacterMarkerKind::LineFeed:
		return config.line_endings;
	case CharacterMarkerKind::Tab:
		return config.tab;
	case CharacterMarkerKind::Control:
		return config.other_control_characters;
	case CharacterMarkerKind::BidiControl:
		return config.bidi_controls;
	case CharacterMarkerKind::JoinControl:
		return IsContextualCharacterError(config.join_control_context_policy,
			span.context, config.join_controls);
	case CharacterMarkerKind::Invisible:
		if (IsVariationSelector(span.codepoint))
			return IsContextualCharacterError(config.variation_selector_context_policy,
				span.context, config.other_invisible_characters);
		return config.other_invisible_characters;
	}
	return false;
}

bool ShouldShowCharacterMarker(CharacterMarkerKind kind, CharacterMarkerShowConfig const& show) {
	switch (CharacterMarkerShowCategoryFor(kind)) {
	case CharacterMarkerShowCategory::Space:
		return show.space;
	case CharacterMarkerShowCategory::IdeographicSpace:
		return show.ideographic_space;
	case CharacterMarkerShowCategory::UnicodeWhitespace:
		return show.unicode_whitespace;
	case CharacterMarkerShowCategory::LineEndings:
		return show.line_endings;
	case CharacterMarkerShowCategory::ControlCharacters:
		return show.control_characters;
	case CharacterMarkerShowCategory::InvisibleCharacters:
		return show.invisible_characters;
	}
	return false;
}

bool CharacterMarkerNeedsVisual(CharacterMarkerSpan const& span,
	CharacterMarkerShowConfig const& show,
	CharacterMarkerErrorConfig const& error) {
	if (IsCharacterMarkerError(span, error))
		return true;
	return ShouldShowCharacterMarker(span.kind, show);
}

bool CharacterMarkerAllowsGlobalRepresentation(CharacterMarkerSpan const& span,
	CharacterMarkerShowConfig const& show,
	CharacterMarkerErrorConfig const& error) {
	if (!CharacterMarkerSpanNeedsRepresentation(span))
		return false;
	if (!CharacterMarkerNeedsVisual(span, show, error))
		return false;

	// Display category is global and intentional for every occurrence.
	if (ShouldShowCharacterMarker(span.kind, show))
		return true;

	// Error-only path for context-sensitive characters.
	// Scintilla SetRepresentation is per-codepoint, not per-occurrence: installing
	// for an error ZWJ/VS also labels recognized-valid occurrences in the same line.
	// That is preferred over zero-width-only indicators, which are not reliably
	// visible or dwell-hit. Valid-only lines never reach here (NeedsVisual is false).
	//
	// Callers must pass spans with real context assessment, never context-free probes.
	return IsCharacterMarkerError(span, error);
}

namespace {

/// Scintilla paints consecutive ranges of the same indicator as one continuous
/// decoration. Alternate primary/alt styles so each adjacent unit stays distinct.
CharacterMarkerIndicatorStyle AlternateAdjacentIndicatorStyle(
	CharacterMarkerIndicatorStyle primary,
	CharacterMarkerRenderPlan const& plan,
	std::size_t byte_start) {
	if (plan.indicators.empty())
		return primary;

	auto const& prev = plan.indicators.back();
	if (prev.byte_start + prev.byte_length != byte_start)
		return primary;

	switch (primary) {
	case CharacterMarkerIndicatorStyle::Space:
		if (prev.style == CharacterMarkerIndicatorStyle::Space)
			return CharacterMarkerIndicatorStyle::SpaceAlt;
		if (prev.style == CharacterMarkerIndicatorStyle::SpaceAlt)
			return CharacterMarkerIndicatorStyle::Space;
		return primary;
	case CharacterMarkerIndicatorStyle::OtherWhitespace:
		if (prev.style == CharacterMarkerIndicatorStyle::OtherWhitespace)
			return CharacterMarkerIndicatorStyle::OtherWhitespaceAlt;
		if (prev.style == CharacterMarkerIndicatorStyle::OtherWhitespaceAlt)
			return CharacterMarkerIndicatorStyle::OtherWhitespace;
		return primary;
	case CharacterMarkerIndicatorStyle::Error:
		if (prev.style == CharacterMarkerIndicatorStyle::Error)
			return CharacterMarkerIndicatorStyle::ErrorAlt;
		if (prev.style == CharacterMarkerIndicatorStyle::ErrorAlt)
			return CharacterMarkerIndicatorStyle::Error;
		return primary;
	default:
		return primary;
	}
}

} // namespace

CharacterMarkerRenderPlan BuildCharacterMarkerRenderPlan(
	std::vector<CharacterMarkerSpan> const& spans,
	CharacterMarkerShowConfig const& show,
	CharacterMarkerErrorConfig const& error) {
	CharacterMarkerRenderPlan plan;
	std::set<char32_t> represented_codepoints;

	for (auto const& span : spans) {
		if (CharacterMarkerAllowsGlobalRepresentation(span, show, error)) {
			auto label = CharacterMarkerCompactLabel(span.codepoint, span.kind);
			if (!label.empty() && represented_codepoints.insert(span.codepoint).second)
				plan.representations.push_back({span.codepoint, std::move(label)});
		}

		bool const is_error = IsCharacterMarkerError(span, error);
		if (!is_error && !ShouldShowCharacterMarker(span.kind, show))
			continue;

		CharacterMarkerIndicatorStyle style;
		if (is_error) {
			style = CharacterMarkerIndicatorStyle::Error;
		} else if (span.kind == CharacterMarkerKind::NoBreakSpace
			|| span.kind == CharacterMarkerKind::UnicodeWhitespace) {
			style = CharacterMarkerIndicatorStyle::OtherWhitespace;
		} else {
			style = CharacterMarkerIndicatorStyle::Space;
		}
		// Keep one indicator entry per occurrence (never merge ranges). Alternating
		// styles only prevent Scintilla from drawing adjacent units as one box.
		style = AlternateAdjacentIndicatorStyle(style, plan, span.byte_start);
		plan.indicators.push_back({span.byte_start, span.byte_length, style});
	}

	return plan;
}

bool CharacterMarkerSpanNeedsRepresentation(CharacterMarkerSpan const& span) {
	switch (span.kind) {
	case CharacterMarkerKind::Space:
	case CharacterMarkerKind::IdeographicSpace:
	case CharacterMarkerKind::NoBreakSpace:
		// Prefer indicators over representation blobs so line wrap width stays natural.
		return false;
	case CharacterMarkerKind::UnicodeWhitespace:
		// Space separators have natural width. Line and paragraph separators do
		// not, so keep an explicit compact representation for those categories.
		return u_charType(static_cast<UChar32>(span.codepoint)) != U_SPACE_SEPARATOR;
	case CharacterMarkerKind::CarriageReturn:
	case CharacterMarkerKind::LineFeed:
	case CharacterMarkerKind::Tab:
	case CharacterMarkerKind::Control:
	case CharacterMarkerKind::BidiControl:
	case CharacterMarkerKind::JoinControl:
	case CharacterMarkerKind::Invisible:
		return true;
	}
	return false;
}

std::string CharacterMarkerCompactLabel(char32_t codepoint, CharacterMarkerKind kind) {
	switch (codepoint) {
	case 0x000D: return "CR";
	case 0x000A: return "LF";
	case 0x0009: return "TAB";
	case 0x00A0: return "NBSP";
	case 0x200B: return "ZWSP";
	case 0x200C: return "ZWNJ";
	case 0x200D: return "ZWJ";
	case 0x200E: return "LRM";
	case 0x200F: return "RLM";
	case 0xFEFF: return "BOM";
	case 0x2060: return "WJ";
	case 0x202A: return "LRE";
	case 0x202B: return "RLE";
	case 0x202C: return "PDF";
	case 0x202D: return "LRO";
	case 0x202E: return "RLO";
	case 0x2066: return "LRI";
	case 0x2067: return "RLI";
	case 0x2068: return "FSI";
	case 0x2069: return "PDI";
	default:
		break;
	}
	if (codepoint >= 0xFE00 && codepoint <= 0xFE0F)
		return "VS" + std::to_string(codepoint - 0xFE00 + 1);
	if (codepoint >= 0xE0100 && codepoint <= 0xE01EF)
		return "VS" + std::to_string(codepoint - 0xE0100 + 17);

	if (auto name = C0ShortName(codepoint))
		return name;

	// Prefer short technical names when known; otherwise U+XXXX.
	if (kind == CharacterMarkerKind::Control ||
	    kind == CharacterMarkerKind::BidiControl ||
	    kind == CharacterMarkerKind::JoinControl ||
	    kind == CharacterMarkerKind::Invisible ||
	    kind == CharacterMarkerKind::UnicodeWhitespace ||
	    kind == CharacterMarkerKind::NoBreakSpace) {
		return HexCodepointLabel(codepoint);
	}

	return {};
}

std::string CharacterMarkerIcuName(char32_t codepoint) {
	char buffer[256];
	UErrorCode status = U_ZERO_ERROR;
	int32_t len = u_charName(static_cast<UChar32>(codepoint), U_UNICODE_CHAR_NAME, buffer, sizeof(buffer), &status);
	if (U_FAILURE(status) || len <= 0)
		return {};
	return std::string(buffer, static_cast<std::size_t>(len));
}

std::string CharacterMarkerGeneralCategoryCode(char32_t codepoint) {
	int8_t const cat = u_charType(static_cast<UChar32>(codepoint));
	// u_getPropertyValueName for UCHAR_GENERAL_CATEGORY yields short codes.
	char const* name = u_getPropertyValueName(UCHAR_GENERAL_CATEGORY, cat, U_SHORT_PROPERTY_NAME);
	if (!name)
		return {};
	return name;
}

CharacterMarkerSpan const* FindCharacterMarkerAtByte(
	std::vector<CharacterMarkerSpan> const& spans, std::size_t byte_pos) {
	if (spans.empty())
		return nullptr;

	auto it = std::upper_bound(spans.begin(), spans.end(), byte_pos,
		[](std::size_t pos, CharacterMarkerSpan const& span) {
			return pos < span.byte_start;
		});

	if (it == spans.begin())
		return nullptr;
	--it;
	if (byte_pos >= it->byte_start && byte_pos < it->byte_start + it->byte_length)
		return &*it;
	return nullptr;
}

CharacterMarkerShowCategory CharacterMarkerShowCategoryFor(CharacterMarkerKind kind) {
	switch (kind) {
	case CharacterMarkerKind::Space:
		return CharacterMarkerShowCategory::Space;
	case CharacterMarkerKind::IdeographicSpace:
		return CharacterMarkerShowCategory::IdeographicSpace;
	case CharacterMarkerKind::NoBreakSpace:
	case CharacterMarkerKind::UnicodeWhitespace:
		return CharacterMarkerShowCategory::UnicodeWhitespace;
	case CharacterMarkerKind::CarriageReturn:
	case CharacterMarkerKind::LineFeed:
		return CharacterMarkerShowCategory::LineEndings;
	case CharacterMarkerKind::Tab:
	case CharacterMarkerKind::Control:
		return CharacterMarkerShowCategory::ControlCharacters;
	case CharacterMarkerKind::BidiControl:
	case CharacterMarkerKind::JoinControl:
	case CharacterMarkerKind::Invisible:
		return CharacterMarkerShowCategory::InvisibleCharacters;
	}
	return CharacterMarkerShowCategory::InvisibleCharacters;
}

} // namespace aegisub
