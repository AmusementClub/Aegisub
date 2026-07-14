// Copyright (c) 2026 Aegisub Project
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

#include <main.h>

#include <libaegisub/ass/drawing.h>
#include <libaegisub/lua/modules.h>
#include <libaegisub/lua/utils.h>

#include <lua.hpp>

#include <memory>

namespace {

struct LuaCloser {
	void operator()(lua_State *L) const {
		if (L)
			lua_close(L);
	}
};

using LuaState = std::unique_ptr<lua_State, LuaCloser>;

void RunLua(lua_State *L, char const* script) {
	if (luaL_dostring(L, script) != 0) {
		auto error = agi::lua::get_string_or_default(L, -1);
		lua_pop(L, 1);
		FAIL() << error;
	}
}

LuaState MakeLuaState() {
	LuaState L(luaL_newstate());
	agi::lua::preload_modules(L.get());
	return L;
}

}

TEST(lua_drawing_module, exposes_string_transform_api) {
	auto L = MakeLuaState();
	RunLua(L.get(), R"(
		local drawing = require 'aegisub.drawing'
		assert(drawing.normalize_open('m 0 0 l 10 10') == 'm 0 0 l 10 10')
		assert(drawing.normalize('m 0 0 l 10 0 l 10 10') == 'm 0 0 l 10 0 10 10')
		assert(drawing.compact('m 10 10 l 0 10 0 0 10 0 10 10') == 'm 0 0 l 10 0 10 10 0 10')
		assert(drawing.translate('m 0 0 l 10 10', 1, 2) == 'm 1 2 l 11 12')
		assert(drawing.scale('m 0 0 l 10 10', 2, 0.5) == 'm 0 0 l 20 5')
		assert(drawing.rotate('m 1 0 l 0 1', 90) == 'm 0 1 l -1 0')
		assert(drawing.transform('m 1 1 l 2 2', 2, 0, 0, 3, 3, 4) == 'm 5 7 l 7 10')
	)");
}

TEST(lua_drawing_module, path_userdata_serializes_once_after_chained_geometry) {
	auto L = MakeLuaState();
	RunLua(L.get(), R"(
		local drawing = require 'aegisub.drawing'
		local path = drawing.rect(0, 0, 2, 2)
		for _ = 1, 50 do
			path:translate(0.01, 0)
		end
		assert(path:ass() == 'm 0.5 0 l 2.5 0 2.5 2 0.5 2')

		local clone = path:clone():rotate(90):translate(1, 2)
		assert(path:ass() == 'm 0.5 0 l 2.5 0 2.5 2 0.5 2')
		assert(clone:ass() ~= path:ass())
		assert(math.abs(path:length() - 8) < 0.000001)

		local open = drawing.path():arc_move_to(0, 0, 20, 20, 0)
		assert(open:ass() == 'm 20 10')
		assert(tostring(open) == 'm 20 10')
	)");
}

TEST(lua_drawing_module, path_userdata_exposes_bounds_and_serialization_modes) {
	auto L = MakeLuaState();
	RunLua(L.get(), R"(
		local drawing = require 'aegisub.drawing'
		local path = drawing.path('m 10 10 l 0 10 0 0 10 0 10 10')
		local open_ass = path:open_ass()
		local filled_ass = path:filled_ass()
		assert(open_ass ~= filled_ass)
		assert(path:ass() == open_ass)
		path:fill()
		assert(path:ass() == filled_ass)
		path:open()
		assert(path:ass() == open_ass)

		local curve = drawing.path('m 0 0 b 0 10 10 10 10 0')
		local x, y, width, height = curve:control_bounds()
		assert(x == 0 and y == 0 and width == 10 and height == 10)
		x, y, width, height = curve:bounds()
		assert(x == 0 and y == 0 and width == 10 and height == 7.5)
	)");
}

