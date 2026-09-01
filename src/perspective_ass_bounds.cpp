// Copyright (c) 2013, Thomas Goyne <plorkyeran@aegisub.org>
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

// Text extent aggregation is adapted from arch1t3cht/Aegisub commit
// 68377edcf8b821acdf063c0eec113f23013654a5. Drawing bounds and the
// effective-state integration are repository-local.

#include "perspective_ass_bounds.h"

#include "ass_dialogue.h"
#include "ass_style.h"
#include "perspective_ass_state.h"

#include <libaegisub/ass/drawing.h>
#include <libaegisub/character_count.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace perspective {
namespace {

using agi::ass::drawing::AssDrawingCompatMode;
using agi::ass::drawing::LexemeType;
using agi::ass::drawing::PathVerb;

struct DrawingSyntax {
	bool invalid = false;
	bool unsupported_spline = false;
};

DrawingSyntax InspectDrawingSyntax(std::string const& drawing) {
	DrawingSyntax result;
	auto const lexemes = agi::ass::drawing::LexDrawing(drawing);
	std::size_t offset = 0;
	for (auto const& lexeme : lexemes) {
		if (lexeme.type == LexemeType::Error)
			result.invalid = true;
		else if (lexeme.type == LexemeType::Command && lexeme.length == 1
			&& (drawing[offset] == 's' || drawing[offset] == 'p' || drawing[offset] == 'c'))
			result.unsupported_spline = true;
		offset += lexeme.length;
	}
	return result;
}

bool IsOnlyDrawingWhitespace(std::string const& drawing) {
	return std::all_of(drawing.begin(), drawing.end(), [](unsigned char value) {
		return value == ' ' || value == '\t' || value == '\r' || value == '\n';
	});
}

std::vector<Vec2> NormalizedDrawingSamples(
	agi::ass::drawing::PathData const& path,
	double divisor) {
	std::vector<Vec2> samples;
	samples.reserve(path.commands.size() * 3);
	auto append = [&](agi::ass::drawing::Point point) {
		samples.push_back({point.x / divisor, point.y / divisor});
	};
	for (auto const& command : path.commands) {
		switch (command.verb) {
			case PathVerb::MoveTo:
			case PathVerb::LineTo:
				append(command.p1);
				break;
			case PathVerb::QuadTo:
			case PathVerb::ConicTo:
				append(command.p1);
				append(command.p2);
				break;
			case PathVerb::CubicTo:
				append(command.p1);
				append(command.p2);
				append(command.p3);
				break;
			case PathVerb::Close:
				break;
		}
	}
	return samples;
}

AssBoundsResult Unsupported(AssBoundsError error, BoundsKind kind) {
	AssBoundsResult result;
	result.error = error;
	result.value.kind = kind;
	return result;
}

std::vector<std::string> SplitTextLines(std::string const& text, int wrap_style) {
	std::vector<std::string> lines(1);
	for (std::size_t index = 0; index < text.size(); ++index) {
		if (text[index] != '\\' || index + 1 >= text.size()) {
			lines.back().push_back(text[index]);
			continue;
		}

		char const escaped = text[index + 1];
		if (escaped == 'N' || (escaped == 'n' && wrap_style == 2)) {
			lines.emplace_back();
			++index;
		}
		else if (escaped == 'h' || escaped == 'n') {
			lines.back().push_back(' ');
			++index;
		}
		else {
			lines.back().push_back(text[index]);
		}
	}
	return lines;
}

AssStyle MakeTextMeasurementStyle(EffectiveAssState::TextStyle const& effective) {
	AssStyle style;
	style.font = effective.font_name;
	style.fontsize = effective.font_size;
	style.spacing = effective.spacing;
	style.bold = effective.font_weight >= 600;
	style.italic = effective.italic;
	style.underline = effective.underline;
	style.strikeout = effective.strikeout;
	style.encoding = effective.encoding;
	// Perspective applies ASS scale after measuring the untransformed text box.
	style.scalex = 100.0;
	style.scaley = 100.0;
	return style;
}

bool ValidMetric(double value) {
	return std::isfinite(value) && value >= 0.0 && value <= MaxAbsCoordinate;
}

AssBoundsResult EvaluateTextBounds(
	std::string const& text,
	EffectiveAssState const& state,
	AssTextExtentsProvider provider) {
	auto const& text_style = state.text_style;
	auto style = MakeTextMeasurementStyle(text_style);
	if (!std::isfinite(style.fontsize) || style.fontsize <= 0.0
		|| style.fontsize > MaxAbsCoordinate
		|| !std::isfinite(style.spacing)
		|| std::abs(style.spacing) > MaxAbsCoordinate)
		return Unsupported(AssBoundsError::FontUnavailable, BoundsKind::FontUnavailable);

	double width = 0.0;
	double height = 0.0;
	for (auto const& line : SplitTextLines(text, text_style.wrap_style)) {
		// CalculateTextExtents historically assumes a non-empty string. Measure
		// one space for line height, but keep a genuinely empty line at zero width.
		std::string const measured_text = line.empty() ? " " : line;
		double line_width = 0.0;
		double line_height = 0.0;
		double descent = 0.0;
		double external_leading = 0.0;
		if (provider && !provider(&style, measured_text, line_width, line_height,
			descent, external_leading))
			return Unsupported(AssBoundsError::FontUnavailable, BoundsKind::FontUnavailable);
		if (!provider) {
			auto const characters = static_cast<double>(agi::CharacterCount(line, 0));
			line_width = characters * (style.fontsize + style.spacing);
			line_height = style.fontsize;
			descent = 0.0;
			external_leading = 0.0;
		}
		if (line.empty())
			line_width = 0.0;
		if (!ValidMetric(line_width) || !ValidMetric(line_height)
			|| !ValidMetric(descent) || !ValidMetric(external_leading))
			return Unsupported(AssBoundsError::FontUnavailable, BoundsKind::FontUnavailable);
		if (line_width > 0.0 && text_style.wrap_style != 2) {
			double const rendered_width = line_width
				* std::abs(state.transform.scale_x) / 100.0;
			if (!std::isfinite(text_style.available_wrap_width)
				|| text_style.available_wrap_width <= 0.0
				|| !std::isfinite(rendered_width)
				|| rendered_width >= text_style.available_wrap_width)
				return {AssBoundsError::UnsupportedAutomaticWrap};
		}
		width = std::max(width, line_width);
		height += line_height;
		if (!ValidMetric(height))
			return Unsupported(AssBoundsError::FontUnavailable, BoundsKind::FontUnavailable);
	}

	if (width <= 0.0 || height <= 0.0)
		return {AssBoundsError::EmptyGeometry};
	Rect const bounds {0.0, 0.0, width, height};
	auto const geometry_error = ValidateRect(bounds);
	if (geometry_error != GeometryError::None)
		return {AssBoundsError::FontUnavailable, geometry_error};

	AssBoundsResult result;
	result.value.rectangle = bounds;
	result.value.kind = BoundsKind::Text;
	return result;
}

}

