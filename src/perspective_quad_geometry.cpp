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

// Quad/projective portions are adapted from arch1t3cht/Aegisub commit
// f44883844f3179fb2f52b937854b1e6f2e014bee. Validation and matrix
// infrastructure are repository-local.

#include "perspective_quad_geometry.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace perspective {
namespace {

constexpr double RelativeLinearEpsilon = 1.0e-9;
constexpr double RelativeAreaEpsilon = 1.0e-12;

bool IsFinite(Vec2 point) {
	return std::isfinite(point.x) && std::isfinite(point.y);
}

bool IsInSafeRange(Vec2 point) {
	return std::abs(point.x) <= MaxAbsCoordinate && std::abs(point.y) <= MaxAbsCoordinate;
}

double MaxAbs(Vec2 point) {
	return std::max(std::abs(point.x), std::abs(point.y));
}

struct EpsilonScale {
	double linear = 0.0;
	double area = 0.0;
};

EpsilonScale EpsilonFor(Quad const& quad) {
	double min_x = quad[0].x;
	double max_x = quad[0].x;
	double min_y = quad[0].y;
	double max_y = quad[0].y;
	double max_abs = MaxAbs(quad[0]);
	for (auto const point : quad) {
		min_x = std::min(min_x, point.x);
		max_x = std::max(max_x, point.x);
		min_y = std::min(min_y, point.y);
		max_y = std::max(max_y, point.y);
		max_abs = std::max(max_abs, MaxAbs(point));
	}

	double const span = std::max({max_x - min_x, max_y - min_y, 1.0});
	double const rounding_floor =
		64.0 * std::numeric_limits<double>::epsilon() * std::max(max_abs, 1.0);
	double const linear = std::max(RelativeLinearEpsilon * span, rounding_floor);
	return {linear, std::max(RelativeAreaEpsilon * span * span, linear * span)};
}

double Orientation(Vec2 first, Vec2 second, Vec2 point) {
	return (second - first).Cross(point - first);
}

bool IsOnSegment(Vec2 first, Vec2 second, Vec2 point, double epsilon) {
	return point.x >= std::min(first.x, second.x) - epsilon
		&& point.x <= std::max(first.x, second.x) + epsilon
		&& point.y >= std::min(first.y, second.y) - epsilon
		&& point.y <= std::max(first.y, second.y) + epsilon;
}

bool SegmentsIntersect(Vec2 a, Vec2 b, Vec2 c, Vec2 d, double linear_epsilon, double area_epsilon) {
	double const ab_c = Orientation(a, b, c);
	double const ab_d = Orientation(a, b, d);
	double const cd_a = Orientation(c, d, a);
	double const cd_b = Orientation(c, d, b);

	if (((ab_c > area_epsilon && ab_d < -area_epsilon)
			|| (ab_c < -area_epsilon && ab_d > area_epsilon))
		&& ((cd_a > area_epsilon && cd_b < -area_epsilon)
			|| (cd_a < -area_epsilon && cd_b > area_epsilon)))
		return true;

	return (std::abs(ab_c) <= area_epsilon && IsOnSegment(a, b, c, linear_epsilon))
		|| (std::abs(ab_d) <= area_epsilon && IsOnSegment(a, b, d, linear_epsilon))
		|| (std::abs(cd_a) <= area_epsilon && IsOnSegment(c, d, a, linear_epsilon))
		|| (std::abs(cd_b) <= area_epsilon && IsOnSegment(c, d, b, linear_epsilon));
}

GeometryError ValidateRectImpl(Rect const& rect) {
	Quad const corners = MakeQuad(rect);
	for (auto const point : corners) {
		if (!IsFinite(point))
			return GeometryError::NonFinite;
		if (!IsInSafeRange(point))
			return GeometryError::CoordinateOutOfRange;
	}

	auto const epsilon = EpsilonFor(corners);
	if (rect.Width() <= epsilon.linear || rect.Height() <= epsilon.linear)
		return GeometryError::InvalidDomain;
	return GeometryError::None;
}

std::optional<Matrix3> SquareToQuad(Quad const& quad, EpsilonScale epsilon) {
	auto const& p0 = quad[0];
	auto const& p1 = quad[1];
	auto const& p2 = quad[2];
	auto const& p3 = quad[3];
	double const dx1 = p1.x - p2.x;
	double const dx2 = p3.x - p2.x;
	double const dx3 = p0.x - p1.x + p2.x - p3.x;
	double const dy1 = p1.y - p2.y;
	double const dy2 = p3.y - p2.y;
	double const dy3 = p0.y - p1.y + p2.y - p3.y;

	if (std::abs(dx3) <= epsilon.linear && std::abs(dy3) <= epsilon.linear) {
		return Matrix3({
			p1.x - p0.x, p3.x - p0.x, p0.x,
			p1.y - p0.y, p3.y - p0.y, p0.y,
			0.0, 0.0, 1.0,
		});
	}

	double const determinant = dx1 * dy2 - dx2 * dy1;
	if (!std::isfinite(determinant) || std::abs(determinant) <= epsilon.area)
		return std::nullopt;
	double const g = (dx3 * dy2 - dx2 * dy3) / determinant;
	double const h = (dx1 * dy3 - dx3 * dy1) / determinant;
	Matrix3 result({
		p1.x - p0.x + g * p1.x, p3.x - p0.x + h * p3.x, p0.x,
		p1.y - p0.y + g * p1.y, p3.y - p0.y + h * p3.y, p0.y,
		g, h, 1.0,
	});
	if (!result.IsFinite())
		return std::nullopt;
	return result;
}

}