TEST(lua_drawing_module, preloads_original_shape_lua_compatibility_surface) {
	auto L = MakeLuaState();
	RunLua(L.get(), R"(
		local shape = require 'shape'
		assert(shape == _G.shape)
		local expected = {
			'ellipse', 'rect', 'rounded_rect', 'arc_move_to', 'arc_to',
			'angle_at_percent', 'length', 'percent_at_length',
			'point_at_percent', 'slope_at_percent', 'bounding',
			'bounding_coords', 'contains_point', 'contains_rect',
			'translate', 'rotate', 'scale', 'shear', 'united',
			'intersected', 'subtracted', 'outline', 'pattern_outline',
		}
		for _, name in ipairs(expected) do
			assert(type(shape[name]) == 'function', name)
		end

		assert(shape.rect(0, 0, 10, 10) == 'm 0 0 l 10 0 10 10 0 10')
		local x, y = shape.point_at_percent('m 0 0 b 0 10 20 10 30 0', 0.25)
		assert(math.abs(x - 3.28125) < 0.000001)
		assert(math.abs(y - 5.625) < 0.000001)
	)");

	if (agi::ass::drawing::DrawingSkiaBackendAvailable()) {
		RunLua(L.get(), R"(
			local shape = require 'shape'
			assert(shape.united(shape.rect(0, 0, 10, 10), shape.rect(5, 5, 10, 10)) ~= '')
		)");
	} else {
		RunLua(L.get(), R"(
			local shape = require 'shape'
			assert(shape.united(shape.rect(0, 0, 10, 10), shape.rect(5, 5, 10, 10)) == '')
			assert(shape.contains_point(shape.rect(0, 0, 10, 10), 5, 5) == false)
		)");
	}
}

TEST(lua_drawing_module, accepts_named_compatibility_modes) {
	auto L = MakeLuaState();
	RunLua(L.get(), R"(
		local drawing = require 'aegisub.drawing'
		assert(drawing.normalize_open('m 0.01 0 l 1.01 0', 'libass') == 'm 0.02 0 l 1.02 0')
		assert(drawing.normalize_open('m 0.01 0 l 1.01 0', 'vsfilter') == 'm 0 0 l 1 0')
	)");
}

TEST(lua_drawing_module, exposes_geometry_query_and_path_tools) {
	auto L = MakeLuaState();
	RunLua(L.get(), R"(
		local drawing = require 'aegisub.drawing'
		local function near(a, b)
			return math.abs(a - b) < 0.000001
		end

		local x, y, w, h = drawing.bounds('m 0 0 b 0 10 10 10 10 0')
		assert(near(x, 0))
		assert(near(y, 0))
		assert(near(w, 10))
		assert(near(h, 7.5))

		assert(drawing.bounds('') == nil)
		assert(drawing.flatten('m 0 0 b 0 10 10 10 10 0', 100) == 'm 0 0 l 10 0')
		assert(drawing.reverse('m 0 0 l 10 0 b 10 10 20 10 20 0') == 'm 20 0 b 20 10 10 10 10 0 l 0 0')
	)");
}

TEST(lua_drawing_module, exposes_path_measurement_api) {
	auto L = MakeLuaState();
	RunLua(L.get(), R"(
		local drawing = require 'aegisub.drawing'
		local function near(a, b)
			return math.abs(a - b) < 0.000001
		end

		assert(near(drawing.length('m 0 0 l 3 4 l 6 8'), 10))
		assert(near(drawing.percent_at_length('m 0 0 l 10 0', 2.5), 0.25))

		local x, y = drawing.point_at_percent('m 0 0 l 10 0', 0.25)
		assert(near(x, 2.5))
		assert(near(y, 0))

		x, y = drawing.point_at_length('m 0 0 l 10 0 l 10 10', 15)
		assert(near(x, 10))
		assert(near(y, 5))

		assert(near(drawing.angle_at_percent('m 0 0 l 0 10', 0.5), 270))
		assert(near(drawing.slope_at_percent('m 0 0 l 10 10', 0.5), 1))
		assert(near(drawing.area('m 0 0 l 10 0 l 10 10 l 0 10'), 100))

		x, y = drawing.centroid('m 0 0 l 10 0 l 10 10 l 0 10')
		assert(near(x, 5))
		assert(near(y, 5))
	)");
}

