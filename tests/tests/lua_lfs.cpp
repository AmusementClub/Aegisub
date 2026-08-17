#include <main.h>

#include <libaegisub/fs.h>
#include <libaegisub/lua/modules.h>
#include <libaegisub/lua/script_reader.h>
#include <libaegisub/lua/utils.h>

#include <lua.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace {
constexpr char nonascii_dir[] = "data/lfs_M\xC3\xBCller";
constexpr char nonascii_file[] = "data/lfs_M\xC3\xBCller/file";

class LuaLfsTest : public ::testing::Test {
protected:
	lua_State *lua = nullptr;

	void SetUp() override {
		lua = luaL_newstate();
		ASSERT_NE(nullptr, lua);
		agi::lua::preload_modules(lua);
		std::vector<agi::fs::path> include_path{
			agi::fs::PathFromString(AEGISUB_PROJECT_SOURCE_DIR) / "automation" / "include"
		};
		ASSERT_TRUE(agi::lua::Install(lua, include_path));

		std::error_code ec;
		std::filesystem::remove_all(agi::fs::PathFromString(nonascii_dir), ec);
		ASSERT_NO_THROW(agi::fs::CreateDirectory(agi::fs::PathFromString(nonascii_dir)));
		ASSERT_NO_THROW(agi::fs::Touch(agi::fs::PathFromString(nonascii_file)));
	}

	void TearDown() override {
		if (lua)
			lua_close(lua);
		std::error_code ec;
		std::filesystem::remove_all(agi::fs::PathFromString(nonascii_dir), ec);
	}

	std::optional<std::string> GetMode(char const *path) {
		static constexpr char chunk[] = R"lua(
local path = ...
local ffi = require 'ffi'
ffi.cdef[[void free(void *);]]
local impl = require 'aegisub.__lfs_impl'
local err = ffi.new('char *[1]')
local mode = impl.get_mode(path, err)
if err[0] ~= nil then
  local message = ffi.string(err[0])
  ffi.C.free(err[0])
  error(message, 0)
end
if mode == nil then return false end
return ffi.string(mode)
)lua";

		if (luaL_loadstring(lua, chunk) != 0) {
			ADD_FAILURE() << lua_tostring(lua, -1);
			lua_pop(lua, 1);
			return std::nullopt;
		}
		lua_pushstring(lua, path);
		if (lua_pcall(lua, 1, 1, 0) != 0) {
			ADD_FAILURE() << lua_tostring(lua, -1);
			lua_pop(lua, 1);
			return std::nullopt;
		}

		std::optional<std::string> result;
		if (lua_type(lua, -1) == LUA_TSTRING)
			result = lua_tostring(lua, -1);
		lua_pop(lua, 1);
		return result;
	}

	void RunLua(char const *script) {
		if (luaL_dostring(lua, script) != 0) {
			auto error = agi::lua::get_string_or_default(lua, -1);
			lua_pop(lua, 1);
			FAIL() << error;
		}
	}

	std::pair<bool, std::string> OpenDirectory(char const *path) {
		static constexpr char chunk[] = R"lua(
local path = ...
local ffi = require 'ffi'
ffi.cdef[[void free(void *);]]
local impl = require 'aegisub.__lfs_impl'
local err = ffi.new('char *[1]')
local iter = impl.dir_new(path, err)
if err[0] ~= nil then
  local message = ffi.string(err[0])
  ffi.C.free(err[0])
  return false, message
end
impl.dir_free(iter)
return true
)lua";

		lua_settop(lua, 0);
		if (luaL_loadstring(lua, chunk) != 0) {
			ADD_FAILURE() << lua_tostring(lua, -1);
			lua_pop(lua, 1);
			return {false, "chunk load failed"};
		}
		lua_pushstring(lua, path);
		if (lua_pcall(lua, 1, 2, 0) != 0) {
			ADD_FAILURE() << lua_tostring(lua, -1);
			lua_pop(lua, 1);
			return {false, "chunk call failed"};
		}

		bool success = lua_toboolean(lua, -2) != 0;
		std::string error = lua_isstring(lua, -1) ? lua_tostring(lua, -1) : "";
		lua_pop(lua, 2);
		return {success, error};
	}
};
}

TEST_F(LuaLfsTest, attributes_handles_non_ascii_paths) {
	EXPECT_EQ(std::optional<std::string>{"directory"}, GetMode(nonascii_dir));
	EXPECT_EQ(std::optional<std::string>{"file"}, GetMode(nonascii_file));
}

TEST_F(LuaLfsTest, attributes_reports_missing_paths) {
	EXPECT_EQ(std::nullopt, GetMode("data/lfs_missing"));
}

TEST_F(LuaLfsTest, dir_rejects_missing_paths) {
	auto const [success, error] = OpenDirectory("data/lfs_missing");
	EXPECT_FALSE(success);
	EXPECT_NE(std::string::npos, error.find("cannot open"));
}

TEST_F(LuaLfsTest, dir_rejects_regular_files) {
	auto const [success, error] = OpenDirectory(nonascii_file);
	EXPECT_FALSE(success);
	EXPECT_NE(std::string::npos, error.find("cannot open"));
}

TEST_F(LuaLfsTest, public_module_preserves_lfs_results_and_errors) {
	lua_pushstring(lua, nonascii_dir);
	lua_setglobal(lua, "__test_dir");
	lua_pushstring(lua, nonascii_file);
	lua_setglobal(lua, "__test_file");
	lua_pushstring(lua, "data/lfs_missing");
	lua_setglobal(lua, "__test_missing");

	RunLua(R"lua(
local lfs = require 'aegisub.lfs'
assert(lfs.attributes(__test_dir, 'mode') == 'directory')
assert(lfs.attributes(__test_file, 'mode') == 'file')

local nested = __test_dir .. '/nested'
assert(lfs.mkdir(nested) == true)
assert(lfs.touch(nested .. '/file') == true)
assert(lfs.attributes(nested .. '/file', 'mode') == 'file')
os.remove(nested .. '/file')
assert(lfs.rmdir(nested) == true)

for _, path in ipairs({__test_missing, __test_file}) do
  local ok, err = pcall(lfs.dir, path)
  assert(ok == false)
  assert(type(err) == 'string' and err:find('cannot open', 1, true))
end
)lua");
}
