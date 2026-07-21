#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class AssDialogueBlockOverride;
class AssOverrideTag;
class AssStyle;

namespace aegisub::ass {

inline constexpr int DefaultFontWeight = 400;
inline constexpr int BoldFontWeight = 700;
inline constexpr int DefaultCharset = 1;

/// Font properties supplied by an ASS style before override tags are applied.
struct AssFontStyleBaseline {
	std::string family;
	int weight = DefaultFontWeight;
	bool italic = false;
	int charset = DefaultCharset;
	double height = 0.0;

	bool operator==(AssFontStyleBaseline const&) const = default;
};

/// Effective font request at one point in an ASS event.
///
/// A raw value is empty both before a tag is seen and for an explicitly empty
/// tag. The corresponding has_explicit_* flag distinguishes those cases.
struct AssFontRequest {
	std::string family;
	int effective_weight = DefaultFontWeight;
	bool italic = false;
	int charset = DefaultCharset;
	double height = 0.0;

	std::string raw_bold_tag;
	std::string raw_italic_tag;
	std::string raw_charset_tag;
	std::string raw_height_tag;

	bool has_explicit_family = false;
	bool has_explicit_bold = false;
	bool has_explicit_italic = false;
	bool has_explicit_charset = false;
	bool has_explicit_height = false;
	bool valid = true;

	bool operator==(AssFontRequest const&) const = default;
};

using AssFontResetStyleResolver =
	std::function<std::optional<AssFontStyleBaseline>(std::string_view style_name)>;

AssFontStyleBaseline MakeAssFontStyleBaseline(AssStyle const& style);

/// Encode an already evaluated weight for APIs which still accept ASS's
/// historical 0/1/numeric `bold` argument. This deliberately ignores raw tag
/// provenance and is used by generic platform/custom lister adapters.
int LegacyAssBoldFromEffectiveWeight(int effective_weight) noexcept;

/// Convert a rich request back to the integer `bold` argument used by legacy
/// font-lister APIs. A parseable explicit \b value is preserved verbatim so a
/// backend such as libass can apply its own normalization. Otherwise canonical
/// effective weights use the historical 0/1 representation.
int LegacyAssBoldArgument(AssFontRequest const& request) noexcept;

/// Normalize an already-parsed ASS \b value using VSFilter/GDI semantics.
/// Values other than 0, 1, or an explicit weight of at least 100 reset to the
/// event's original style weight.
int NormalizeVsfilterAssWeight(int raw_value, int event_style_weight) noexcept;

/// Normalize an already-parsed ASS \i value using VSFilter semantics.
/// Only 0 and 1 are accepted; all other values reset to the event style.
bool NormalizeVsfilterAssItalic(int raw_value, bool event_style_italic) noexcept;

/// Normalize an already-parsed ASS \fe value. Negative values request the
/// platform default charset, while non-negative values are preserved.
int NormalizeVsfilterAssCharset(int raw_value, int event_style_charset) noexcept;

/// Stateful evaluator for the font-affecting tags in one ASS event.
///
/// The event baseline never changes. A named \r installs another current style,
/// but an empty or invalid \fn/\b/\i/\fe/\fs still falls back to the original
/// event baseline, matching VSFilter's override behavior.
class AssFontStateEvaluator {
public:
	explicit AssFontStateEvaluator(AssFontStyleBaseline event_style);

	AssFontStyleBaseline const& EventStyle() const noexcept { return event_style; }
	AssFontRequest const& Request() const noexcept { return request; }
	bool IsValid() const noexcept { return request.valid; }

	void ResetToEventStyle();
	void ApplyTag(AssOverrideTag const& tag,
	              AssFontResetStyleResolver const& resolve_reset_style = {});
	void ApplyTags(std::vector<AssOverrideTag> const& tags,
	               AssFontResetStyleResolver const& resolve_reset_style = {});
	void ApplyBlock(AssDialogueBlockOverride const& block,
	                AssFontResetStyleResolver const& resolve_reset_style = {});

private:
	AssFontStyleBaseline event_style;
	AssFontRequest request;

	void SetCurrentStyle(AssFontStyleBaseline const& style);
};

AssFontRequest EvaluateAssFontTags(
	AssFontStyleBaseline event_style,
	std::vector<AssOverrideTag> const& tags,
	AssFontResetStyleResolver const& resolve_reset_style = {});

AssFontRequest EvaluateAssFontBlock(
	AssFontStyleBaseline event_style,
	AssDialogueBlockOverride const& block,
	AssFontResetStyleResolver const& resolve_reset_style = {});

} // namespace aegisub::ass