TEST(lua_drawing_module, exposes_legacy_shape_named_api) {
	auto L = MakeLuaState();
	RunLua(L.get(), R"(
		local drawing = require 'aegisub.drawing'
		local function near(a, b)
			return math.abs(a - b) < 0.000001
		end

		assert(drawing.shape_rect(0, 0, 10, 10) == 'm 0 0 l 10 0 10 10 0 10')
		assert(drawing.shape_translate('m 0 0 l 10 10', 1, 2) == 'm 1 2 l 11 12')
		assert(near(drawing.shape_length('m 0 0 l 3 4'), 5))

		local x, y = drawing.shape_point_at_percent('m 0 0 l 10 0', 0.25)
		assert(near(x, 2.5))
		assert(near(y, 0))

		local curve = 'm 0 0 b 0 10 20 10 30 0'
		local legacy_x, legacy_y = drawing.shape_point_at_percent(curve, 0.25)
		assert(near(legacy_x, 3.28125))
		assert(near(legacy_y, 5.625))
		local modern_x, modern_y = drawing.point_at_percent(curve, 0.25)
		assert(math.abs(modern_x - legacy_x) + math.abs(modern_y - legacy_y) > 0.01)
		assert(near(drawing.shape_slope_at_percent(curve, 0.25), 15 / 24.375))
		assert(math.abs(drawing.shape_angle_at_percent(curve, 0.25) - drawing.angle_at_percent(curve, 0.25)) > 0.01)
		assert(math.abs(drawing.shape_percent_at_length(curve, drawing.length(curve) * 0.25) - 0.25) > 0.0001)

		x, y = drawing.shape_point_at_percent(curve, -0.01)
		assert(near(x, 0) and near(y, 0))
		x, y = drawing.shape_point_at_percent(curve, 1.01)
		assert(near(x, 0) and near(y, 0))
		assert(near(drawing.shape_angle_at_percent(curve, -0.01), 0))
		assert(near(drawing.shape_slope_at_percent(curve, 1.01), 0))
		assert(near(drawing.shape_percent_at_length(curve, -1), 0))
		assert(near(drawing.shape_percent_at_length(curve, math.huge), 1))

		-- The non-shape API retains modern clamped, normalized arc-length semantics.
		x, y = drawing.point_at_percent(curve, 1.01)
		assert(near(x, 30) and near(y, 0))

		local w, h
		x, y, w, h = drawing.shape_bouding(drawing.shape_ellipse(0, 0, 20, 10))
		assert(near(x, 0))
		assert(near(y, 0))
		assert(near(w, 20))
		assert(near(h, 10))

		-- Legacy control-point bounds, not tight curve extrema.
		x, y, w, h = drawing.shape_bouding('m 0 0 b 0 10 10 10 10 0')
		assert(near(x, 0))
		assert(near(y, 0))
		assert(near(w, 10))
		assert(near(h, 10))

		-- Public bounds() stays geometric/tight.
		x, y, w, h = drawing.bounds('m 0 0 b 0 10 10 10 10 0')
		assert(near(x, 0))
		assert(near(y, 0))
		assert(near(w, 10))
		assert(near(h, 7.5))

		local x1, y1, x2, y2 = drawing.shape_bouding_coords(drawing.shape_rect(0, 0, 10, 10))
		assert(near(x1, 0))
		assert(near(y1, 0))
		assert(near(x2, 10))
		assert(near(y2, 10))

		assert(drawing.shape_arc_move_to('', 0, 0, 20, 20, 0) == 'm 20 10')
	)");
}

