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

TEST(lua_drawing_module, accepts_named_compatibility_modes) {
	auto L = MakeLuaState();
	RunLua(L.get(), R"(
		local drawing = require 'aegisub.drawing'
		assert(drawing.normalize_open('m 0.01 0 l 1.01 0', 'libass') == 'm 0.016 0 l 1.016 0')
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

		local w, h
		x, y, w, h = drawing.shape_bouding(drawing.shape_ellipse(0, 0, 20, 10))
		assert(near(x, 0))
		assert(near(y, 0))
		assert(near(w, 20))
		assert(near(h, 10))

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
	)");
}
