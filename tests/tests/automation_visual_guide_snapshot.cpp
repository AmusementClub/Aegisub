#include <main.h>

#include "../../src/automation/automation_lua_runtime.h"
#include "../../src/automation/automation_visual_guide_snapshot.h"

#include <libaegisub/lua/modules.h>

#include <lua.hpp>

#include <memory>
#include <string>
#include <utility>

namespace {

struct LuaCloser {
	void operator()(lua_State *L) const {
		if (L)
			lua_close(L);
	}
};

using LuaState = std::unique_ptr<lua_State, LuaCloser>;

LuaState MakeLuaState() {
	LuaState L(luaL_newstate());
	if (L)
		agi::lua::preload_modules(L.get());
	return L;
}

void RunLua(lua_State *L, char const* script) {
	if (luaL_dostring(L, script) != 0) {
		auto const *error = lua_tostring(L, -1);
		FAIL() << (error ? error : "Lua assertion failed without an error message");
	}
}

void PushSnapshot(lua_State *L, Automation4::AutomationVisualGuideSnapshot const& snapshot) {
	Automation4::LuaPushVisualGuideSnapshot(L, snapshot);
	lua_setglobal(L, "snapshot");
}

Automation4::AutomationVisualGuide MakeGuide(std::string id) {
	Automation4::AutomationVisualGuide guide;
	guide.id = std::move(id);
	guide.kind = "measurement_segment";
	guide.coordinate_space = "script";
	return guide;
}

} // namespace

TEST(automation_visual_guide_snapshot, unavailable_snapshot_uses_documented_sentinel_values) {
	auto L = MakeLuaState();
	ASSERT_TRUE(L);

	Automation4::AutomationVisualGuideSnapshot unavailable;
	PushSnapshot(L.get(), unavailable);

	RunLua(L.get(), R"(
		assert(snapshot.schema_version == 1)
		assert(snapshot.available == false)
		assert(snapshot.generation == 0)
		assert(snapshot.frame == -1)
		assert(snapshot.script_resolution.width == 0)
		assert(snapshot.script_resolution.height == 0)
		assert(snapshot.frame_resolution.width == 0)
		assert(snapshot.frame_resolution.height == 0)
		assert(snapshot.selected_id == nil)
		assert(snapshot.last_measurement_id == nil)
		assert(type(snapshot.guides) == 'table')
		assert(#snapshot.guides == 0)
	)");
}

TEST(automation_visual_guide_snapshot, serializes_measurement_fields_as_values) {
	auto L = MakeLuaState();
	ASSERT_TRUE(L);

	Automation4::AutomationVisualGuideSnapshot source;
	source.available = true;
	source.generation = 23;
	source.frame = 47;
	source.script_width = 1920;
	source.script_height = 1080;
	source.frame_width = 3840;
	source.frame_height = 2160;
	source.selected_id = "measurement";
	source.last_measurement_id = "measurement";

	auto measurement = MakeGuide("measurement");
	measurement.first = { 4.5, 8.0 };
	measurement.second = { 19.5, 28.0 };
	measurement.delta_x = 15.0;
	measurement.delta_y = 20.0;
	measurement.distance = 25.0;
	measurement.angle_degrees = 53.13010235415598;
	source.guides.push_back(measurement);

	auto second = MakeGuide("second");
	second.first = { 0.0, 0.0 };
	second.second = { 10.0, 0.0 };
	second.delta_x = 10.0;
	second.distance = 10.0;
	source.guides.push_back(second);

	PushSnapshot(L.get(), source);

	// Mutating the host-side DTO after encoding must not change the Lua value.
	source.generation = 99;
	source.selected_id = "second";
	source.guides[0].first.x = -1.0;

	RunLua(L.get(), R"(
		local function near(actual, expected)
			assert(math.abs(actual - expected) < 0.0000001,
				string.format('expected %.17g, got %.17g', expected, actual))
		end

		assert(snapshot.schema_version == 1)
		assert(snapshot.available == true)
		assert(snapshot.generation == 23)
		assert(snapshot.frame == 47)
		assert(snapshot.script_resolution.width == 1920)
		assert(snapshot.script_resolution.height == 1080)
		assert(snapshot.frame_resolution.width == 3840)
		assert(snapshot.frame_resolution.height == 2160)
		assert(snapshot.selected_id == 'measurement')
		assert(snapshot.last_measurement_id == 'measurement')
		assert(#snapshot.guides == 2)

		local measurement = snapshot.guides[1]
		assert(measurement.id == 'measurement')
		assert(measurement.kind == 'measurement_segment')
		assert(measurement.coordinate_space == 'script')
		near(measurement.first.x, 4.5)
		near(measurement.first.y, 8.0)
		near(measurement.second.x, 19.5)
		near(measurement.second.y, 28.0)
		near(measurement.delta_x, 15.0)
		near(measurement.delta_y, 20.0)
		near(measurement.distance, 25.0)
		near(measurement.angle_degrees, 53.13010235415598)
		assert(measurement.visible == nil)
		assert(measurement.locked == nil)
		assert(measurement.show_label == nil)
		assert(measurement.position == nil)

		local second = snapshot.guides[2]
		assert(second.id == 'second')
		assert(second.kind == 'measurement_segment')
		assert(second.coordinate_space == 'script')
		near(second.first.x, 0.0)
		near(second.second.x, 10.0)
		near(second.distance, 10.0)
	)");
}