TEST(lua_drawing_module, exposes_requested_public_shape_api_without_debug_helpers) {
	auto L = MakeLuaState();
	RunLua(L.get(), R"(
		local drawing = require 'aegisub.drawing'
		local expected = {
			'shape_angle_at_percent',
			'shape_arc_move_to',
			'shape_arc_to',
			'shape_bouding',
			'shape_bouding_coords',
			'shape_contains_point',
			'shape_contains_rect',
			'shape_ellipse',
			'shape_intersected',
			'shape_length',
			'shape_normalize_ass',
			'shape_normalize_ass_with_mode',
			'shape_outline',
			'shape_outline_with_flatten',
			'shape_pattern_outline',
			'shape_percent_at_length',
			'shape_point_at_percent',
			'shape_rect',
			'shape_rotate',
			'shape_rounded_rect',
			'shape_scale',
			'shape_shear',
			'shape_slope_at_percent',
			'shape_subtracted',
			'shape_translate',
			'shape_united',
			'shape_xored',
		}

		for _, name in ipairs(expected) do
			assert(type(drawing[name]) == 'function', name)
		end

		for name in pairs(drawing) do
			assert(not name:match('^shape_debug_'), name)
		end
	)");
}

TEST(lua_drawing_module, exposes_skia_backed_legacy_shape_api) {
	auto L = MakeLuaState();

	if (!agi::ass::drawing::DrawingSkiaBackendAvailable()) {
		RunLua(L.get(), R"(
			local drawing = require 'aegisub.drawing'
			assert(not pcall(function()
				drawing.shape_united('m 0 0 l 10 0', 'm 0 0 l 0 10')
			end))
		)");
		return;
	}

	RunLua(L.get(), R"(
		local drawing = require 'aegisub.drawing'

		local rect = drawing.shape_rect(0, 0, 10, 10)
		assert(drawing.shape_contains_point(rect, 5, 5))
		assert(not drawing.shape_contains_point(rect, 15, 5))
		assert(drawing.shape_contains_rect(rect, 2, 2, 2, 2))
		assert(not drawing.shape_contains_rect(rect, 8, 8, 4, 4))

		local lhs = drawing.shape_rect(0, 0, 10, 10)
		local rhs = drawing.shape_rect(5, 5, 10, 10)
		assert(drawing.shape_contains_point(drawing.shape_united(lhs, rhs), 12, 12))
		assert(drawing.shape_contains_point(drawing.shape_intersected(lhs, rhs), 7, 7))
		assert(not drawing.shape_contains_point(drawing.shape_intersected(lhs, rhs), 2, 2))
		assert(drawing.shape_contains_point(drawing.shape_subtracted(lhs, rhs), 2, 2))
		assert(not drawing.shape_contains_point(drawing.shape_subtracted(lhs, rhs), 7, 7))
		assert(drawing.shape_contains_point(drawing.shape_xored(lhs, rhs), 2, 2))
		assert(not drawing.shape_contains_point(drawing.shape_xored(lhs, rhs), 7, 7))

		local outline = drawing.shape_outline('m 0 0 l 10 0', 2, 'flat', 'bevel')
		assert(drawing.shape_contains_point(outline, 5, 0))
		assert(not drawing.shape_contains_point(outline, 5, 2))

		local flat = drawing.shape_outline_with_flatten('m 0 0 b 0 10 10 10 10 0', 2, 'round', 'round', 0.5)
		assert(flat ~= '' and not flat:find(' b ', 1, true))

		local dashed = drawing.shape_pattern_outline('m 0 0 l 20 0', 2, 'flat', 'bevel', 2.5, 2.5, 0)
		assert(drawing.shape_contains_point(dashed, 2, 0))
		assert(not drawing.shape_contains_point(dashed, 7, 0))

		local path_lhs = drawing.rect(0, 0, 10, 10)
		local path_rhs = drawing.rect(5, 5, 10, 10)
		path_lhs:unite(path_rhs)
		assert(path_lhs:contains_point(12, 12))
		assert(path_lhs:ass() ~= '')

		local path_outline = drawing.path('m 0 0 l 20 0')
		path_outline:outline(2, 'flat', 'bevel')
		assert(path_outline:contains_point(5, 0))
		assert(path_outline:ass() ~= '')

		local path_pattern = drawing.path('m 0 0 l 20 0')
		path_pattern:pattern_outline(2, 'flat', 'bevel', 2.5, 2.5, 0)
		assert(path_pattern:contains_point(2, 0))
		assert(not path_pattern:contains_point(7, 0))

		assert(drawing.shape_outline('m 0 0 l 10 0', 0, 'flat', 'bevel') == '')
		assert(drawing.shape_pattern_outline('m 0 0 l 10 0', 2, 'flat', 'bevel', 0, 1, 0) == '')
		assert(drawing.shape_pattern_outline('', 2, 'flat', 'bevel', 1, 1, 0) == '')
		assert(not drawing.shape_contains_rect(rect, 1, 1, 0, 2))
		assert(not drawing.shape_contains_rect(rect, 1, 1, -2, 2))

		assert(not pcall(function()
			drawing.shape_outline('m 0 0 l 10 0', -1, 'flat', 'bevel')
		end))
		assert(not pcall(function()
			drawing.shape_pattern_outline('m 0 0 l 10 0', 2, 'flat', 'bevel', -1, 1, 0)
		end))
		assert(not pcall(function()
			drawing.shape_pattern_outline('m 0 0 l 10 0', 2, 'flat', 'bevel', 1, -1, 0)
		end))
		assert(not pcall(function()
			drawing.shape_outline('m 0 0 l 10 0', 0 / 0, 'flat', 'bevel')
		end))

		local continuous = drawing.shape_pattern_outline('m 0 0 l 10 0', 2, 'flat', 'bevel', 1, 0, -3)
		assert(drawing.shape_contains_point(continuous, 5, 0))

		local shape = require 'shape'
		assert(shape.outline('m 0 0 l 10 0', -1, 'flat', 'bevel') == '')
		assert(shape.pattern_outline('m 0 0 l 10 0', 2, 'flat', 'bevel', -1, 1, 0) == '')
		assert(shape.pattern_outline('m 0 0 l 10 0', 2, 'flat', 'bevel', 1, -1, 0) == '')
		assert(not shape.contains_rect(rect, 1, 1, -2, 2))
	)");
}

