#include <libaegisub/fs.h>
#include <libaegisub/lua/modules.h>
#include <libaegisub/lua/script_reader.h>
#include <libaegisub/path.h>

#include <gtest/gtest.h>

#include "../../vendor/luajit/src/lua.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {
struct LuaStateCloser {
	void operator()(lua_State *L) const {
		if (L)
			lua_close(L);
	}
};

using LuaStatePtr = std::unique_ptr<lua_State, LuaStateCloser>;

LuaStatePtr CreateLuaState() {
	LuaStatePtr state(luaL_newstate());
	if (!state)
		return state;

	agi::lua::preload_modules(state.get());
	std::vector<agi::fs::path> include_path{
		std::filesystem::path(AEGISUB_PROJECT_SOURCE_DIR) / "automation" / "include"
	};
	if (!agi::lua::Install(state.get(), include_path))
		return LuaStatePtr();

	return state;
}

std::string GetPackageField(lua_State *L, char const* field) {
	lua_getglobal(L, "package");
	lua_getfield(L, -1, field);
	size_t len = 0;
	char const* value = lua_tolstring(L, -1, &len);
	std::string result(value ? value : "", len);
	lua_pop(L, 2);
	return result;
}

void SetPackageField(lua_State *L, char const* field, std::string const& value) {
	lua_getglobal(L, "package");
	lua_pushlstring(L, value.c_str(), value.size());
	lua_setfield(L, -2, field);
	lua_pop(L, 1);
}

std::pair<std::string, bool> CallFfiLoad(lua_State *L, std::string const& name, bool global) {
	lua_settop(L, 0);
	lua_pushlstring(L, name.c_str(), name.size());
	lua_setglobal(L, "__test_name");
	lua_pushboolean(L, global);
	lua_setglobal(L, "__test_global");

	auto status = luaL_dostring(L, R"lua(
local ffi = require('ffi')
ffi.__aegisub_original_load = function(resolved_name, resolved_global)
  return resolved_name, resolved_global
end
return ffi.load(__test_name, __test_global)
)lua");
	EXPECT_EQ(0, status) << (lua_isstring(L, -1) ? lua_tostring(L, -1) : "luaL_dostring failed");
	if (status != 0)
		return {};

	size_t len = 0;
	char const* resolved_name = lua_tolstring(L, -2, &len);
	bool resolved_global = !!lua_toboolean(L, -1);
	std::string result(resolved_name ? resolved_name : "", len);
	lua_settop(L, 0);
	return { result, resolved_global };
}

struct TempDirectory {
	std::filesystem::path path;

	TempDirectory() {
		auto unique = std::to_string(
			static_cast<long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
		path = std::filesystem::temp_directory_path()
			/ std::filesystem::path("aegisub-lua-script-reader-test-" + unique);
		std::filesystem::create_directories(path);
	}

	~TempDirectory() {
		std::error_code ec;
		std::filesystem::remove_all(path, ec);
	}
};

void TouchFile(std::filesystem::path const& path) {
	std::filesystem::create_directories(path.parent_path());
	std::ofstream file(path, std::ios::binary);
	file << "x";
}
}

TEST(lua_script_reader, install_prepends_runtime_directories_to_package_cpath) {
#ifndef _WIN32
	GTEST_SKIP();
#else
	auto state = CreateLuaState();
	ASSERT_TRUE(state);

	agi::Path path;
	auto const expected_prefix =
		(agi::fs::PathToString(path.Decode("?user/runtimes")) + "/?.dll;" +
		 agi::fs::PathToString(path.Decode("?data/runtimes")) + "/?.dll;");

	auto const cpath = GetPackageField(state.get(), "cpath");
	EXPECT_EQ(0u, cpath.rfind(expected_prefix, 0));
#endif
}

TEST(lua_script_reader, ffi_load_resolves_bare_library_names_using_package_cpath) {
#ifndef _WIN32
	GTEST_SKIP();
#else
	auto state = CreateLuaState();
	ASSERT_TRUE(state);

	TempDirectory temp;
	auto library = temp.path / "sample.dll";
	TouchFile(library);

	auto const current_cpath = GetPackageField(state.get(), "cpath");
	SetPackageField(state.get(), "cpath", library.parent_path().generic_string() + "/?.dll;" + current_cpath);

	auto const [resolved_name, resolved_global] = CallFfiLoad(state.get(), "sample", true);
	EXPECT_EQ(library.generic_string(), std::filesystem::path(resolved_name).generic_string());
	EXPECT_TRUE(resolved_global);

	auto const [resolved_suffixed_name, resolved_suffixed_global] = CallFfiLoad(state.get(), "sample.dll", false);
	EXPECT_EQ(library.generic_string(), std::filesystem::path(resolved_suffixed_name).generic_string());
	EXPECT_FALSE(resolved_suffixed_global);
#endif
}

TEST(lua_script_reader, ffi_load_preserves_explicit_paths) {
#ifndef _WIN32
	GTEST_SKIP();
#else
	auto state = CreateLuaState();
	ASSERT_TRUE(state);

	auto const [resolved_name, resolved_global] = CallFfiLoad(state.get(), "subdir\\sample.dll", true);
	EXPECT_EQ("subdir\\sample.dll", resolved_name);
	EXPECT_TRUE(resolved_global);
#endif
}