char const* DescribeAssBoundsError(AssBoundsError error) {
	switch (error) {
		case AssBoundsError::None: return "Perspective ASS base bounds evaluated successfully";
		case AssBoundsError::InvalidInput: return "Perspective ASS bounds input is incomplete";
		case AssBoundsError::EmptyGeometry: return "the dialogue has no visible geometry";
		case AssBoundsError::FontUnavailable: return "renderer-compatible text font bounds are unavailable";
		case AssBoundsError::UnsupportedAutomaticWrap: return "automatic text wrapping does not have stable Perspective base bounds; use explicit line breaks or \\q2";
		case AssBoundsError::MixedGeometryRuns: return "mixed text and drawing runs do not have stable base bounds";
		case AssBoundsError::UnsupportedDrawingLayout: return "multiple ASS drawing runs require unsupported glyph layout";
		case AssBoundsError::UnsupportedDrawingCommand: return "ASS B-spline drawing metrics are not supported";
		case AssBoundsError::DrawingStateMismatch: return "the parsed drawing runs do not match the evaluated ASS state";
		case AssBoundsError::InvalidDrawingSyntax: return "the ASS drawing contains invalid syntax";
		case AssBoundsError::InvalidDrawingGeometry: return "the ASS drawing does not produce measurable geometry";
		case AssBoundsError::InvalidDrawingBounds: return "the ASS drawing bounds are degenerate or outside the supported range";
	}
	return "unknown Perspective ASS bounds error";
}