TEST(lua_drawing_module, exposes_filled_topology_metrics) {
	auto L = MakeLuaState();

	if (!agi::ass::drawing::DrawingSkiaBackendAvailable()) {
		RunLua(L.get(), R"(
			local drawing = require 'aegisub.drawing'
			assert(not pcall(function()
				drawing.filled_area('m 0 0 l 10 0 10 10 0 10')
			end))
			assert(not pcall(function()
				drawing.filled_path('m 0 0 l 10 0 10 10 0 10'):filled_centroid()
			end))
		)");
		return;
	}

	RunLua(L.get(), R"(
		local drawing = require 'aegisub.drawing'
		local function near(a, b)
			return math.abs(a - b) < 0.000001
		end

		local overlapping =
			'm 0 0 l 10 0 10 10 0 10 ' ..
			'm 5 0 l 15 0 15 10 5 10'
		assert(near(drawing.area(overlapping), 200))
		assert(near(drawing.filled_area(overlapping), 150))
		local x, y = drawing.filled_centroid(overlapping)
		assert(near(x, 7.5) and near(y, 5))

		local bowtie = 'm 0 0 l 20 20 0 20 20 0'
		assert(drawing.area(bowtie) == nil)
		assert(near(drawing.filled_area(bowtie), 200))
		x, y = drawing.filled_centroid(bowtie)
		assert(near(x, 10) and near(y, 10))

		local path = drawing.filled_path(overlapping)
		assert(near(path:filled_area(), 150))
		x, y = path:filled_centroid()
		assert(near(x, 7.5) and near(y, 5))
		assert(near(path:filled_area(), 150))
		path:scale(2, 1)
		assert(near(path:filled_area(), 300))
		x, y = path:filled_centroid()
		assert(near(x, 15) and near(y, 5))

		local curved = drawing.filled_path('m 0 0 b 0 20 20 20 20 0')
		local coarse_area = curved:filled_area(10)
		local fine_area = curved:filled_area(0.01)
		assert(math.abs(coarse_area - fine_area) > 0.01)
		assert(near(curved:filled_area(0.01), fine_area))
		curved:scale(2, 1)
		assert(near(curved:filled_area(0.01), fine_area * 2))

		local empty = drawing.filled_path()
		assert(empty:filled_area() == nil)
		assert(empty:filled_centroid() == nil)
		assert(drawing.filled_area('') == nil)
	)");
}

