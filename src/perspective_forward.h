// Copyright (c) 2022, arch1t3cht <arch1t3cht@gmail.com>
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

// Renderer-compatible forward math adapted from arch1t3cht/Aegisub commits
// f44883844f3179fb2f52b937854b1e6f2e014bee,
// 438aa6882b41f5488e31f3f4af31b324507e900b, and
// 947af58ad98e36dee47c6fbb88d89ae271cfa517.

#pragma once

#include "perspective_quad_geometry.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace perspective {

constexpr double MaxTransformParameter = 1.0e7;

struct Resolution {
	double width = 0.0;
	double height = 0.0;
};

enum class BoundsKind {
	Text,
	Drawing,
	MixedRuns,
	Dynamic,
	FontUnavailable,
};

struct BaseBounds {
	Rect rectangle;
	BoundsKind kind = BoundsKind::Text;
	// Drawing glyphs can have tight visible bounds which differ from the
	// control-point metrics used by the renderer for alignment.
	std::optional<Resolution> alignment_extent;
	// Applied after fax/fay, but before scale and rotation. This keeps drawing
	// baseline layout from being incorrectly sheared with the outline.
	Vec2 alignment_offset;
	// Additional source-space points which must participate in residual checks.
	// Drawing bounds populate this with normalized path endpoints and controls;
	// the solver always adds the rectangle, edge midpoints, center, outline and
	// shadow samples independently.
	std::vector<Vec2> residual_samples;
	// libass shears text per glyph line: every line's own baseline is the
	// shear reference and its layout offset is never sheared, so the forward
	// model's global x' = x + fax*y over this rectangle is only faithful for
	// a single line. Text bounds set this when the line breaks explicitly;
	// the solver freezes both shear axes for such bounds instead of reporting
	// a sheared target as exactly reachable.
	bool multiline_text = false;
};

[[nodiscard]] Vec2 ResolveBoundsAlignmentShift(
	BaseBounds const& bounds, int alignment);

struct EvaluatedTransformState {
	// The timestamp is carried with the evaluated state so callers cannot
	// accidentally reuse a state evaluated for a different event time.
	std::int64_t event_time_ms = 0;
	int alignment = 2;
	Vec2 position;
	std::optional<Vec2> origin;
	double scale_x = 100.0;
	double scale_y = 100.0;
	double shear_x = 0.0;
	double shear_y = 0.0;
	double rotation_x = 0.0;
	double rotation_y = 0.0;
	double rotation_z = 0.0;
	// Border/shadow values are retained for the later residual sampler. They
	// are validated here so a non-finite evaluated style can never reach math.
	double outline_x = 0.0;
	double outline_y = 0.0;
	double shadow_x = 0.0;
	double shadow_y = 0.0;
};

struct ForwardInput {
	Resolution play_resolution;
	std::optional<Resolution> layout_resolution;
	std::optional<Resolution> video_storage_resolution;
	BaseBounds bounds;
	EvaluatedTransformState state;
};

enum class ForwardError {
	None,
	UnsupportedMixedBounds,
	UnsupportedDynamicBounds,
	UnsupportedFontBounds,
	InvalidBounds,
	InvalidResolution,
	InvalidEventTime,
	InvalidAlignment,
	NonFiniteState,
	TransformParameterOutOfRange,
	DegenerateScale,
	MirroredTransform,
	SingularTransform,
	ProjectionDomain,
	InvalidQuad,
	SingularHomography,
};

struct ForwardResult {
	ForwardError error = ForwardError::None;
	GeometryError geometry_error = GeometryError::None;
	Quad quad {};
	Homography transform;
	Resolution resolved_layout;
	double camera_distance = 0.0;

	explicit operator bool() const { return error == ForwardError::None; }
};

[[nodiscard]] char const* DescribeForwardError(ForwardError error);
[[nodiscard]] ForwardResult ForwardQuad(ForwardInput const& input);
// Projects an override state while reusing the immutable resolutions and
// bounds in input. Solver trial states must not copy residual_samples.
[[nodiscard]] ForwardResult ForwardQuad(
	ForwardInput const& input, EvaluatedTransformState const& state);
}