AssBoundsResult EvaluateAssBaseBounds(AssBoundsInput const& input) {
	if (!input.line || !input.state)
		return {AssBoundsError::InvalidInput};

	auto const& state = *input.state;
	auto blocks = input.line->ParseTags();
	std::string drawing;
	std::string plain_text;
	std::size_t visible_runs = 0;
	std::size_t drawing_runs = 0;
	bool has_text = false;
	bool has_drawing = false;
	bool drawing_state_mismatch = false;

	for (auto const& block : blocks) {
		auto const type = block->GetType();
		if (type != AssBlockType::PLAIN && type != AssBlockType::DRAWING)
			continue;

		auto const& text = type == AssBlockType::PLAIN
			? static_cast<AssDialogueBlockPlain const&>(*block).text
			: static_cast<AssDialogueBlockDrawing const&>(*block).text;
		if (text.empty())
			continue;
		++visible_runs;

		if (type == AssBlockType::PLAIN) {
			has_text = true;
			plain_text += text;
			continue;
		}

		has_drawing = true;
		++drawing_runs;
		auto const& drawing_block = static_cast<AssDialogueBlockDrawing const&>(*block);
		if (drawing_block.Scale != state.drawing_scale)
			drawing_state_mismatch = true;
		if (drawing_runs == 1)
			drawing = text;
	}

	if (!visible_runs)
		return {AssBoundsError::EmptyGeometry};
	if (has_text && has_drawing)
		return Unsupported(AssBoundsError::MixedGeometryRuns, BoundsKind::MixedRuns);
	if (has_text) {
		if (state.drawing_mode || visible_runs != state.geometry_run_count)
			return {AssBoundsError::DrawingStateMismatch};
		return EvaluateTextBounds(plain_text, state, input.text_extents);
	}
	if (!has_drawing)
		return {AssBoundsError::EmptyGeometry};
	if (drawing_runs > 1)
		return {AssBoundsError::UnsupportedDrawingLayout};
	if (!state.drawing_mode || state.drawing_scale < 1 || state.drawing_scale > 16
		|| drawing_state_mismatch || visible_runs != state.geometry_run_count)
		return {AssBoundsError::DrawingStateMismatch};
	if (IsOnlyDrawingWhitespace(drawing))
		return {AssBoundsError::EmptyGeometry};
	auto const syntax = InspectDrawingSyntax(drawing);
	if (syntax.invalid)
		return {AssBoundsError::InvalidDrawingSyntax};
	if (syntax.unsupported_spline)
		return {AssBoundsError::UnsupportedDrawingCommand};

	auto const path = agi::ass::drawing::ParseAss(
		drawing, AssDrawingCompatMode::Libass);
	agi::ass::drawing::Rect raw_bounds;
	if (!agi::ass::drawing::TryGetBounds(path, raw_bounds))
		return {AssBoundsError::InvalidDrawingGeometry};
	agi::ass::drawing::Rect metric_bounds;
	if (!agi::ass::drawing::TryGetControlPointBounds(path, metric_bounds))
		return {AssBoundsError::InvalidDrawingGeometry};

	double const divisor = std::ldexp(1.0, state.drawing_scale - 1);
	double const raw_ascent = std::max(
		metric_bounds.height - state.drawing_baseline_offset, 0.0);
	double const raw_descent = std::max(state.drawing_baseline_offset, 0.0);
	// The renderer positions the outline using clamped ascent/descent. This
	// residual is applied after shear so drawing baseline layout cannot alter
	// the outline's fax/fay geometry.
	double const top_offset = (
		state.drawing_baseline_offset - metric_bounds.height + raw_ascent) / divisor;
	Rect const bounds {
		raw_bounds.x / divisor,
		raw_bounds.y / divisor,
		(raw_bounds.x + raw_bounds.width) / divisor,
		(raw_bounds.y + raw_bounds.height) / divisor,
	};
	auto const geometry_error = ValidateRect(bounds);
	if (geometry_error != GeometryError::None)
		return {AssBoundsError::InvalidDrawingBounds, geometry_error};
	Resolution const alignment_extent {
		metric_bounds.width / divisor,
		(raw_ascent + raw_descent) / divisor,
	};
	if (!std::isfinite(alignment_extent.width)
		|| !std::isfinite(alignment_extent.height)
		|| alignment_extent.width <= 0.0 || alignment_extent.height <= 0.0
		|| alignment_extent.width > MaxAbsCoordinate
		|| alignment_extent.height > MaxAbsCoordinate)
		return {AssBoundsError::InvalidDrawingBounds, GeometryError::InvalidDomain};

	AssBoundsResult result;
	result.value = {
		bounds,
		BoundsKind::Drawing,
		alignment_extent,
		{0.0, top_offset},
		NormalizedDrawingSamples(path, divisor),
	};
	return result;
}

}