TEST(lua_drawing_module, path_measurement_cache_follows_mutations) {
	auto L = MakeLuaState();
	RunLua(L.get(), R"(
		local drawing = require 'aegisub.drawing'
		local function near(a, b)
			return math.abs(a - b) < 0.000001
		end

		local path = drawing.path('m 0 0 l 3 4')
		assert(near(path:length(), 5))
		assert(near(path:length(), 5))
		local x, y = path:point_at_percent(0.5)
		assert(near(x, 1.5) and near(y, 2))

		path:scale(2, 2)
		assert(near(path:length(), 10))
		x, y = path:point_at_length(5)
		assert(near(x, 3) and near(y, 4))

		path:translate(10, 0)
		x, y = path:point_at_percent(0.5)
		assert(near(x, 13) and near(y, 4))

		local reversed = drawing.path('m 0 0 l 10 0')
		x, y = reversed:point_at_percent(0.25)
		assert(near(x, 2.5) and near(y, 0))
		reversed:reverse()
		x, y = reversed:point_at_percent(0.25)
		assert(near(x, 7.5) and near(y, 0))

		local curve = drawing.path('m 0 0 b 0 10 10 10 10 0')
		local curve_length = curve:length()
		curve:flatten(100)
		assert(curve:length() < curve_length - 1)

		local arc = drawing.path('m 20 5')
		assert(near(arc:length(), 0))
		arc:arc_to(0, 0, 20, 10, 0, 90)
		assert(arc:length() > 10)
	)");

	if (agi::ass::drawing::DrawingSkiaBackendAvailable()) {
		RunLua(L.get(), R"(
			local drawing = require 'aegisub.drawing'
			local function near(a, b)
				return math.abs(a - b) < 0.000001
			end

			local combined = drawing.rect(0, 0, 10, 10)
			assert(near(combined:filled_area(), 100))
			combined:unite(drawing.rect(5, 0, 10, 10))
			assert(near(combined:filled_area(), 150))

			local stroked = drawing.path('m 0 0 l 10 0')
			assert(stroked:filled_area() == nil)
			stroked:outline(2, 'flat', 'bevel')
			assert(near(stroked:filled_area(), 20))

			local arc = drawing.filled_path('m 20 5')
			assert(arc:filled_area() == nil)
			arc:arc_to(0, 0, 20, 10, 0, 90)
			assert(arc:filled_area() > 0)
		)");
	}
}

TEST(lua_drawing_module, path_userdata_is_reclaimed_across_gc_cycles) {
	auto L = MakeLuaState();
	RunLua(L.get(), R"(
		local drawing = require 'aegisub.drawing'
		for index = 1, 1000 do
			local path = drawing.rect(index, index, 10, 10)
			path:length()
			if index % 25 == 0 then
				collectgarbage('collect')
			end
		end
		collectgarbage('collect')
		collectgarbage('collect')
	)");
}
