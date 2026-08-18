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

#include "perspective_forward.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace perspective {
namespace {

constexpr double CameraDistanceBase = 312.5;
constexpr double DegreesToRadians = 3.1415926535897932384626433832795 / 180.0;
constexpr double MaxTransformParameter = 1.0e7;

struct Vec3 {
	double x = 0.0;
	double y = 0.0;
	double z = 0.0;
};

bool IsFinite(Resolution resolution) {
	return std::isfinite(resolution.width) && std::isfinite(resolution.height);
}

bool IsUsable(Resolution resolution) {
	return IsFinite(resolution)
		&& resolution.width > 0.0 && resolution.height > 0.0
		&& resolution.width <= MaxAbsCoordinate && resolution.height <= MaxAbsCoordinate;
}

bool IsFiniteAndSafe(Vec2 point) {
	return std::isfinite(point.x) && std::isfinite(point.y)
		&& std::abs(point.x) <= MaxAbsCoordinate
		&& std::abs(point.y) <= MaxAbsCoordinate;
}

ForwardError BoundsError(BoundsKind kind) {
	switch (kind) {
		case BoundsKind::Text:
		case BoundsKind::Drawing:
			return ForwardError::None;
		case BoundsKind::MixedRuns:
			return ForwardError::UnsupportedMixedBounds;
		case BoundsKind::Dynamic:
			return ForwardError::UnsupportedDynamicBounds;
		case BoundsKind::FontUnavailable:
			return ForwardError::UnsupportedFontBounds;
	}
	return ForwardError::InvalidBounds;
}

ForwardError ValidateResolutionInput(Resolution resolution, bool require_positive) {
	if (!IsFinite(resolution)
		|| std::abs(resolution.width) > MaxAbsCoordinate
		|| std::abs(resolution.height) > MaxAbsCoordinate)
		return ForwardError::InvalidResolution;
	if (require_positive && !IsUsable(resolution))
		return ForwardError::InvalidResolution;
	return ForwardError::None;
}

struct ResolvedLayout {
	ForwardError error = ForwardError::None;
	Resolution value;
	double camera_distance = 0.0;
};

ResolvedLayout ResolveLayoutResolution(ForwardInput const& input) {
	if (auto const error = ValidateResolutionInput(input.play_resolution, true);
		error != ForwardError::None)
		return {error};

	Resolution layout = input.play_resolution;
	if (input.layout_resolution) {
		if (auto const error = ValidateResolutionInput(*input.layout_resolution, false);
			error != ForwardError::None)
			return {error};
		if (IsUsable(*input.layout_resolution))
			layout = *input.layout_resolution;
		else if (input.video_storage_resolution) {
			if (auto const error = ValidateResolutionInput(*input.video_storage_resolution, false);
				error != ForwardError::None)
				return {error};
			if (IsUsable(*input.video_storage_resolution))
				layout = *input.video_storage_resolution;
		}
	} else if (input.video_storage_resolution) {
		if (auto const error = ValidateResolutionInput(*input.video_storage_resolution, false);
			error != ForwardError::None)
			return {error};
		if (IsUsable(*input.video_storage_resolution))
			layout = *input.video_storage_resolution;
	}

	double const camera_distance =
		CameraDistanceBase * input.play_resolution.height / layout.height;
	if (!std::isfinite(camera_distance) || camera_distance <= 0.0)
		return {ForwardError::InvalidResolution};
	return {ForwardError::None, layout, camera_distance};
}

ForwardError ValidateState(EvaluatedTransformState const& state) {
	if (state.event_time_ms < 0)
		return ForwardError::InvalidEventTime;
	if (state.alignment < 1 || state.alignment > 9)
		return ForwardError::InvalidAlignment;
	if (!IsFiniteAndSafe(state.position)
		|| (state.origin && !IsFiniteAndSafe(*state.origin)))
		return ForwardError::NonFiniteState;

	for (double const value : {
		state.scale_x, state.scale_y, state.shear_x, state.shear_y,
		state.rotation_x, state.rotation_y, state.rotation_z,
		state.outline_x, state.outline_y, state.shadow_x, state.shadow_y,
	}) {
		if (!std::isfinite(value))
			return ForwardError::NonFiniteState;
		if (std::abs(value) > MaxTransformParameter)
			return ForwardError::TransformParameterOutOfRange;
	}
	if (state.scale_x == 0.0 || state.scale_y == 0.0)
		return ForwardError::DegenerateScale;
	if (state.scale_x < 0.0 || state.scale_y < 0.0)
		return ForwardError::MirroredTransform;

	double const shear_determinant = 1.0 - state.shear_x * state.shear_y;
	double const shear_scale = 1.0 + std::abs(state.shear_x * state.shear_y);
	double const shear_epsilon =
		64.0 * std::numeric_limits<double>::epsilon() * shear_scale;
	if (!std::isfinite(shear_determinant) || std::abs(shear_determinant) <= shear_epsilon)
		return ForwardError::SingularTransform;
	if (shear_determinant < 0.0)
		return ForwardError::MirroredTransform;
	return ForwardError::None;
}

}

