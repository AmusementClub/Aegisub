/// @file subtitle_character_markers.h
/// @brief Pure UTF-8 character classification for STC visual markers.
///
/// Framework-free: no wx dependency. ICU is used for Unicode properties.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace aegisub {

enum class CharacterMarkerKind {
	Space,
	IdeographicSpace,
	NoBreakSpace,
	UnicodeWhitespace,
	CarriageReturn,
	LineFeed,
	Tab,
	Control,
	BidiControl,
	JoinControl,
	Invisible
};

enum class CharacterContextAssessment {
	NotApplicable,
	RecognizedValid,
	Suspicious,
	Unknown
};

/// Stable integer values for contextual character error-policy options.
enum class CharacterContextPolicy : int {
	AlwaysUseCategory = 0,
	ExemptRecognizedValidContext = 1,
	NeverError = 2
};

using JoinControlContextPolicy = CharacterContextPolicy;
using VariationSelectorContextPolicy = CharacterContextPolicy;

struct CharacterMarkerSpan {
	std::size_t byte_start = 0;
	std::size_t byte_length = 0;
	char32_t codepoint = 0;
	CharacterMarkerKind kind = CharacterMarkerKind::Space;
	CharacterContextAssessment context = CharacterContextAssessment::NotApplicable;
};

struct CharacterMarkerShowConfig {
	bool space = false;
	bool ideographic_space = false;
	bool unicode_whitespace = false;
	bool line_endings = false;
	bool control_characters = false;
	bool invisible_characters = false;
};

struct CharacterMarkerErrorConfig {
	bool enabled = false;
	bool space = false;
	bool ideographic_space = false;
	bool no_break_space = true;
	bool other_unicode_whitespace = true;
	bool line_endings = true;
	bool tab = true;
	bool other_control_characters = true;
	bool bidi_controls = true;
	bool join_controls = true;
	bool other_invisible_characters = true;
	JoinControlContextPolicy join_control_context_policy = JoinControlContextPolicy::AlwaysUseCategory;
	VariationSelectorContextPolicy variation_selector_context_policy = VariationSelectorContextPolicy::AlwaysUseCategory;
};

enum class CharacterMarkerIndicatorStyle {
	Space,
	/// Alternates with Space so Scintilla does not paint adjacent spaces as one run.
	SpaceAlt,
	OtherWhitespace,
	/// Alternates with OtherWhitespace for the same reason as SpaceAlt.
	OtherWhitespaceAlt,
	Error,
	/// Alternates with Error for consecutive error markers.
	ErrorAlt
};

struct CharacterMarkerRenderRepresentation {
	char32_t codepoint = 0;
	std::string label;
};

struct CharacterMarkerRenderIndicator {
	std::size_t byte_start = 0;
	std::size_t byte_length = 0;
	CharacterMarkerIndicatorStyle style = CharacterMarkerIndicatorStyle::Space;
};

/// Framework-free description of the exact marker visuals needed for a line.
/// Representations are unique by codepoint; indicators remain per occurrence.
struct CharacterMarkerRenderPlan {
	std::vector<CharacterMarkerRenderRepresentation> representations;
	std::vector<CharacterMarkerRenderIndicator> indicators;
};

/// Default error policy from product decisions (error master switch off).
CharacterMarkerErrorConfig DefaultCharacterMarkerErrorConfig();

/// Whether the option snapshot can produce any marker or error visual.
bool CharacterMarkersEnabled(
	CharacterMarkerShowConfig const& show,
	CharacterMarkerErrorConfig const& error);

/// Scan UTF-8 text and return marker spans sorted by byte_start.
/// Invalid UTF-8 sequences are skipped without crashing; first version does not
/// emit markers for invalid bytes.
std::vector<CharacterMarkerSpan> ScanCharacterMarkers(std::string_view utf8);

/// Decide whether a span is an error under the given policy snapshot.
bool IsCharacterMarkerError(CharacterMarkerSpan const& span, CharacterMarkerErrorConfig const& config);

/// Whether a non-error span should show a normal visual marker.
bool ShouldShowCharacterMarker(CharacterMarkerKind kind, CharacterMarkerShowConfig const& show);

/// Whether any visual treatment is needed (show or error).
bool CharacterMarkerNeedsVisual(CharacterMarkerSpan const& span,
	CharacterMarkerShowConfig const& show,
	CharacterMarkerErrorConfig const& error);

/// Build the desired representation mappings and indicator ranges for a line.
CharacterMarkerRenderPlan BuildCharacterMarkerRenderPlan(
	std::vector<CharacterMarkerSpan> const& spans,
	CharacterMarkerShowConfig const& show,
	CharacterMarkerErrorConfig const& error);

/// Whether a span may install a Scintilla SetRepresentation for its codepoint.
///
/// Representations are global per codepoint. For context-sensitive characters,
/// the error-only path returns true only when this occurrence is an error, so a
/// zero-width error gets a compact blob and a hittable indicator. A mixed line
/// can still show that global blob on recognized-valid occurrences of the same
/// codepoint; callers must always build mappings from real scanned spans.
bool CharacterMarkerAllowsGlobalRepresentation(CharacterMarkerSpan const& span,
	CharacterMarkerShowConfig const& show,
	CharacterMarkerErrorConfig const& error);

/// Compact untranslated editor representation label, or empty if none.
/// Fixed technical abbreviations are never translated.
std::string CharacterMarkerCompactLabel(char32_t codepoint, CharacterMarkerKind kind);

/// Whether this particular span needs a representation blob. This is
/// codepoint-aware because naturally wide Unicode space separators must retain
/// their native width and use an indicator instead.
bool CharacterMarkerSpanNeedsRepresentation(CharacterMarkerSpan const& span);

/// ICU official English character name (stable technical string), or empty.
std::string CharacterMarkerIcuName(char32_t codepoint);

/// Short general category code such as "Zs", "Cc", "Cf".
std::string CharacterMarkerGeneralCategoryCode(char32_t codepoint);

/// Binary search for a span covering the given UTF-8 byte position.
CharacterMarkerSpan const* FindCharacterMarkerAtByte(
	std::vector<CharacterMarkerSpan> const& spans, std::size_t byte_pos);

/// Map kind to the display-category option group used by the View menu.
enum class CharacterMarkerShowCategory {
	Space,
	IdeographicSpace,
	UnicodeWhitespace,
	LineEndings,
	ControlCharacters,
	InvisibleCharacters
};

CharacterMarkerShowCategory CharacterMarkerShowCategoryFor(CharacterMarkerKind kind);

} // namespace aegisub
