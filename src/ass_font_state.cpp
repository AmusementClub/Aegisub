#include "ass_font_state.h"

#include "ass_compat.h"
#include "ass_dialogue.h"
#include "ass_style.h"

#include <cmath>
#include <utility>

namespace aegisub::ass {
namespace {

std::string RawValue(AssOverrideTag const& tag) {
	if (tag.Params.empty() || tag.Params.front().omitted || tag.Params.front().empty)
		return {};
	return tag.Params.front().Get<std::string>();
}

bool HasParameter(AssOverrideTag const& tag) {
	return !tag.Params.empty() && !tag.Params.front().omitted;
}

bool ParseInteger(std::string_view text, int& value) {
	return AssCompat::ParseInteger(text, value);
}

bool ParseHeight(std::string_view text, double& value) {
	return AssCompat::ParseFloat(text, value) && std::isfinite(value) && value > 0.0;
}

AssFontRequest RequestFromStyle(AssFontStyleBaseline const& style) {
	AssFontRequest request;
	request.family = style.family;
	request.effective_weight = style.weight;
	request.italic = style.italic;
	request.charset = style.charset;
	request.height = style.height;
	return request;
}

} // namespace

AssFontStyleBaseline MakeAssFontStyleBaseline(AssStyle const& style) {
	return {
		style.font,
		style.bold ? BoldFontWeight : DefaultFontWeight,
		style.italic,
		style.encoding,
		style.fontsize,
	};
}

int LegacyAssBoldFromEffectiveWeight(int effective_weight) noexcept {
	if (effective_weight == DefaultFontWeight)
		return 0;
	if (effective_weight == BoldFontWeight)
		return 1;
	return effective_weight;
}

int LegacyAssBoldArgument(AssFontRequest const& request) noexcept {
	if (request.has_explicit_bold && !request.raw_bold_tag.empty()) {
		int raw_value = 0;
		if (ParseInteger(request.raw_bold_tag, raw_value))
			return raw_value;
	}
	return LegacyAssBoldFromEffectiveWeight(request.effective_weight);
}

int NormalizeVsfilterAssWeight(int raw_value, int event_style_weight) noexcept {
	if (raw_value == 0)
		return DefaultFontWeight;
	if (raw_value == 1)
		return BoldFontWeight;
	if (raw_value >= 100)
		return raw_value;
	return event_style_weight;
}

bool NormalizeVsfilterAssItalic(int raw_value, bool event_style_italic) noexcept {
	if (raw_value == 0)
		return false;
	if (raw_value == 1)
		return true;
	return event_style_italic;
}

int NormalizeVsfilterAssCharset(int raw_value, int event_style_charset) noexcept {
	(void)event_style_charset;
	return raw_value < 0 ? DefaultCharset : raw_value;
}

AssFontStateEvaluator::AssFontStateEvaluator(AssFontStyleBaseline event_style)
: event_style(std::move(event_style))
, request(RequestFromStyle(this->event_style))
{
}

void AssFontStateEvaluator::SetCurrentStyle(AssFontStyleBaseline const& style) {
	request = RequestFromStyle(style);
}

void AssFontStateEvaluator::ResetToEventStyle() {
	SetCurrentStyle(event_style);
}

void AssFontStateEvaluator::ApplyTag(
	AssOverrideTag const& tag,
	AssFontResetStyleResolver const& resolve_reset_style) {
	if (tag.Name == "\\r") {
		if (!HasParameter(tag) || tag.Params.front().empty) {
			ResetToEventStyle();
			return;
		}

		auto const style_name = RawValue(tag);
		if (style_name.empty()) {
			ResetToEventStyle();
			return;
		}

		auto style = resolve_reset_style ? resolve_reset_style(style_name) : std::nullopt;
		if (style)
			SetCurrentStyle(*style);
		else
			request.valid = false;
		return;
	}

	if (!request.valid)
		return;

	if (tag.Name == "\\fn") {
		request.has_explicit_family = HasParameter(tag);
		if (!request.has_explicit_family || tag.Params.front().empty) {
			request.family = event_style.family;
			return;
		}

		auto family = RawValue(tag);
		request.family = family.empty() ? event_style.family : std::move(family);
		return;
	}

	if (tag.Name == "\\b") {
		request.has_explicit_bold = HasParameter(tag);
		request.raw_bold_tag = RawValue(tag);
		int value = 0;
		if (!request.has_explicit_bold || tag.Params.front().empty ||
		    !ParseInteger(request.raw_bold_tag, value)) {
			request.effective_weight = event_style.weight;
			return;
		}
		request.effective_weight = NormalizeVsfilterAssWeight(value, event_style.weight);
		return;
	}

	if (tag.Name == "\\i") {
		request.has_explicit_italic = HasParameter(tag);
		request.raw_italic_tag = RawValue(tag);
		int value = 0;
		if (!request.has_explicit_italic || tag.Params.front().empty ||
		    !ParseInteger(request.raw_italic_tag, value)) {
			request.italic = event_style.italic;
			return;
		}
		request.italic = NormalizeVsfilterAssItalic(value, event_style.italic);
		return;
	}

	if (tag.Name == "\\fe") {
		request.has_explicit_charset = HasParameter(tag);
		request.raw_charset_tag = RawValue(tag);
		int value = 0;
		if (!request.has_explicit_charset || tag.Params.front().empty ||
		    !ParseInteger(request.raw_charset_tag, value)) {
			request.charset = event_style.charset;
			return;
		}
		request.charset = NormalizeVsfilterAssCharset(value, event_style.charset);
		return;
	}

	if (tag.Name == "\\fs") {
		request.has_explicit_height = HasParameter(tag);
		request.raw_height_tag = RawValue(tag);
		double value = 0.0;
		if (!request.has_explicit_height || tag.Params.front().empty ||
		    !ParseHeight(request.raw_height_tag, value)) {
			request.height = event_style.height;
			return;
		}
		request.height = value;
		return;
	}

	if (tag.Name == "\\t" && tag.Params.size() > 3 &&
	    !tag.Params[3].omitted && !tag.Params[3].empty) {
		if (auto *nested = tag.Params[3].Get<AssDialogueBlockOverride*>())
			ApplyBlock(*nested, resolve_reset_style);
	}
}

void AssFontStateEvaluator::ApplyTags(
	std::vector<AssOverrideTag> const& tags,
	AssFontResetStyleResolver const& resolve_reset_style) {
	for (auto const& tag : tags)
		ApplyTag(tag, resolve_reset_style);
}

void AssFontStateEvaluator::ApplyBlock(
	AssDialogueBlockOverride const& block,
	AssFontResetStyleResolver const& resolve_reset_style) {
	ApplyTags(block.Tags, resolve_reset_style);
}

AssFontRequest EvaluateAssFontTags(
	AssFontStyleBaseline event_style,
	std::vector<AssOverrideTag> const& tags,
	AssFontResetStyleResolver const& resolve_reset_style) {
	AssFontStateEvaluator evaluator(std::move(event_style));
	evaluator.ApplyTags(tags, resolve_reset_style);
	return evaluator.Request();
}

AssFontRequest EvaluateAssFontBlock(
	AssFontStyleBaseline event_style,
	AssDialogueBlockOverride const& block,
	AssFontResetStyleResolver const& resolve_reset_style) {
	AssFontStateEvaluator evaluator(std::move(event_style));
	evaluator.ApplyBlock(block, resolve_reset_style);
	return evaluator.Request();
}

} // namespace aegisub::ass
