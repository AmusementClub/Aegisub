#include "libaegisub/ass/drawing.h"
#include "libaegisub/lua/utils.h"

#include <lua.hpp>

#include <cctype>
#include <cmath>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <utility>

namespace {

using agi::ass::drawing::AssDrawingCompatMode;
using agi::ass::drawing::DrawingBooleanOp;
using agi::ass::drawing::DrawingStrokeCap;
using agi::ass::drawing::DrawingStrokeJoin;
using agi::ass::drawing::Matrix3x2;
using agi::ass::drawing::PathData;
using agi::ass::drawing::PathMeasure;
using agi::ass::drawing::Point;
using agi::ass::drawing::Rect;

char const *const kDrawingPathMetatable = "aegisub.drawing.path";

struct LuaDrawingPath {
	PathData path;
	bool filled_output = false;
	std::optional<PathMeasure> measure;
};

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

int PushFilledShape(lua_State *L, PathData const& path, bool compact = false) {
	std::string output;
	bool serialized = compact
		? agi::ass::drawing::TrySerializeAssCompactFilled(path, output)
		: agi::ass::drawing::TrySerializeAssFilled(path, output);
	if (!serialized)
		return agi::lua::error(L, "ASS filled output requires nonzero winding fill");

	PushShape(L, output);
	return 1;
}

LuaDrawingPath *CheckDrawingPath(lua_State *L, int idx) {
	return static_cast<LuaDrawingPath *>(luaL_checkudata(L, idx, kDrawingPathMetatable));
}

PathMeasure const& MeasureDrawingPath(LuaDrawingPath& path) {
	if (!path.measure)
		path.measure.emplace(path.path);
	return *path.measure;
}

void InvalidateDrawingPathMeasure(LuaDrawingPath& path) {
	path.measure.reset();
}

void ReplaceDrawingPath(LuaDrawingPath& path, PathData replacement) {
	path.path = std::move(replacement);
	InvalidateDrawingPathMeasure(path);
}

int PushDrawingPath(lua_State *L, PathData path, bool filled_output) {
	auto *storage = lua_newuserdata(L, sizeof(LuaDrawingPath));
	auto *drawing_path = new (storage) LuaDrawingPath {std::move(path), filled_output, std::nullopt};
	(void)drawing_path;
	luaL_getmetatable(L, kDrawingPathMetatable);
	lua_setmetatable(L, -2);
	return 1;
}

int ReturnDrawingPath(lua_State *L) {
	lua_settop(L, 1);
	return 1;
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

int PushControlPointBoundsOrZero(lua_State *L, PathData const& path, bool as_coords = false) {
	Rect bounds;
	if (!agi::ass::drawing::TryGetControlPointBounds(path, bounds))
		return PushZeroRect(L);

	return PushRect(L, bounds, as_coords);
}

int BackendOperationError(lua_State *L, char const *name) {
	if (agi::ass::drawing::DrawingBackendAvailable())
		return agi::lua::error(L, "%s failed in the drawing geometry backend", name);

	return agi::lua::error(L, "%s requires the drawing geometry backend", name);
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

int PushPointOrZero(lua_State *L, bool available, Point const& point) {
	Point value = available ? point : Point {};
	lua_pushnumber(L, value.x);
	lua_pushnumber(L, value.y);
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

int NewDrawingPath(lua_State *L, bool filled) {
	PathData path;
	if (lua_gettop(L) >= 1 && !lua_isnil(L, 1)) {
		auto shape = CheckShape(L, 1);
		auto mode = CheckCompatMode(L, 2);
		path = filled
			? agi::ass::drawing::ParseAss(shape, mode)
			: agi::ass::drawing::ParseAssOpen(shape, mode);
	}
	return PushDrawingPath(L, std::move(path), filled);
}

int DrawingPathNew(lua_State *L) {
	return NewDrawingPath(L, false);
}

int DrawingFilledPathNew(lua_State *L) {
	return NewDrawingPath(L, true);
}

int DrawingRectPath(lua_State *L) {
	return PushDrawingPath(L, agi::ass::drawing::MakeRect(
		luaL_checknumber(L, 1),
		luaL_checknumber(L, 2),
		luaL_checknumber(L, 3),
		luaL_checknumber(L, 4)), true);
}

int DrawingEllipsePath(lua_State *L) {
	return PushDrawingPath(L, agi::ass::drawing::MakeEllipse(
		luaL_checknumber(L, 1),
		luaL_checknumber(L, 2),
		luaL_checknumber(L, 3),
		luaL_checknumber(L, 4)), true);
}

int DrawingRoundedRectPath(lua_State *L) {
	return PushDrawingPath(L, agi::ass::drawing::MakeRoundedRect(
		luaL_checknumber(L, 1),
		luaL_checknumber(L, 2),
		luaL_checknumber(L, 3),
		luaL_checknumber(L, 4),
		luaL_checknumber(L, 5),
		luaL_checknumber(L, 6)), true);
}

int DrawingPathDestroy(lua_State *L) {
	CheckDrawingPath(L, 1)->~LuaDrawingPath();
	return 0;
}

int DrawingPathClone(lua_State *L) {
	auto const *path = CheckDrawingPath(L, 1);
	return PushDrawingPath(L, path->path, path->filled_output);
}

int DrawingPathAss(lua_State *L) {
	auto const *path = CheckDrawingPath(L, 1);
	if (path->filled_output)
		return PushFilledShape(L, path->path, true);

	PushShape(L, agi::ass::drawing::SerializeAss(path->path));
	return 1;
}

int DrawingPathFilledAss(lua_State *L) {
	return PushFilledShape(L, CheckDrawingPath(L, 1)->path, true);
}

int DrawingPathOpenAss(lua_State *L) {
	PushShape(L, agi::ass::drawing::SerializeAss(CheckDrawingPath(L, 1)->path));
	return 1;
}

int DrawingPathSetFilled(lua_State *L) {
	CheckDrawingPath(L, 1)->filled_output = true;
	return ReturnDrawingPath(L);
}

int DrawingPathSetOpen(lua_State *L) {
	CheckDrawingPath(L, 1)->filled_output = false;
	return ReturnDrawingPath(L);
}

int TransformDrawingPath(lua_State *L, Matrix3x2 const& matrix) {
	auto *path = CheckDrawingPath(L, 1);
	ReplaceDrawingPath(*path, agi::ass::drawing::TransformPath(std::move(path->path), matrix));
	return ReturnDrawingPath(L);
}

int DrawingPathTransform(lua_State *L) {
	return TransformDrawingPath(L, {
		luaL_checknumber(L, 2),
		luaL_checknumber(L, 3),
		luaL_checknumber(L, 4),
		luaL_checknumber(L, 5),
		luaL_checknumber(L, 6),
		luaL_checknumber(L, 7),
	});
}

int DrawingPathTranslate(lua_State *L) {
	double dx = luaL_checknumber(L, 2);
	double dy = luaL_checknumber(L, 3);
	return TransformDrawingPath(L, {1.0, 0.0, 0.0, 1.0, dx, dy});
}

int DrawingPathScale(lua_State *L) {
	double sx = luaL_checknumber(L, 2);
	double sy = luaL_checknumber(L, 3);
	return TransformDrawingPath(L, {sx, 0.0, 0.0, sy, 0.0, 0.0});
}

int DrawingPathRotate(lua_State *L) {
	double radians = luaL_checknumber(L, 2) * 3.14159265358979323846 / 180.0;
	double c = std::cos(radians);
	double s = std::sin(radians);
	return TransformDrawingPath(L, {c, s, -s, c, 0.0, 0.0});
}

int DrawingPathShear(lua_State *L) {
	double horizontal = luaL_checknumber(L, 2);
	double vertical = luaL_checknumber(L, 3);
	return TransformDrawingPath(L, {1.0, vertical, horizontal, 1.0, 0.0, 0.0});
}

int DrawingPathArcMoveTo(lua_State *L) {
	auto *path = CheckDrawingPath(L, 1);
	agi::ass::drawing::AppendArcMoveTo(path->path,
		luaL_checknumber(L, 2),
		luaL_checknumber(L, 3),
		luaL_checknumber(L, 4),
		luaL_checknumber(L, 5),
		luaL_checknumber(L, 6));
	InvalidateDrawingPathMeasure(*path);
	return ReturnDrawingPath(L);
}

int DrawingPathArcTo(lua_State *L) {
	auto *path = CheckDrawingPath(L, 1);
	agi::ass::drawing::AppendArcTo(path->path,
		luaL_checknumber(L, 2),
		luaL_checknumber(L, 3),
		luaL_checknumber(L, 4),
		luaL_checknumber(L, 5),
		luaL_checknumber(L, 6),
		luaL_checknumber(L, 7));
	InvalidateDrawingPathMeasure(*path);
	return ReturnDrawingPath(L);
}

int DrawingPathFlatten(lua_State *L) {
	auto *path = CheckDrawingPath(L, 1);
	ReplaceDrawingPath(*path, agi::ass::drawing::FlattenPath(path->path, luaL_optnumber(L, 2, 0.25)));
	return ReturnDrawingPath(L);
}

int DrawingPathReverse(lua_State *L) {
	auto *path = CheckDrawingPath(L, 1);
	ReplaceDrawingPath(*path, agi::ass::drawing::ReversePath(path->path));
	return ReturnDrawingPath(L);
}

int DrawingPathBounds(lua_State *L) {
	Rect bounds;
	if (!agi::ass::drawing::TryGetBounds(CheckDrawingPath(L, 1)->path, bounds)) {
		lua_pushnil(L);
		return 1;
	}
	return PushRect(L, bounds);
}

int DrawingPathControlBounds(lua_State *L) {
	Rect bounds;
	if (!agi::ass::drawing::TryGetControlPointBounds(CheckDrawingPath(L, 1)->path, bounds)) {
		lua_pushnil(L);
		return 1;
	}
	return PushRect(L, bounds);
}

int DrawingPathLength(lua_State *L) {
	lua_pushnumber(L, MeasureDrawingPath(*CheckDrawingPath(L, 1)).Length());
	return 1;
}

int DrawingPathPercentAtLength(lua_State *L) {
	lua_pushnumber(L, MeasureDrawingPath(*CheckDrawingPath(L, 1)).PercentAtLength(luaL_checknumber(L, 2)));
	return 1;
}

int DrawingPathPointAtPercent(lua_State *L) {
	Point point;
	Point tangent;
	return PushPointOrNil(L, MeasureDrawingPath(*CheckDrawingPath(L, 1)).TryGetPositionAtPercent(
		luaL_checknumber(L, 2), point, tangent), point);
}

int DrawingPathPointAtLength(lua_State *L) {
	Point point;
	Point tangent;
	return PushPointOrNil(L, MeasureDrawingPath(*CheckDrawingPath(L, 1)).TryGetPositionAtLength(
		luaL_checknumber(L, 2), point, tangent), point);
}

int DrawingPathAngleAtPercent(lua_State *L) {
	Point point;
	Point tangent;
	if (!MeasureDrawingPath(*CheckDrawingPath(L, 1)).TryGetPositionAtPercent(
		luaL_checknumber(L, 2), point, tangent))
		lua_pushnumber(L, 0.0);
	else
		lua_pushnumber(L, AngleFromTangent(tangent));
	return 1;
}

int DrawingPathSlopeAtPercent(lua_State *L) {
	Point point;
	Point tangent;
	if (!MeasureDrawingPath(*CheckDrawingPath(L, 1)).TryGetPositionAtPercent(
		luaL_checknumber(L, 2), point, tangent))
		lua_pushnumber(L, 0.0);
	else
		lua_pushnumber(L, SlopeFromTangent(tangent));
	return 1;
}

int DrawingPathArea(lua_State *L) {
	double signed_area;
	Point centroid;
	if (!agi::ass::drawing::TryGetSignedAreaAndCentroid(
		CheckDrawingPath(L, 1)->path, signed_area, centroid, luaL_optnumber(L, 2, 0.25))) {
		lua_pushnil(L);
		return 1;
	}
	lua_pushnumber(L, std::abs(signed_area));
	return 1;
}

int DrawingPathCentroid(lua_State *L) {
	double signed_area;
	Point centroid;
	return PushPointOrNil(L, agi::ass::drawing::TryGetSignedAreaAndCentroid(
		CheckDrawingPath(L, 1)->path, signed_area, centroid, luaL_optnumber(L, 2, 0.25)), centroid);
}

int PushFilledMetric(lua_State *L, PathData const& path, double tolerance, bool return_centroid, char const *name) {
	double area;
	Point centroid;
	bool measurable = false;
	if (!agi::ass::drawing::TryDrawingFilledAreaAndCentroid(path, tolerance, area, centroid, measurable))
		return BackendOperationError(L, name);
	if (!measurable) {
		lua_pushnil(L);
		return 1;
	}

	if (return_centroid)
		return PushPointOrNil(L, true, centroid);
	lua_pushnumber(L, area);
	return 1;
}

int DrawingPathFilledArea(lua_State *L) {
	return PushFilledMetric(L, CheckDrawingPath(L, 1)->path,
		luaL_optnumber(L, 2, 0.25), false, "drawing path filled_area");
}

int DrawingPathFilledCentroid(lua_State *L) {
	return PushFilledMetric(L, CheckDrawingPath(L, 1)->path,
		luaL_optnumber(L, 2, 0.25), true, "drawing path filled_centroid");
}

int DrawingPathContainsPoint(lua_State *L) {
	bool contains = false;
	if (!agi::ass::drawing::TryDrawingContainsPoint(CheckDrawingPath(L, 1)->path,
		luaL_checknumber(L, 2), luaL_checknumber(L, 3), contains))
		return BackendOperationError(L, "drawing path contains_point");
	lua_pushboolean(L, contains);
	return 1;
}

int DrawingPathContainsRect(lua_State *L) {
	bool contains = false;
	if (!agi::ass::drawing::TryDrawingContainsRect(CheckDrawingPath(L, 1)->path,
		luaL_checknumber(L, 2), luaL_checknumber(L, 3),
		luaL_checknumber(L, 4), luaL_checknumber(L, 5), contains))
		return BackendOperationError(L, "drawing path contains_rect");
	lua_pushboolean(L, contains);
	return 1;
}

int DrawingPathBoolean(lua_State *L, DrawingBooleanOp op, char const *name) {
	auto *path = CheckDrawingPath(L, 1);
	auto const *other = CheckDrawingPath(L, 2);
	PathData result;
	if (!agi::ass::drawing::TryDrawingBoolean(path->path, other->path, op, result))
		return BackendOperationError(L, name);
	ReplaceDrawingPath(*path, std::move(result));
	path->filled_output = true;
	return ReturnDrawingPath(L);
}

int DrawingPathUnite(lua_State *L) {
	return DrawingPathBoolean(L, DrawingBooleanOp::Union, "drawing path unite");
}

int DrawingPathIntersect(lua_State *L) {
	return DrawingPathBoolean(L, DrawingBooleanOp::Intersect, "drawing path intersect");
}

int DrawingPathSubtract(lua_State *L) {
	return DrawingPathBoolean(L, DrawingBooleanOp::Subtract, "drawing path subtract");
}

int DrawingPathXor(lua_State *L) {
	return DrawingPathBoolean(L, DrawingBooleanOp::Xor, "drawing path xor");
}

int DrawingPathOutline(lua_State *L) {
	auto *path = CheckDrawingPath(L, 1);
	PathData result;
	if (!agi::ass::drawing::TryDrawingOutline(path->path,
		luaL_checknumber(L, 2), CheckCapStyle(L, 3), CheckJoinStyle(L, 4), result))
		return BackendOperationError(L, "drawing path outline");
	if (lua_gettop(L) >= 5) {
		double tolerance = luaL_checknumber(L, 5);
		if (tolerance > 0.0)
			result = agi::ass::drawing::FlattenPath(result, tolerance);
	}
	ReplaceDrawingPath(*path, std::move(result));
	path->filled_output = true;
	return ReturnDrawingPath(L);
}

int DrawingPathPatternOutline(lua_State *L) {
	auto *path = CheckDrawingPath(L, 1);
	PathData result;
	if (!agi::ass::drawing::TryDrawingPatternOutline(path->path,
		luaL_checknumber(L, 2),
		CheckCapStyle(L, 3),
		CheckJoinStyle(L, 4),
		luaL_checknumber(L, 5),
		luaL_checknumber(L, 6),
		luaL_checknumber(L, 7),
		result))
		return BackendOperationError(L, "drawing path pattern_outline");
	ReplaceDrawingPath(*path, std::move(result));
	path->filled_output = true;
	return ReturnDrawingPath(L);
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

bool TryGetLegacyPercentPosition(lua_State *L, Point& point, Point& tangent) {
	auto path = ParseOpen(L, 1, 3);
	double percent = luaL_checknumber(L, 2);
	return agi::ass::drawing::TryGetLegacyPositionAtPercent(path, percent, point, tangent);
}

int Normalize(lua_State *L) {
	auto shape = CheckShape(L, 1);
	auto mode = CheckCompatMode(L, 2);
	return PushFilledShape(L, agi::ass::drawing::ParseAss(shape, mode));
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
	return PushFilledShape(L, agi::ass::drawing::MakeRect(
		luaL_checknumber(L, 1),
		luaL_checknumber(L, 2),
		luaL_checknumber(L, 3),
		luaL_checknumber(L, 4)));
}

int ShapeEllipse(lua_State *L) {
	return PushFilledShape(L, agi::ass::drawing::MakeEllipse(
		luaL_checknumber(L, 1),
		luaL_checknumber(L, 2),
		luaL_checknumber(L, 3),
		luaL_checknumber(L, 4)));
}

int ShapeRoundedRect(lua_State *L) {
	return PushFilledShape(L, agi::ass::drawing::MakeRoundedRect(
		luaL_checknumber(L, 1),
		luaL_checknumber(L, 2),
		luaL_checknumber(L, 3),
		luaL_checknumber(L, 4),
		luaL_checknumber(L, 5),
		luaL_checknumber(L, 6)));
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
	return PushControlPointBoundsOrZero(L, ParseFilled(L, 1, 2));
}

int ShapeBoudingCoords(lua_State *L) {
	return PushControlPointBoundsOrZero(L, ParseFilled(L, 1, 2), true);
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

int ShapePercentAtLength(lua_State *L) {
	auto path = ParseOpen(L, 1, 3);
	double distance = luaL_checknumber(L, 2);
	lua_pushnumber(L, agi::ass::drawing::LegacyPercentAtLength(path, distance));
	return 1;
}

int PointAtPercent(lua_State *L) {
	Point point;
	Point tangent;
	return PushPointOrNil(L, TryGetPercentPosition(L, point, tangent), point);
}

int ShapePointAtPercent(lua_State *L) {
	Point point;
	Point tangent;
	return PushPointOrZero(L, TryGetLegacyPercentPosition(L, point, tangent), point);
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

int ShapeAngleAtPercent(lua_State *L) {
	Point point;
	Point tangent;
	if (!TryGetLegacyPercentPosition(L, point, tangent))
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

int ShapeSlopeAtPercent(lua_State *L) {
	Point point;
	Point tangent;
	if (!TryGetLegacyPercentPosition(L, point, tangent))
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

int FilledArea(lua_State *L) {
	auto options = CheckToleranceAndMode(L, 2);
	return PushFilledMetric(L, ParseFilled(L, 1, options.mode_idx),
		options.tolerance, false, "filled_area");
}

int FilledCentroid(lua_State *L) {
	auto options = CheckToleranceAndMode(L, 2);
	return PushFilledMetric(L, ParseFilled(L, 1, options.mode_idx),
		options.tolerance, true, "filled_centroid");
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

	return PushFilledShape(L, result);
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
	return PushFilledShape(L, result);
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

	return PushFilledShape(L, result);
}

int PushCompatibilityFilledShape(lua_State *L, PathData const& path) {
	std::string output;
	if (!agi::ass::drawing::TrySerializeAssFilled(path, output))
		output.clear();
	PushShape(L, output);
	return 1;
}

int CompatibilityContainsPoint(lua_State *L) {
	bool contains = false;
	agi::ass::drawing::TryDrawingContainsPoint(ParseFilled(L, 1, 4),
		luaL_checknumber(L, 2), luaL_checknumber(L, 3), contains);
	lua_pushboolean(L, contains);
	return 1;
}

int CompatibilityContainsRect(lua_State *L) {
	bool contains = false;
	agi::ass::drawing::TryDrawingContainsRect(ParseFilled(L, 1, 6),
		luaL_checknumber(L, 2),
		luaL_checknumber(L, 3),
		luaL_checknumber(L, 4),
		luaL_checknumber(L, 5),
		contains);
	lua_pushboolean(L, contains);
	return 1;
}

int CompatibilityBoolean(lua_State *L, DrawingBooleanOp op) {
	PathData result;
	if (!agi::ass::drawing::TryDrawingBoolean(ParseFilled(L, 1, 3), ParseFilled(L, 2, 3), op, result))
		result = {};
	return PushCompatibilityFilledShape(L, result);
}

int CompatibilityUnited(lua_State *L) {
	return CompatibilityBoolean(L, DrawingBooleanOp::Union);
}

int CompatibilityIntersected(lua_State *L) {
	return CompatibilityBoolean(L, DrawingBooleanOp::Intersect);
}

int CompatibilitySubtracted(lua_State *L) {
	return CompatibilityBoolean(L, DrawingBooleanOp::Subtract);
}

int CompatibilityXored(lua_State *L) {
	return CompatibilityBoolean(L, DrawingBooleanOp::Xor);
}

int CompatibilityOutline(lua_State *L, int mode_idx, double flatten_tolerance = 0.0) {
	PathData result;
	if (!agi::ass::drawing::TryDrawingOutline(ParseOpen(L, 1, mode_idx),
		luaL_checknumber(L, 2), CheckCapStyle(L, 3), CheckJoinStyle(L, 4), result))
		result = {};
	if (flatten_tolerance > 0.0)
		result = agi::ass::drawing::FlattenPath(result, flatten_tolerance);
	return PushCompatibilityFilledShape(L, result);
}

int CompatibilityShapeOutline(lua_State *L) {
	return CompatibilityOutline(L, 5);
}

int CompatibilityShapeOutlineWithFlatten(lua_State *L) {
	return CompatibilityOutline(L, 6, luaL_checknumber(L, 5));
}

int CompatibilityPatternOutline(lua_State *L) {
	PathData result;
	if (!agi::ass::drawing::TryDrawingPatternOutline(ParseOpen(L, 1, 8),
		luaL_checknumber(L, 2),
		CheckCapStyle(L, 3),
		CheckJoinStyle(L, 4),
		luaL_checknumber(L, 5),
		luaL_checknumber(L, 6),
		luaL_checknumber(L, 7),
		result))
		result = {};
	return PushCompatibilityFilledShape(L, result);
}

luaL_Reg const DrawingPathMethods[] = {
	{"clone", DrawingPathClone},
	{"ass", DrawingPathAss},
	{"filled_ass", DrawingPathFilledAss},
	{"open_ass", DrawingPathOpenAss},
	{"fill", DrawingPathSetFilled},
	{"open", DrawingPathSetOpen},
	{"transform", DrawingPathTransform},
	{"translate", DrawingPathTranslate},
	{"scale", DrawingPathScale},
	{"rotate", DrawingPathRotate},
	{"shear", DrawingPathShear},
	{"arc_move_to", DrawingPathArcMoveTo},
	{"arc_to", DrawingPathArcTo},
	{"flatten", DrawingPathFlatten},
	{"reverse", DrawingPathReverse},
	{"bounds", DrawingPathBounds},
	{"control_bounds", DrawingPathControlBounds},
	{"length", DrawingPathLength},
	{"percent_at_length", DrawingPathPercentAtLength},
	{"point_at_percent", DrawingPathPointAtPercent},
	{"point_at_length", DrawingPathPointAtLength},
	{"angle_at_percent", DrawingPathAngleAtPercent},
	{"slope_at_percent", DrawingPathSlopeAtPercent},
	{"area", DrawingPathArea},
	{"centroid", DrawingPathCentroid},
	{"filled_area", DrawingPathFilledArea},
	{"filled_centroid", DrawingPathFilledCentroid},
	{"contains_point", DrawingPathContainsPoint},
	{"contains_rect", DrawingPathContainsRect},
	{"unite", DrawingPathUnite},
	{"intersect", DrawingPathIntersect},
	{"subtract", DrawingPathSubtract},
	{"xor", DrawingPathXor},
	{"outline", DrawingPathOutline},
	{"pattern_outline", DrawingPathPatternOutline},
	{nullptr, nullptr}
};

void RegisterDrawingPathMetatable(lua_State *L) {
	if (luaL_newmetatable(L, kDrawingPathMetatable)) {
		lua_pushcfunction(L, DrawingPathDestroy);
		lua_setfield(L, -2, "__gc");
		lua_pushcfunction(L, DrawingPathAss);
		lua_setfield(L, -2, "__tostring");
		lua_createtable(L, 0, static_cast<int>(sizeof(DrawingPathMethods) / sizeof(DrawingPathMethods[0]) - 1));
		luaL_register(L, nullptr, DrawingPathMethods);
		lua_setfield(L, -2, "__index");
	}
	lua_pop(L, 1);
}

luaL_Reg const DrawingFunctions[] = {
	{"path", DrawingPathNew},
	{"filled_path", DrawingFilledPathNew},
	{"rect", DrawingRectPath},
	{"ellipse", DrawingEllipsePath},
	{"rounded_rect", DrawingRoundedRectPath},
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
	{"shape_percent_at_length", ShapePercentAtLength},
	{"point_at_percent", PointAtPercent},
	{"shape_point_at_percent", ShapePointAtPercent},
	{"point_at_length", PointAtLength},
	{"angle_at_percent", AngleAtPercent},
	{"shape_angle_at_percent", ShapeAngleAtPercent},
	{"slope_at_percent", SlopeAtPercent},
	{"shape_slope_at_percent", ShapeSlopeAtPercent},
	{"area", Area},
	{"centroid", Centroid},
	{"filled_area", FilledArea},
	{"filled_centroid", FilledCentroid},
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

luaL_Reg const ShapeCompatibilityFunctions[] = {
	{"ellipse", ShapeEllipse},
	{"rect", ShapeRect},
	{"rounded_rect", ShapeRoundedRect},
	{"arc_move_to", ShapeArcMoveTo},
	{"arc_to", ShapeArcTo},
	{"angle_at_percent", ShapeAngleAtPercent},
	{"length", Length},
	{"percent_at_length", ShapePercentAtLength},
	{"point_at_percent", ShapePointAtPercent},
	{"slope_at_percent", ShapeSlopeAtPercent},
	{"bounding", ShapeBouding},
	{"bounding_coords", ShapeBoudingCoords},
	{"contains_point", CompatibilityContainsPoint},
	{"contains_rect", CompatibilityContainsRect},
	{"translate", Translate},
	{"rotate", Rotate},
	{"scale", Scale},
	{"shear", Shear},
	{"united", CompatibilityUnited},
	{"intersected", CompatibilityIntersected},
	{"subtracted", CompatibilitySubtracted},
	{"outline", CompatibilityShapeOutline},
	{"pattern_outline", CompatibilityPatternOutline},
	{"xored", CompatibilityXored},
	{"outline_with_flatten", CompatibilityShapeOutlineWithFlatten},
	{"normalize", Normalize},
	{nullptr, nullptr}
};

}

extern "C" int luaopen_drawing_impl(lua_State *L) {
	RegisterDrawingPathMetatable(L);
	lua_createtable(L, 0, static_cast<int>(sizeof(DrawingFunctions) / sizeof(DrawingFunctions[0]) - 1));
	luaL_register(L, nullptr, DrawingFunctions);
	return 1;
}

extern "C" int luaopen_shape_compat_impl(lua_State *L) {
	lua_createtable(L, 0, static_cast<int>(sizeof(ShapeCompatibilityFunctions) / sizeof(ShapeCompatibilityFunctions[0]) - 1));
	luaL_register(L, nullptr, ShapeCompatibilityFunctions);
	lua_pushvalue(L, -1);
	lua_setglobal(L, "shape");
	return 1;
}