Matrix3::Matrix3()
: values_({1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0}) {
}

Matrix3::Matrix3(std::array<double, 9> values)
: values_(values) {
}

Matrix3 Matrix3::Identity() {
	return Matrix3();
}

double Matrix3::operator()(std::size_t row, std::size_t column) const {
	return values_[row * 3 + column];
}

bool Matrix3::IsFinite() const {
	return std::all_of(values_.begin(), values_.end(), [](double value) {
		return std::isfinite(value);
	});
}

double Matrix3::Determinant() const {
	return values_[0] * (values_[4] * values_[8] - values_[5] * values_[7])
		- values_[1] * (values_[3] * values_[8] - values_[5] * values_[6])
		+ values_[2] * (values_[3] * values_[7] - values_[4] * values_[6]);
}

std::optional<Matrix3> Matrix3::Inverse() const {
	if (!IsFinite())
		return std::nullopt;

	double const determinant = Determinant();
	double const determinant_scale =
		std::abs(values_[0] * (values_[4] * values_[8] - values_[5] * values_[7]))
		+ std::abs(values_[1] * (values_[3] * values_[8] - values_[5] * values_[6]))
		+ std::abs(values_[2] * (values_[3] * values_[7] - values_[4] * values_[6]));
	double const epsilon =
		64.0 * std::numeric_limits<double>::epsilon() * determinant_scale;
	if (!std::isfinite(determinant) || determinant_scale == 0.0
		|| std::abs(determinant) <= epsilon)
		return std::nullopt;

	std::array<double, 9> inverse {
		values_[4] * values_[8] - values_[5] * values_[7],
		values_[2] * values_[7] - values_[1] * values_[8],
		values_[1] * values_[5] - values_[2] * values_[4],
		values_[5] * values_[6] - values_[3] * values_[8],
		values_[0] * values_[8] - values_[2] * values_[6],
		values_[2] * values_[3] - values_[0] * values_[5],
		values_[3] * values_[7] - values_[4] * values_[6],
		values_[1] * values_[6] - values_[0] * values_[7],
		values_[0] * values_[4] - values_[1] * values_[3],
	};
	for (double& value : inverse)
		value /= determinant;
	Matrix3 result(inverse);
	if (!result.IsFinite())
		return std::nullopt;
	return result;
}

Matrix3 Matrix3::operator*(Matrix3 const& right) const {
	std::array<double, 9> result {};
	for (std::size_t row = 0; row < 3; ++row) {
		for (std::size_t column = 0; column < 3; ++column) {
			for (std::size_t inner = 0; inner < 3; ++inner)
				result[row * 3 + column] += (*this)(row, inner) * right(inner, column);
		}
	}
	return Matrix3(result);
}

Homography::Homography() = default;

Homography::Homography(Matrix3 matrix)
: matrix_(std::move(matrix)) {
}

double Homography::Denominator(Vec2 point) const {
	return matrix_(2, 0) * point.x + matrix_(2, 1) * point.y + matrix_(2, 2);
}

