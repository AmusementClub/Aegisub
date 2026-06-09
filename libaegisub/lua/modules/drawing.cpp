#include "libaegisub/ass/drawing.h"
#include "libaegisub/lua/utils.h"

#include <lua.hpp>

#include <cctype>
#include <cmath>
#include <limits>
#include <string>

namespace {

using agi::ass::drawing::AssDrawingCompatMode;
using agi::ass::drawing::DrawingBooleanOp;
using agi::ass::drawing::DrawingStrokeCap;
using agi::ass::drawing::DrawingStrokeJoin;
using agi::ass::drawing::Matrix3x2;
using agi::ass::drawing::PathData;
using agi::ass::drawing::Point;
using agi::ass::drawing::Rect;

AssDrawingCompatMode CheckCompatMode(lua_State *L, int idx) {
	if (lua_gettop(L) < idx || lua_isnil(L, idx))
		return agi::ass::drawing::kDefaultAssDrawingCompatMode;

	if (lua_isnumber(L, idx))
		return agi::ass::drawing::SanitizeAssDrawingCompatMode(static_cast<int>(lua_tointeger(L, idx)));

	auto mode = agi::lua::check_string(L, idx);
	for (auto& c : mode)
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

	if (mode == "vsfilter" || mode == "xy-vsfilter" || mode == "xy")
		return AssDrawingCompatMode::VsFilter;
	if (mode == "libass")
		return AssDrawingCompatMode::Libass;

	agi::lua::error(L, "unknown drawing compatibility mode: %s", mode.c_str());
	return agi::ass::drawing::kDefaultAssDrawingCompatMode;
}

std::string CheckLowerString(lua_State *L, int idx) {
	auto value = agi::lua::check_string(L, idx);
	for (auto& c : value)
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return value;
}

DrawingStrokeCap CheckCapStyle(lua_State *L, int idx) {
	auto value = CheckLowerString(L, idx);
	if (value == "flat" || value == "butt")
		return DrawingStrokeCap::Flat;
	if (value == "round")
		return DrawingStrokeCap::Round;
	return DrawingStrokeCap::Square;
}

DrawingStrokeJoin CheckJoinStyle(lua_State *L, int idx) {
	auto value = CheckLowerString(L, idx);
	if (value == "miter")
		return DrawingStrokeJoin::Miter;
	if (value == "round")
		return DrawingStrokeJoin::Round;
	if (value == "svgmiter")
		return DrawingStrokeJoin::SvgMiter;
	return DrawingStrokeJoin::Bevel;
}

std::string CheckShape(lua_State *L, int idx) {
	return agi::lua::check_string(L, idx);
}

void PushShape(lua_State *L, std::string const& value) {
	agi::lua::push_value(L, value);
}

int PushRect(lua_State *L, Rect const& bounds, bool as_coords = false) {
	lua_pushnumber(L, bounds.x);
	lua_pushnumber(L, bounds.y);
	lua_pushnumber(L, as_coords ? bounds.x + bounds.width : bounds.width);
	lua_pushnumber(L, as_coords ? bounds.y + bounds.height : bounds.height);
	return 4;
}

int PushZeroRect(lua_State *L) {
	return PushRect(L, {});
}

int PushBoundsOrZero(lua_State *L, PathData const& path, bool as_coords = false) {
	Rect bounds;
	if (!agi::ass::drawing::TryGetBounds(path, bounds))
		return PushZeroRect(L);

	return PushRect(L, bounds, as_coords);
}

int BackendOperationError(lua_State *L, char const *name) {
	if (agi::ass::drawing::DrawingSkiaBackendAvailable())
		return agi::lua::error(L, "%s failed in the drawing Skia backend", name);

	return agi::lua::error(L, "%s requires the drawing Skia backend", name);
}

PathData ParseOpen(lua_State *L, int shape_idx, int mode_idx) {
	return agi::ass::drawing::ParseAssOpen(CheckShape(L, shape_idx), CheckCompatMode(L, mode_idx));
}

PathData ParseFilled(lua_State *L, int shape_idx, int mode_idx) {
	return agi::ass::drawing::ParseAss(CheckShape(L, shape_idx), CheckCompatMode(L, mode_idx));
}

struct ToleranceAndMode {
	double tolerance = 0.25;
	int mode_idx = 3;
};

ToleranceAndMode CheckToleranceAndMode(lua_State *L, int idx) {
	ToleranceAndMode result;
	result.mode_idx = idx + 1;
	if (lua_gettop(L) < idx || lua_isnil(L, idx))
		return result;

	if (lua_isnumber(L, idx))
		result.tolerance = luaL_checknumber(L, idx);
	else
		result.mode_idx = idx;
	return result;
}

int PushPointOrNil(lua_State *L, bool available, Point const& point) {
	if (!available) {
		lua_pushnil(L);
		return 1;
	}

	lua_pushnumber(L, point.x);
	lua_pushnumber(L, point.y);
	return 2;
}

double AngleFromTangent(Point const& tangent) {
	if (std::hypot(tangent.x, tangent.y) <= agi::ass::drawing::kPointEpsilon)
		return 0.0;

	constexpr double radians_to_degrees = 180.0 / 3.14159265358979323846;
	double degrees = std::atan2(-tangent.y, tangent.x) * radians_to_degrees;
	return std::fmod(degrees + 360.0, 360.0);
}

double SlopeFromTangent(Point const& tangent) {
	if (std::abs(tangent.x) <= agi::ass::drawing::kPointEpsilon) {
		if (std::abs(tangent.y) <= agi::ass::drawing::kPointEpsilon)
			return 0.0;
		return tangent.y >= 0.0
			? std::numeric_limits<double>::infinity()
			: -std::numeric_limits<double>::infinity();
	}

	return tangent.y / tangent.x;
}

int PushTransformedOpen(lua_State *L, Matrix3x2 const& matrix, int mode_idx) {
	PushShape(L, agi::ass::drawing::SerializeAss(agi::ass::drawing::TransformPath(ParseOpen(L, 1, mode_idx), matrix)));
	return 1;
}

bool TryGetPercentPosition(lua_State *L, Point& point, Point& tangent) {
	auto path = ParseOpen(L, 1, 3);
	double percent = luaL_checknumber(L, 2);
	return agi::ass::drawing::TryGetPositionAtPercent(path, percent, point, tangent);
}

int Normalize(lua_State *L) {
	auto shape = CheckShape(L, 1);
	auto mode = CheckCompatMode(L, 2);
	PushShape(L, agi::ass::drawing::SerializeAssFilled(agi::ass::drawing::ParseAss(shape, mode)));
	return 1;
}

int NormalizeOpen(lua_State *L) {
	auto shape = CheckShape(L, 1);
	auto mode = CheckCompatMode(L, 2);
	PushShape(L, agi::ass::drawing::SerializeAss(agi::ass::drawing::ParseAssOpen(shape, mode)));
	return 1;
}

int Compact(lua_State *L) {
	auto shape = CheckShape(L, 1);
	auto mode = CheckCompatMode(L, 2);
	PushShape(L, agi::ass::drawing::CompactAss(shape, mode));
	return 1;
}

int ShapeRect(lua_State *L) {
	PushShape(L, agi::ass::drawing::SerializeAssFilled(agi::ass::drawing::MakeRect(
		luaL_checknumber(L, 1),
		luaL_checknumber(L, 2),
		luaL_checknumber(L, 3),
		luaL_checknumber(L, 4))));
	return 1;
}

int ShapeEllipse(lua_State *L) {
	PushShape(L, agi::ass::drawing::SerializeAssFilled(agi::ass::drawing::MakeEllipse(
		luaL_checknumber(L, 1),
		luaL_checknumber(L, 2),
		luaL_checknumber(L, 3),
		luaL_checknumber(L, 4))));
	return 1;
}

int ShapeRoundedRect(lua_State *L) {
	PushShape(L, agi::ass::drawing::SerializeAssFilled(agi::ass::drawing::MakeRoundedRect(
		luaL_checknumber(L, 1),
		luaL_checknumber(L, 2),
		luaL_checknumber(L, 3),
		luaL_checknumber(L, 4),
		luaL_checknumber(L, 5),
		luaL_checknumber(L, 6))));
	return 1;
}

int ShapeArcMoveTo(lua_State *L) {
	auto path = ParseOpen(L, 1, 7);
	agi::ass::drawing::AppendArcMoveTo(path,
		luaL_checknumber(L, 2),
		luaL_checknumber(L, 3),
		luaL_checknumber(L, 4),
		luaL_checknumber(L, 5),
		luaL_checknumber(L, 6));
	PushShape(L, agi::ass::drawing::SerializeAss(path));
	return 1;
}

int ShapeArcTo(lua_State *L) {
	auto path = ParseOpen(L, 1, 8);
	agi::ass::drawing::AppendArcTo(path,
		luaL_checknumber(L, 2),
		luaL_checknumber(L, 3),
		luaL_checknumber(L, 4),
		luaL_checknumber(L, 5),
		luaL_checknumber(L, 6),
		luaL_checknumber(L, 7));
	PushShape(L, agi::ass::drawing::SerializeAss(path));
	return 1;
}

int Transform(lua_State *L) {
	Matrix3x2 matrix {
		luaL_checknumber(L, 2),
		luaL_checknumber(L, 3),
		luaL_checknumber(L, 4),
		luaL_checknumber(L, 5),
		luaL_checknumber(L, 6),
		luaL_checknumber(L, 7),
	};
	return PushTransformedOpen(L, matrix, 8);
}

int Translate(lua_State *L) {
	double dx = luaL_checknumber(L, 2);
	double dy = luaL_checknumber(L, 3);
	return PushTransformedOpen(L, {1.0, 0.0, 0.0, 1.0, dx, dy}, 4);
}

int Scale(lua_State *L) {
	double sx = luaL_checknumber(L, 2);
	double sy = luaL_checknumber(L, 3);
	return PushTransformedOpen(L, {sx, 0.0, 0.0, sy, 0.0, 0.0}, 4);
}

int Rotate(lua_State *L) {
	double radians = luaL_checknumber(L, 2) * 3.14159265358979323846 / 180.0;
	double c = std::cos(radians);
	double s = std::sin(radians);
	return PushTransformedOpen(L, {c, s, -s, c, 0.0, 0.0}, 3);
}

int Shear(lua_State *L) {
	double horizontal = luaL_checknumber(L, 2);
	double vertical = luaL_checknumber(L, 3);
	return PushTransformedOpen(L, {1.0, vertical, horizontal, 1.0, 0.0, 0.0}, 4);
}

int Bounds(lua_State *L) {
	Rect bounds;
	if (!agi::ass::drawing::TryGetBounds(ParseOpen(L, 1, 2), bounds)) {
		lua_pushnil(L);
		return 1;
	}

	return PushRect(L, bounds);
}

int ShapeBouding(lua_State *L) {
	return PushBoundsOrZero(L, ParseFilled(L, 1, 2));
}

int ShapeBoudingCoords(lua_State *L) {
	return PushBoundsOrZero(L, ParseFilled(L, 1, 2), true);
}

int Flatten(lua_State *L) {
	auto options = CheckToleranceAndMode(L, 2);
	PushShape(L, agi::ass::drawing::SerializeAss(agi::ass::drawing::FlattenPath(ParseOpen(L, 1, options.mode_idx), options.tolerance)));
	return 1;
}

int Reverse(lua_State *L) {
	PushShape(L, agi::ass::drawing::SerializeAss(agi::ass::drawing::ReversePath(ParseOpen(L, 1, 2))));
	return 1;
}

int Length(lua_State *L) {
	lua_pushnumber(L, agi::ass::drawing::PathLength(ParseOpen(L, 1, 2)));
	return 1;
}

int PercentAtLength(lua_State *L) {
	auto path = ParseOpen(L, 1, 3);
	double distance = luaL_checknumber(L, 2);
	lua_pushnumber(L, agi::ass::drawing::PercentAtLength(path, distance));
	return 1;
}

int PointAtPercent(lua_State *L) {
	Point point;
	Point tangent;
	return PushPointOrNil(L, TryGetPercentPosition(L, point, tangent), point);
}

int PointAtLength(lua_State *L) {
	auto path = ParseOpen(L, 1, 3);
	double distance = luaL_checknumber(L, 2);
	Point point;
	Point tangent;
	return PushPointOrNil(L, agi::ass::drawing::TryGetPositionAtLength(path, distance, point, tangent), point);
}

int AngleAtPercent(lua_State *L) {
	Point point;
	Point tangent;
	if (!TryGetPercentPosition(L, point, tangent))
		lua_pushnumber(L, 0.0);
	else
		lua_pushnumber(L, AngleFromTangent(tangent));
	return 1;
}

int SlopeAtPercent(lua_State *L) {
	Point point;
	Point tangent;
	if (!TryGetPercentPosition(L, point, tangent))
		lua_pushnumber(L, 0.0);
	else
		lua_pushnumber(L, SlopeFromTangent(tangent));
	return 1;
}

int Area(lua_State *L) {
	auto options = CheckToleranceAndMode(L, 2);
	double signed_area;
	Point centroid;
	if (!agi::ass::drawing::TryGetSignedAreaAndCentroid(ParseFilled(L, 1, options.mode_idx), signed_area, centroid, options.tolerance)) {
		lua_pushnil(L);
		return 1;
	}

	lua_pushnumber(L, std::abs(signed_area));
	return 1;
}

int Centroid(lua_State *L) {
	auto options = CheckToleranceAndMode(L, 2);
	double signed_area;
	Point centroid;
	return PushPointOrNil(L,
		agi::ass::drawing::TryGetSignedAreaAndCentroid(ParseFilled(L, 1, options.mode_idx), signed_area, centroid, options.tolerance),
		centroid);
}

int ShapeContainsPoint(lua_State *L) {
	bool contains = false;
	if (!agi::ass::drawing::TryDrawingContainsPoint(ParseFilled(L, 1, 4), luaL_checknumber(L, 2), luaL_checknumber(L, 3), contains))
		return BackendOperationError(L, "shape_contains_point");

	lua_pushboolean(L, contains);
	return 1;
}

int ShapeContainsRect(lua_State *L) {
	bool contains = false;
	if (!agi::ass::drawing::TryDrawingContainsRect(ParseFilled(L, 1, 6),
		luaL_checknumber(L, 2),
		luaL_checknumber(L, 3),
		luaL_checknumber(L, 4),
		luaL_checknumber(L, 5),
		contains))
		return BackendOperationError(L, "shape_contains_rect");

	lua_pushboolean(L, contains);
	return 1;
}

int ShapeBoolean(lua_State *L, DrawingBooleanOp op, char const *name) {
	PathData result;
	if (!agi::ass::drawing::TryDrawingBoolean(ParseFilled(L, 1, 3), ParseFilled(L, 2, 3), op, result))
		return BackendOperationError(L, name);

	PushShape(L, agi::ass::drawing::SerializeAssFilled(result));
	return 1;
}

int ShapeUnited(lua_State *L) {
	return ShapeBoolean(L, DrawingBooleanOp::Union, "shape_united");
}

int ShapeIntersected(lua_State *L) {
	return ShapeBoolean(L, DrawingBooleanOp::Intersect, "shape_intersected");
}

int ShapeSubtracted(lua_State *L) {
	return ShapeBoolean(L, DrawingBooleanOp::Subtract, "shape_subtracted");
}

int ShapeXored(lua_State *L) {
	return ShapeBoolean(L, DrawingBooleanOp::Xor, "shape_xored");
}

int PushShapeOutline(lua_State *L, int mode_idx, char const *name, double flatten_tolerance = 0.0) {
	PathData result;
	if (!agi::ass::drawing::TryDrawingOutline(ParseOpen(L, 1, mode_idx),
		luaL_checknumber(L, 2),
		CheckCapStyle(L, 3),
		CheckJoinStyle(L, 4),
		result))
		return BackendOperationError(L, name);

	if (flatten_tolerance > 0.0)
		result = agi::ass::drawing::FlattenPath(result, flatten_tolerance);
	PushShape(L, agi::ass::drawing::SerializeAssFilled(result));
	return 1;
}

int ShapeOutline(lua_State *L) {
	return PushShapeOutline(L, 5, "shape_outline");
}

int ShapeOutlineWithFlatten(lua_State *L) {
	return PushShapeOutline(L, 6, "shape_outline_with_flatten", luaL_checknumber(L, 5));
}

int ShapePatternOutline(lua_State *L) {
	PathData result;
	if (!agi::ass::drawing::TryDrawingPatternOutline(ParseOpen(L, 1, 8),
		luaL_checknumber(L, 2),
		CheckCapStyle(L, 3),
		CheckJoinStyle(L, 4),
		luaL_checknumber(L, 5),
		luaL_checknumber(L, 6),
		luaL_checknumber(L, 7),
		result))
		return BackendOperationError(L, "shape_pattern_outline");

	PushShape(L, agi::ass::drawing::SerializeAssFilled(result));
	return 1;
}

luaL_Reg const DrawingFunctions[] = {
	{"normalize", Normalize},
	{"normalize_open", NormalizeOpen},
	{"compact", Compact},
	{"shape_normalize_ass", Normalize},
	{"shape_normalize_ass_with_mode", Normalize},
	{"shape_rect", ShapeRect},
	{"shape_ellipse", ShapeEllipse},
	{"shape_rounded_rect", ShapeRoundedRect},
	{"shape_arc_move_to", ShapeArcMoveTo},
	{"shape_arc_to", ShapeArcTo},
	{"transform", Transform},
	{"translate", Translate},
	{"scale", Scale},
	{"rotate", Rotate},
	{"shear", Shear},
	{"shape_translate", Translate},
	{"shape_rotate", Rotate},
	{"shape_scale", Scale},
	{"shape_shear", Shear},
	{"bounds", Bounds},
	{"shape_bouding", ShapeBouding},
	{"shape_bouding_coords", ShapeBoudingCoords},
	{"flatten", Flatten},
	{"reverse", Reverse},
	{"length", Length},
	{"shape_length", Length},
	{"percent_at_length", PercentAtLength},
	{"shape_percent_at_length", PercentAtLength},
	{"point_at_percent", PointAtPercent},
	{"shape_point_at_percent", PointAtPercent},
	{"point_at_length", PointAtLength},
	{"angle_at_percent", AngleAtPercent},
	{"shape_angle_at_percent", AngleAtPercent},
	{"slope_at_percent", SlopeAtPercent},
	{"shape_slope_at_percent", SlopeAtPercent},
	{"area", Area},
	{"centroid", Centroid},
	{"shape_contains_point", ShapeContainsPoint},
	{"shape_contains_rect", ShapeContainsRect},
	{"shape_united", ShapeUnited},
	{"shape_intersected", ShapeIntersected},
	{"shape_subtracted", ShapeSubtracted},
	{"shape_xored", ShapeXored},
	{"shape_outline", ShapeOutline},
	{"shape_outline_with_flatten", ShapeOutlineWithFlatten},
	{"shape_pattern_outline", ShapePatternOutline},
	{nullptr, nullptr}
};

}

extern "C" int luaopen_drawing_impl(lua_State *L) {
	lua_createtable(L, 0, 45);
	luaL_register(L, nullptr, DrawingFunctions);
	return 1;
}
