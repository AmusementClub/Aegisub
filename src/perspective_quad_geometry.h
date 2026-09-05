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

#pragma once

#include <array>
#include <cstddef>
#include <optional>

namespace perspective {

constexpr double MaxAbsCoordinate = 1.0e9;

struct Vec2 {
	double x = 0.0;
	double y = 0.0;

	constexpr Vec2 operator+(Vec2 other) const { return {x + other.x, y + other.y}; }
	constexpr Vec2 operator-(Vec2 other) const { return {x - other.x, y - other.y}; }
	constexpr Vec2 operator*(double factor) const { return {x * factor, y * factor}; }
	constexpr Vec2 operator/(double divisor) const { return {x / divisor, y / divisor}; }
	constexpr double Dot(Vec2 other) const { return x * other.x + y * other.y; }
	constexpr double Cross(Vec2 other) const { return x * other.y - y * other.x; }
	constexpr double SquareLength() const { return Dot(*this); }
};

struct Rect {
	double left = 0.0;
	double top = 0.0;
	double right = 0.0;
	double bottom = 0.0;

	constexpr double Width() const { return right - left; }
	constexpr double Height() const { return bottom - top; }
};

using Quad = std::array<Vec2, 4>;

enum class GeometryError {
	None,
	NonFinite,
	CoordinateOutOfRange,
	DuplicatePoint,
	EdgeTooShort,
	AreaTooSmall,
	SelfIntersecting,
	WrongWinding,
	NonConvex,
	InvalidDomain,
	SingularHomography,
	ProjectionDomain,
};

struct GeometryValidation {
	GeometryError error = GeometryError::None;
	double linear_epsilon = 0.0;
	double area_epsilon = 0.0;

	explicit operator bool() const { return error == GeometryError::None; }
};

class Matrix3 {
	std::array<double, 9> values_;

public:
	Matrix3();
	explicit Matrix3(std::array<double, 9> values);

	static Matrix3 Identity();

	double operator()(std::size_t row, std::size_t column) const;
	std::array<double, 9> const& Values() const { return values_; }
	bool IsFinite() const;
	double Determinant() const;
	std::optional<Matrix3> Inverse() const;
	Matrix3 operator*(Matrix3 const& right) const;
};

class Homography {
	Matrix3 matrix_;

public:
	Homography();
	explicit Homography(Matrix3 matrix);

	Matrix3 const& Matrix() const { return matrix_; }
	double Denominator(Vec2 point) const;
	std::optional<Vec2> Map(Vec2 point) const;
	std::optional<Homography> Inverse() const;
};

struct HomographyResult {
	GeometryError error = GeometryError::None;
	Homography value;

	explicit operator bool() const { return error == GeometryError::None; }
};

[[nodiscard]] char const* DescribeGeometryError(GeometryError error);
[[nodiscard]] double SignedArea(Quad const& quad);
[[nodiscard]] Quad MakeQuad(Rect const& rect);
[[nodiscard]] GeometryError ValidateRect(Rect const& rect);
[[nodiscard]] GeometryValidation ValidateQuad(Quad const& quad);
[[nodiscard]] GeometryError ValidateProjectionDomain(Homography const& homography, Rect const& domain);
[[nodiscard]] HomographyResult MakeHomography(Rect const& source, Quad const& target);
[[nodiscard]] std::optional<Vec2> QuadCenter(Quad const& quad);

// Parameter interval of a segment inside the viewport, for bounded overlay
// generation. An empty intersection has no interval.
[[nodiscard]] std::optional<std::array<double, 2>> ClipSegmentRange(
	Vec2 first, Vec2 last, Rect viewport);
}
