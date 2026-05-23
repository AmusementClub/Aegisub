#pragma once

#include <cstddef>
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