std::optional<Vec2> Homography::Map(Vec2 point) const {
	if (!IsFinite(point) || !matrix_.IsFinite())
		return std::nullopt;
	double const denominator = Denominator(point);
	double const denominator_scale = std::abs(matrix_(2, 0) * point.x)
		+ std::abs(matrix_(2, 1) * point.y) + std::abs(matrix_(2, 2));
	double const epsilon =
		64.0 * std::numeric_limits<double>::epsilon() * denominator_scale;
	if (!std::isfinite(denominator) || denominator_scale == 0.0
		|| std::abs(denominator) <= epsilon)
		return std::nullopt;

	Vec2 result {
		(matrix_(0, 0) * point.x + matrix_(0, 1) * point.y + matrix_(0, 2)) / denominator,
		(matrix_(1, 0) * point.x + matrix_(1, 1) * point.y + matrix_(1, 2)) / denominator,
	};
	if (!IsFinite(result) || !IsInSafeRange(result))
		return std::nullopt;
	return result;
}

std::optional<Homography> Homography::Inverse() const {
	auto inverse = matrix_.Inverse();
	if (!inverse)
		return std::nullopt;
	return Homography(*inverse);
}

char const* DescribeGeometryError(GeometryError error) {
	switch (error) {
		case GeometryError::None: return "valid geometry";
		case GeometryError::NonFinite: return "coordinate is not finite";
		case GeometryError::CoordinateOutOfRange: return "coordinate exceeds the supported range";
		case GeometryError::DuplicatePoint: return "quad contains duplicate points";
		case GeometryError::EdgeTooShort: return "quad edge is too short";
		case GeometryError::AreaTooSmall: return "quad area is too small";
		case GeometryError::SelfIntersecting: return "quad is self-intersecting";
		case GeometryError::WrongWinding: return "quad winding must be TL, TR, BR, BL";
		case GeometryError::NonConvex: return "quad must be strictly convex";
		case GeometryError::InvalidDomain: return "geometry domain is invalid";
		case GeometryError::SingularHomography: return "homography is singular";
		case GeometryError::ProjectionDomain: return "projection denominator crosses or approaches zero";
	}
	return "unknown geometry error";
}

double SignedArea(Quad const& quad) {
	// Sum the two triangle cross products about the first point instead of the
	// absolute-coordinate shoelace. Both are algebraically identical, but the
	// shoelace builds ~1e16-magnitude products for a quad sitting near the
	// coordinate limit and the area cancels away inside them; translating
	// first keeps the arithmetic at the quad's own scale.
	Vec2 const origin = quad[0];
	Vec2 const first = quad[1] - origin;
	Vec2 const second = quad[2] - origin;
	Vec2 const third = quad[3] - origin;
	return (first.Cross(second) + second.Cross(third)) * 0.5;
}

Quad MakeQuad(Rect const& rect) {
	return {{{rect.left, rect.top}, {rect.right, rect.top},
		{rect.right, rect.bottom}, {rect.left, rect.bottom}}};
}

GeometryError ValidateRect(Rect const& rect) {
	return ValidateRectImpl(rect);
}

GeometryValidation ValidateQuad(Quad const& quad) {
	for (auto const point : quad) {
		if (!IsFinite(point))
			return {GeometryError::NonFinite};
		if (!IsInSafeRange(point))
			return {GeometryError::CoordinateOutOfRange};
	}

	auto const epsilon = EpsilonFor(quad);
	for (std::size_t first = 0; first < quad.size(); ++first) {
		for (std::size_t second = first + 1; second < quad.size(); ++second) {
			if ((quad[first] - quad[second]).SquareLength() == 0.0)
				return {GeometryError::DuplicatePoint, epsilon.linear, epsilon.area};
		}
	}
	for (std::size_t index = 0; index < quad.size(); ++index) {
		if ((quad[(index + 1) % quad.size()] - quad[index]).SquareLength()
			<= epsilon.linear * epsilon.linear)
			return {GeometryError::EdgeTooShort, epsilon.linear, epsilon.area};
	}
	if ((quad[0] - quad[2]).SquareLength() <= epsilon.linear * epsilon.linear
		|| (quad[1] - quad[3]).SquareLength() <= epsilon.linear * epsilon.linear)
		return {GeometryError::DuplicatePoint, epsilon.linear, epsilon.area};

	if (SegmentsIntersect(quad[0], quad[1], quad[2], quad[3], epsilon.linear, epsilon.area)
		|| SegmentsIntersect(quad[1], quad[2], quad[3], quad[0], epsilon.linear, epsilon.area))
		return {GeometryError::SelfIntersecting, epsilon.linear, epsilon.area};

	double const area = SignedArea(quad);
	if (area < -epsilon.area)
		return {GeometryError::WrongWinding, epsilon.linear, epsilon.area};
	if (area <= epsilon.area)
		return {GeometryError::AreaTooSmall, epsilon.linear, epsilon.area};

	for (std::size_t index = 0; index < quad.size(); ++index) {
		auto const edge = quad[(index + 1) % quad.size()] - quad[index];
		auto const next_edge = quad[(index + 2) % quad.size()] - quad[(index + 1) % quad.size()];
		if (edge.Cross(next_edge) <= epsilon.area)
			return {GeometryError::NonConvex, epsilon.linear, epsilon.area};
	}
	return {GeometryError::None, epsilon.linear, epsilon.area};
}

