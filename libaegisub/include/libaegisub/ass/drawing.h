#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace agi {
namespace ass {
namespace drawing {

constexpr double kPointEpsilon = 1e-9;

struct Point {
	double x = 0.0;
	double y = 0.0;
};

bool AlmostEqual(double lhs, double rhs, double epsilon = kPointEpsilon);
bool SamePoint(Point const& lhs, Point const& rhs, double epsilon = kPointEpsilon);

struct Rect {
	double x = 0.0;
	double y = 0.0;
	double width = 0.0;
	double height = 0.0;
};

struct Matrix3x2 {
	double m11 = 1.0;
	double m12 = 0.0;
	double m21 = 0.0;
	double m22 = 1.0;
	double dx = 0.0;
	double dy = 0.0;
};

enum class PathVerb {
	MoveTo,
	LineTo,
	QuadTo,
	ConicTo,
	CubicTo,
	Close,
};

struct PathCommand {
	PathVerb verb = PathVerb::MoveTo;
	Point p1 {};
	Point p2 {};
	Point p3 {};
	double weight = 1.0;
};

struct PathData {
	std::vector<PathCommand> commands;
	// ASS drawing output has only nonzero winding fill. Even-odd paths may be
	// used as backend inputs, but must be converted before filled serialization.
	bool winding_fill = true;

	bool empty() const { return commands.empty(); }
};

enum class AssDrawingCompatMode {
	VsFilter = 0,
	Libass = 1,
};

enum class AssDrawingPathMode {
	FilledContours = 0,
	PreserveOpenContours = 1,
};

enum class DrawingBooleanOp {
	Union,
	Intersect,
	Subtract,
	Xor,
};

enum class DrawingStrokeCap {
	Flat,
	Round,
	Square,
};

enum class DrawingStrokeJoin {
	Miter,
	Bevel,
	Round,
	SvgMiter,
};

constexpr AssDrawingCompatMode kDefaultAssDrawingCompatMode = AssDrawingCompatMode::VsFilter;

AssDrawingCompatMode SanitizeAssDrawingCompatMode(int raw_mode);

PathData ParseAss(std::string_view ass_shape,
	AssDrawingCompatMode compat_mode = kDefaultAssDrawingCompatMode,
	AssDrawingPathMode path_mode = AssDrawingPathMode::FilledContours);

inline PathData ParseAssOpen(std::string_view ass_shape,
	AssDrawingCompatMode compat_mode = kDefaultAssDrawingCompatMode) {
	return ParseAss(ass_shape, compat_mode, AssDrawingPathMode::PreserveOpenContours);
}

Point TransformPoint(Point point, Matrix3x2 const& matrix);
PathData TransformPath(PathData path, Matrix3x2 const& matrix);

PathData LowerForAss(PathData const& path, bool implicit_close_contours = false);
std::string SerializeAss(PathData const& path);
// Requires winding_fill=true; ASS has no even-odd fill-mode command.
std::string SerializeAssFilled(PathData const& path);
// Same winding-fill precondition as SerializeAssFilled.
std::string SerializeAssCompactFilled(PathData const& path);
// Checked filled serializers reject even-odd input and clear output on failure.
bool TrySerializeAssFilled(PathData const& path, std::string& output);
bool TrySerializeAssCompactFilled(PathData const& path, std::string& output);
std::string CompactAss(std::string_view ass_shape,
	AssDrawingCompatMode compat_mode = kDefaultAssDrawingCompatMode);
// Tight geometric bounds of the raw path (curve extrema included).
bool TryGetBounds(PathData const& path, Rect& bounds);
// Control-point rectangle used by the legacy shape_bouding* APIs.
// Includes only stored command points, not curve extrema.
bool TryGetControlPointBounds(PathData const& path, Rect& bounds);
PathData FlattenPath(PathData const& path, double tolerance = 0.25);
PathData ReversePath(PathData const& path);

class PathMeasure {
public:
	struct Impl;

	explicit PathMeasure(PathData const& path);
	double Length() const;
	double PercentAtLength(double distance) const;
	double LegacyPercentAtLength(double distance) const;
	bool TryGetPositionAtPercent(double percent, Point& point, Point& tangent) const;
	bool TryGetLegacyPositionAtPercent(double percent, Point& point, Point& tangent) const;
	bool TryGetPositionAtLength(double distance, Point& point, Point& tangent) const;

private:
	std::shared_ptr<Impl const> impl_;
};

double PathLength(PathData const& path);
// Modern normalized arc-length mapping.
double PercentAtLength(PathData const& path, double distance);
bool TryGetPositionAtPercent(PathData const& path, double percent, Point& point, Point& tangent);
// Legacy QPainterPath-compatible percent mapping. Segments are allocated by
// measured length, while the allocation inside a curve is its Bezier t.
double LegacyPercentAtLength(PathData const& path, double distance);
bool TryGetLegacyPositionAtPercent(PathData const& path, double percent, Point& point, Point& tangent);
bool TryGetPositionAtLength(PathData const& path, double distance, Point& point, Point& tangent);
bool TryGetSignedAreaAndCentroid(PathData const& path, double& signed_area, Point& centroid, double tolerance = 0.25);
PathData MakeRect(double x, double y, double width, double height);
PathData MakeEllipse(double x, double y, double width, double height);
PathData MakeRoundedRect(double x, double y, double width, double height, double radius_x, double radius_y);
void AppendArcMoveTo(PathData& path, double x, double y, double width, double height, double angle);
void AppendArcTo(PathData& path, double x, double y, double width, double height, double start_angle, double sweep_length);
bool DrawingBackendAvailable();
// Compatibility/introspection name for the current backend implementation.
bool DrawingSkiaBackendAvailable();
bool TryDrawingContainsPoint(PathData const& path, double x, double y, bool& contains);
bool TryDrawingContainsRect(PathData const& path, double x, double y, double width, double height, bool& contains);
bool TryDrawingBoolean(PathData const& lhs, PathData const& rhs, DrawingBooleanOp op, PathData& result);
bool TryDrawingOutline(PathData const& path, double width, DrawingStrokeCap cap, DrawingStrokeJoin join, PathData& result);
bool TryDrawingPatternOutline(PathData const& path,
	double width,
	DrawingStrokeCap cap,
	DrawingStrokeJoin join,
	double pattern_length,
	double space_length,
	double dash_offset,
	PathData& result);

enum class LexemeType {
	Normal,
	Command,
	X,
	Y,
	EndpointX,
	EndpointY,
	Error,
};

struct Lexeme {
	LexemeType type = LexemeType::Normal;
	std::size_t length = 0;
};

std::vector<Lexeme> LexDrawing(std::string_view text);

} // namespace drawing
} // namespace ass
} // namespace agi