Vec2 ResolveBoundsAlignmentShift(BaseBounds const& bounds, int alignment) {
	double const width = bounds.alignment_extent
		? bounds.alignment_extent->width : bounds.rectangle.Width();
	double const height = bounds.alignment_extent
		? bounds.alignment_extent->height : bounds.rectangle.Height();
	double horizontal = 0.0;
	double vertical = 0.0;
	switch ((alignment - 1) % 3) {
		case 1: horizontal = -width / 2.0; break;
		case 2: horizontal = -width; break;
		default: break;
	}
	switch ((alignment - 1) / 3) {
		case 0: vertical = -height; break;
		case 1: vertical = -height / 2.0; break;
		default: break;
	}
	return {horizontal + bounds.alignment_offset.x,
		vertical + bounds.alignment_offset.y};
}

namespace {

Vec3 RotateZ(Vec3 value, double degrees) {
	double const angle = degrees * DegreesToRadians;
	double const cosine = std::cos(angle);
	double const sine = std::sin(angle);
	return {cosine * value.x - sine * value.y,
		sine * value.x + cosine * value.y, value.z};
}

Vec3 RotateX(Vec3 value, double degrees) {
	double const angle = degrees * DegreesToRadians;
	double const cosine = std::cos(angle);
	double const sine = std::sin(angle);
	return {value.x, cosine * value.y - sine * value.z,
		sine * value.y + cosine * value.z};
}

Vec3 RotateY(Vec3 value, double degrees) {
	double const angle = degrees * DegreesToRadians;
	double const cosine = std::cos(angle);
	double const sine = std::sin(angle);
	return {cosine * value.x - sine * value.z, value.y,
		sine * value.x + cosine * value.z};
}

std::optional<Vec2> TransformPoint(
	Vec2 point,
	BaseBounds const& bounds,
	EvaluatedTransformState const& state,
	Vec2 origin,
	double camera_distance,
	double& denominator) {
	Vec2 const shift = ResolveBoundsAlignmentShift(bounds, state.alignment);
	Vec3 value {
		point.x + point.y * state.shear_x + shift.x,
		point.x * state.shear_y + point.y + shift.y,
		0.0,
	};
	value.x = value.x * state.scale_x / 100.0 + state.position.x - origin.x;
	value.y = value.y * state.scale_y / 100.0 + state.position.y - origin.y;
	value = RotateZ(value, -state.rotation_z);
	value = RotateX(value, -state.rotation_x);
	value = RotateY(value, state.rotation_y);

	denominator = value.z + camera_distance;
	if (!std::isfinite(denominator) || !std::isfinite(value.x)
		|| !std::isfinite(value.y))
		return std::nullopt;
	double const projection = camera_distance / denominator;
	Vec2 result {
		origin.x + value.x * projection,
		origin.y + value.y * projection,
	};
	if (!IsFiniteAndSafe(result))
		return std::nullopt;
	return result;
}

}