GeometryError ValidateProjectionDomain(Homography const& homography, Rect const& domain) {
	auto const domain_error = ValidateRect(domain);
	if (domain_error != GeometryError::None)
		return domain_error;
	if (!homography.Matrix().IsFinite())
		return GeometryError::NonFinite;

	double min_denominator = std::numeric_limits<double>::infinity();
	double max_denominator = -std::numeric_limits<double>::infinity();
	double denominator_scale = 0.0;
	for (auto const point : MakeQuad(domain)) {
		double const denominator = homography.Denominator(point);
		if (!std::isfinite(denominator))
			return GeometryError::NonFinite;
		min_denominator = std::min(min_denominator, denominator);
		max_denominator = std::max(max_denominator, denominator);
		denominator_scale = std::max(denominator_scale, std::abs(denominator));
	}
	if (denominator_scale == 0.0)
		return GeometryError::ProjectionDomain;
	// A linear denominator reaches its extrema at rectangle corners. Equal,
	// non-zero corner signs therefore prove it cannot cross zero inside.
	double const epsilon = 128.0 * std::numeric_limits<double>::epsilon() * denominator_scale;
	if (min_denominator <= epsilon && max_denominator >= -epsilon)
		return GeometryError::ProjectionDomain;
	return GeometryError::None;
}

HomographyResult MakeHomography(Rect const& source, Quad const& target) {
	auto const source_error = ValidateRect(source);
	if (source_error != GeometryError::None)
		return {source_error};
	auto const target_validation = ValidateQuad(target);
	if (!target_validation)
		return {target_validation.error};

	auto square_to_quad = SquareToQuad(
		target, {target_validation.linear_epsilon, target_validation.area_epsilon});
	if (!square_to_quad)
		return {GeometryError::SingularHomography};
	Matrix3 const normalize_source({
		1.0 / source.Width(), 0.0, -source.left / source.Width(),
		0.0, 1.0 / source.Height(), -source.top / source.Height(),
		0.0, 0.0, 1.0,
	});
	Homography result(*square_to_quad * normalize_source);
	if (!result.Matrix().Inverse())
		return {GeometryError::SingularHomography};
	auto const projection_error = ValidateProjectionDomain(result, source);
	if (projection_error != GeometryError::None)
		return {projection_error};
	auto const source_corners = MakeQuad(source);
	double const residual_limit = target_validation.linear_epsilon * 64.0;
	for (std::size_t index = 0; index < target.size(); ++index) {
		auto const mapped = result.Map(source_corners[index]);
		if (!mapped || (*mapped - target[index]).SquareLength() > residual_limit * residual_limit)
			return {GeometryError::SingularHomography};
	}
	return {GeometryError::None, result};
}

std::optional<Vec2> QuadCenter(Quad const& quad) {
	if (!ValidateQuad(quad))
		return std::nullopt;
	Vec2 const first_diagonal = quad[2] - quad[0];
	Vec2 const second_diagonal = quad[3] - quad[1];
	double const denominator = first_diagonal.Cross(second_diagonal);
	auto const epsilon = EpsilonFor(quad);
	if (std::abs(denominator) <= epsilon.area)
		return std::nullopt;
	double const factor = (quad[1] - quad[0]).Cross(second_diagonal) / denominator;
	Vec2 const center = quad[0] + first_diagonal * factor;
	if (!IsFinite(center))
		return std::nullopt;
	return center;
}

}
