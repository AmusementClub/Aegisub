#include <main.h>

#include <libaegisub/lua/modules.h>
#include <libaegisub/lua/script_reader.h>
#include <libaegisub/lua/utils.h>
#include <libaegisub/fs.h>

#include <lua.hpp>

#include <memory>
#include <vector>

namespace {
struct LuaCloser {
	void operator()(lua_State *lua) const {
		if (lua)
			lua_close(lua);
	}
};

using LuaState = std::unique_ptr<lua_State, LuaCloser>;

LuaState MakeLuaState() {
	LuaState lua(luaL_newstate());
	agi::lua::preload_modules(lua.get());
	std::vector<agi::fs::path> include_path{
		agi::fs::PathFromString(AEGISUB_PROJECT_SOURCE_DIR) / "automation" / "include"
	};
	if (!agi::lua::Install(lua.get(), include_path))
		return {};
	return lua;
}

void RunLua(lua_State *lua, char const *script) {
	if (luaL_dostring(lua, script) != 0) {
		auto error = agi::lua::get_string_or_default(lua, -1);
		lua_pop(lua, 1);
		FAIL() << error;
	}
}
}

TEST(lua_re, invalid_pattern_reports_an_owned_error) {
	auto lua = MakeLuaState();
	ASSERT_TRUE(lua);
	RunLua(lua.get(), R"lua(
local ffi = require 'ffi'
ffi.cdef[[
void free(void *);
typedef struct agi_re_flag {
  const char *name;
  int value;
} agi_re_flag;
]]
local impl = require 'aegisub.__re_impl'
local err = ffi.new('char *[1]')
local regex = impl.compile('(', 0, err)
assert(regex == nil)
assert(err[0] ~= nil)
local message = ffi.string(err[0])
ffi.C.free(err[0])
assert(type(message) == 'string' and #message > 0)
)lua");
}

TEST(lua_re, invalid_utf8_is_trapped_at_each_native_boundary) {
	auto lua = MakeLuaState();
	ASSERT_TRUE(lua);
	RunLua(lua.get(), R"lua(
local ffi = require 'ffi'
ffi.cdef[[
void free(void *);
typedef struct agi_re_flag {
  const char *name;
  int value;
} agi_re_flag;
]]
local impl = require 'aegisub.__re_impl'
local err = ffi.new('char *[1]')
local regex = impl.compile('.', 0, err)
assert(regex ~= nil and err[0] == nil)

local invalid = string.char(255)
local calls = {
  {call = function() return impl.search(regex, invalid, #invalid, 0, err) end,
   cleanup = ffi.C.free},
  {call = function() return impl.match(regex, invalid, #invalid, 0, err) end,
   cleanup = impl.match_free},
  {call = function() return impl.replace(regex, 'x', invalid, #invalid, 1, err) end,
   cleanup = ffi.C.free},
}

for _, entry in ipairs(calls) do
  err[0] = nil
  local result = entry.call()
  if result ~= nil then entry.cleanup(result) end
  assert(err[0] ~= nil)
  local message = ffi.string(err[0])
  ffi.C.free(err[0])
  assert(message:find('UTF-8', 1, true))
end

impl.regex_free(regex)
)lua");
}

TEST(lua_re, public_module_reports_native_errors_as_lua_strings) {
	auto lua = MakeLuaState();
	ASSERT_TRUE(lua);
	RunLua(lua.get(), R"lua(
local re = require 'aegisub.re'

local ok, err = pcall(re.compile, '(')
assert(ok == false and type(err) == 'string')

ok, err = pcall(re.find, 'abc' .. string.char(255) .. 'def', 'd')
assert(ok == false)
assert(type(err) == 'string' and err:find('UTF-8', 1, true))

ok, err = pcall(re.match, string.char(255), 'x')
assert(ok == false)
assert(type(err) == 'string' and err:find('UTF-8', 1, true))
)lua");
}