char const* DescribeForwardError(ForwardError error) {
	switch (error) {
		case ForwardError::None: return "valid forward transform";
		case ForwardError::UnsupportedMixedBounds: return "mixed text and drawing runs do not have a stable base bounds";
		case ForwardError::UnsupportedDynamicBounds: return "dynamic geometry has no single base bounds";
		case ForwardError::UnsupportedFontBounds: return "font metrics are unavailable for base bounds";
		case ForwardError::InvalidBounds: return "base bounds are invalid";
		case ForwardError::InvalidResolution: return "PlayRes/LayoutRes/video resolution is invalid";
		case ForwardError::InvalidEventTime: return "event-relative evaluation time is invalid";
		case ForwardError::InvalidAlignment: return "alignment must be in the range 1..9";
		case ForwardError::NonFiniteState: return "evaluated ASS state is not finite";
		case ForwardError::TransformParameterOutOfRange: return "transform parameter exceeds the supported range";
		case ForwardError::DegenerateScale: return "scale must not be zero";
		case ForwardError::MirroredTransform: return "mirrored transforms are not supported for ordered quads";
		case ForwardError::SingularTransform: return "shear transform is singular";
		case ForwardError::ProjectionDomain: return "perspective denominator crosses or approaches zero";
		case ForwardError::InvalidQuad: return "forward transform produced an invalid quad";
		case ForwardError::SingularHomography: return "forward homography is singular";
	}
	return "unknown forward error";
}

ForwardResult ForwardQuad(ForwardInput const& input) {
	if (auto const bounds_error = BoundsError(input.bounds.kind);
		bounds_error != ForwardError::None)
		return {bounds_error};
	if (ValidateRect(input.bounds.rectangle) != GeometryError::None)
		return {ForwardError::InvalidBounds};
	if (input.bounds.alignment_extent && !IsUsable(*input.bounds.alignment_extent))
		return {ForwardError::InvalidBounds};
	if (!IsFiniteAndSafe(input.bounds.alignment_offset))
		return {ForwardError::InvalidBounds};
	if (auto const state_error = ValidateState(input.state);
		state_error != ForwardError::None)
		return {state_error};
	auto const resolution = ResolveLayoutResolution(input);
	if (resolution.error != ForwardError::None)
		return {resolution.error};

	Vec2 const origin = input.state.origin.value_or(input.state.position);
	Quad const source_quad = MakeQuad(input.bounds.rectangle);
	std::array<Vec2, 4> projected;
	std::array<double, 4> denominators {};
	for (std::size_t index = 0; index < source_quad.size(); ++index) {
		auto const transformed = TransformPoint(
			source_quad[index], input.bounds, input.state, origin,
				resolution.camera_distance, denominators[index]);
		if (!transformed)
			return {ForwardError::ProjectionDomain};
		projected[index] = *transformed;
	}

	double min_denominator = denominators[0];
	double max_denominator = denominators[0];
	double denominator_scale = std::abs(denominators[0]);
	for (double const denominator : denominators) {
		min_denominator = std::min(min_denominator, denominator);
		max_denominator = std::max(max_denominator, denominator);
		denominator_scale = std::max(denominator_scale, std::abs(denominator));
	}
	double const denominator_epsilon =
		128.0 * std::numeric_limits<double>::epsilon() * denominator_scale;
	if (denominator_scale == 0.0
		|| (min_denominator <= denominator_epsilon && max_denominator >= -denominator_epsilon))
		return {ForwardError::ProjectionDomain};

	Quad const quad = {projected[0], projected[1], projected[2], projected[3]};
	auto const validation = ValidateQuad(quad);
	if (!validation)
		return {ForwardError::InvalidQuad, validation.error, quad,
			Homography(), resolution.value, resolution.camera_distance};
	auto const homography = MakeHomography(input.bounds.rectangle, quad);
	if (!homography)
		return {homography.error == GeometryError::ProjectionDomain
				? ForwardError::ProjectionDomain
				: ForwardError::SingularHomography,
			homography.error, quad, Homography(), resolution.value, resolution.camera_distance};
	return {ForwardError::None, GeometryError::None, quad, homography.value,
		resolution.value, resolution.camera_distance};
}

}
