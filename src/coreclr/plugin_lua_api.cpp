#include "plugin_lua_api.h"

#include "dotnet_automation_engine.h"

#include <libaegisub/cajun/elements.h>
#include <libaegisub/cajun/reader.h>
#include <libaegisub/cajun/visitor.h>
#include <libaegisub/cajun/writer.h>
#include <libaegisub/lua/utils.h>
#include <libaegisub/scope_exit.h>

#include <lua.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace Automation4 {
namespace {

constexpr size_t kMaxTransportNodes = 4096;
constexpr size_t kMaxTransportDepth = 32;
constexpr size_t kMaxTransportStringBytes = 1024 * 1024;
constexpr size_t kMaxTransportArrayLength = 4096;

int AbsoluteIndex(lua_State *L, int index) {
	if (index > 0 || index <= LUA_REGISTRYINDEX)
		return index;
	return lua_gettop(L) + index + 1;
}

struct LuaJsonReadState {
	size_t nodes = 0;
	size_t string_bytes = 0;
	std::set<void const*> active_tables;
};

bool IsValidUtf8(std::string_view value) {
	auto continuation = [](unsigned char byte) {
		return byte >= 0x80 && byte <= 0xBF;
	};
	for (size_t index = 0; index < value.size();) {
		auto lead = static_cast<unsigned char>(value[index]);
		if (lead <= 0x7F) {
			++index;
			continue;
		}
		if (lead >= 0xC2 && lead <= 0xDF) {
			if (index + 1 >= value.size() ||
				!continuation(static_cast<unsigned char>(value[index + 1])))
				return false;
			index += 2;
			continue;
		}
		if (lead >= 0xE0 && lead <= 0xEF) {
			if (index + 2 >= value.size())
				return false;
			auto second = static_cast<unsigned char>(value[index + 1]);
			auto third = static_cast<unsigned char>(value[index + 2]);
			if (!continuation(third) ||
				(lead == 0xE0 ? second < 0xA0 || second > 0xBF :
				 lead == 0xED ? second < 0x80 || second > 0x9F :
				 !continuation(second)))
				return false;
			index += 3;
			continue;
		}
		if (lead >= 0xF0 && lead <= 0xF4) {
			if (index + 3 >= value.size())
				return false;
			auto second = static_cast<unsigned char>(value[index + 1]);
			if ((lead == 0xF0 ? second < 0x90 || second > 0xBF :
				 lead == 0xF4 ? second < 0x80 || second > 0x8F :
				 !continuation(second)) ||
				!continuation(static_cast<unsigned char>(value[index + 2])) ||
				!continuation(static_cast<unsigned char>(value[index + 3])))
				return false;
			index += 4;
			continue;
		}
		return false;
	}
	return true;
}

void CountStringBytes(LuaJsonReadState& state, size_t length) {
	if (length > kMaxTransportStringBytes - state.string_bytes)
		throw std::runtime_error(
			"Plugin service transport strings exceed the size limit");
	state.string_bytes += length;
}

json::UnknownElement ReadLuaJsonValue(
	lua_State *L,
	int index,
	LuaJsonReadState& state,
	size_t depth) {
	if (++state.nodes > kMaxTransportNodes)
		throw std::runtime_error(
			"Plugin service transport request exceeds the node limit");
	if (depth > kMaxTransportDepth)
		throw std::runtime_error(
			"Plugin service transport request exceeds the nesting limit");

	index = AbsoluteIndex(L, index);
	switch (lua_type(L, index)) {
		case LUA_TNIL:
			return json::Null{};
		case LUA_TBOOLEAN:
			return lua_toboolean(L, index) != 0;
		case LUA_TNUMBER: {
			double value = static_cast<double>(lua_tonumber(L, index));
			if (!std::isfinite(value))
				throw std::runtime_error(
					"Plugin service transport numbers must be finite");
			double integer_part = 0.0;
			if (std::modf(value, &integer_part) == 0.0 &&
				integer_part >= static_cast<double>(std::numeric_limits<int64_t>::min()) &&
				integer_part < 9223372036854775808.0)
				return static_cast<int64_t>(integer_part);
			return value;
		}
		case LUA_TSTRING: {
			size_t length = 0;
			auto const* value = lua_tolstring(L, index, &length);
			CountStringBytes(state, length);
			if (!IsValidUtf8(std::string_view(value, length)))
				throw std::runtime_error(
					"Plugin service transport strings must contain valid UTF-8");
			return std::string(value, length);
		}
		case LUA_TTABLE:
			break;
		default:
			throw std::runtime_error(
				"Plugin service transport supports only nil, boolean, number, string, and table values");
	}

	auto const* identity = lua_topointer(L, index);
	if (!state.active_tables.insert(identity).second)
		throw std::runtime_error(
			"Plugin service transport does not accept cyclic tables");
	auto remove_active = agi::make_scope_exit([&] { state.active_tables.erase(identity); });

	bool has_array_keys = false;
	bool has_object_keys = false;
	size_t array_count = 0;
	size_t maximum_array_index = 0;
	lua_pushnil(L);
	while (lua_next(L, index) != 0) {
		auto pop_value = agi::make_scope_exit([&] { lua_pop(L, 1); });
		if (lua_type(L, -2) == LUA_TNUMBER) {
			double key = static_cast<double>(lua_tonumber(L, -2));
			double integer_part = 0.0;
			if (!std::isfinite(key) || std::modf(key, &integer_part) != 0.0 ||
				integer_part < 1.0 || integer_part > kMaxTransportArrayLength)
				throw std::runtime_error(
					"Plugin service transport array keys must be contiguous positive integers");
			has_array_keys = true;
			++array_count;
			maximum_array_index = std::max(
				maximum_array_index,
				static_cast<size_t>(integer_part));
		}
		else if (lua_type(L, -2) == LUA_TSTRING) {
			has_object_keys = true;
		}
		else {
			throw std::runtime_error(
				"Plugin service transport table keys must be strings or positive integers");
		}
	}
	if (has_array_keys && has_object_keys)
		throw std::runtime_error(
			"Plugin service transport does not accept mixed array and object keys");

	if (has_array_keys) {
		if (array_count != maximum_array_index)
			throw std::runtime_error(
				"Plugin service transport arrays cannot contain holes");
		json::Array array;
		array.reserve(array_count);
		for (size_t item = 1; item <= array_count; ++item) {
			lua_rawgeti(L, index, static_cast<int>(item));
			auto pop_item = agi::make_scope_exit([&] { lua_pop(L, 1); });
			array.emplace_back(ReadLuaJsonValue(L, -1, state, depth + 1));
		}
		return array;
	}

	json::Object object;
	lua_pushnil(L);
	while (lua_next(L, index) != 0) {
		auto pop_value = agi::make_scope_exit([&] { lua_pop(L, 1); });
		size_t key_length = 0;
		auto const* key_value = lua_tolstring(L, -2, &key_length);
		CountStringBytes(state, key_length);
		std::string key(key_value, key_length);
		if (key.find('\0') != std::string::npos)
			throw std::runtime_error(
				"Plugin service transport object keys cannot contain NUL characters");
		if (!IsValidUtf8(key))
			throw std::runtime_error(
				"Plugin service transport object keys must contain valid UTF-8");
		object.emplace(
			std::move(key),
			ReadLuaJsonValue(L, -1, state, depth + 1));
	}
	return object;
}

class LuaJsonWriter final : public json::ConstVisitor {
	lua_State *L;
	size_t nodes = 0;
	size_t string_bytes = 0;
	size_t depth = 0;

	void Push(json::UnknownElement const& value) {
		if (++nodes > kMaxTransportNodes)
			throw std::runtime_error(
				"Plugin service transport response exceeds the node limit");
		if (depth > kMaxTransportDepth)
			throw std::runtime_error(
				"Plugin service transport response exceeds the nesting limit");
		value.Accept(*this);
	}

	void CountResponseString(size_t length) {
		if (length > kMaxTransportStringBytes - string_bytes)
			throw std::runtime_error(
				"Plugin service transport response strings exceed the size limit");
		string_bytes += length;
	}

public:
	explicit LuaJsonWriter(lua_State *L) : L(L) { }

	void Write(json::UnknownElement const& value) { Push(value); }

	void Visit(json::Array const& array) override {
		if (array.size() > kMaxTransportArrayLength)
			throw std::runtime_error(
				"Plugin service transport response array exceeds the size limit");
		lua_createtable(L, static_cast<int>(array.size()), 0);
		++depth;
		auto restore_depth = agi::make_scope_exit([&] { --depth; });
		for (size_t index = 0; index < array.size(); ++index) {
			Push(array[index]);
			lua_rawseti(L, -2, static_cast<int>(index + 1));
		}
	}

	void Visit(json::Object const& object) override {
		lua_createtable(L, 0, static_cast<int>(object.size()));
		++depth;
		auto restore_depth = agi::make_scope_exit([&] { --depth; });
		for (auto const& [key, value] : object) {
			CountResponseString(key.size());
			lua_pushlstring(L, key.data(), key.size());
			Push(value);
			lua_rawset(L, -3);
		}
	}

	void Visit(int64_t number) override {
		lua_pushnumber(L, static_cast<lua_Number>(number));
	}

	void Visit(double number) override {
		if (!std::isfinite(number))
			throw std::runtime_error(
				"Plugin service transport response contains a non-finite number");
		lua_pushnumber(L, static_cast<lua_Number>(number));
	}

	void Visit(std::string const& string) override {
		CountResponseString(string.size());
		lua_pushlstring(L, string.data(), string.size());
	}

	void Visit(bool boolean) override { lua_pushboolean(L, boolean); }
	void Visit(json::Null const&) override { lua_pushnil(L); }
};

std::string RequireTransportString(
	lua_State *L,
	int index,
	char const* field_name,
	size_t maximum_length) {
	if (!lua_isstring(L, index))
		throw std::runtime_error(
			std::string("Plugin service ") + field_name + " must be a string");
	size_t length = 0;
	auto const* value = lua_tolstring(L, index, &length);
	std::string result(value, length);
	if (result.empty() || result.size() > maximum_length)
		throw std::runtime_error(
			std::string("Plugin service ") + field_name +
			" is empty or too long");
	if (result.find('\0') != std::string::npos || !IsValidUtf8(result))
		throw std::runtime_error(
			std::string("Plugin service ") + field_name +
			" must contain valid UTF-8 without NUL characters");
	return result;
}

std::string ReadOptionalRequestJson(lua_State *L, int index) {
	if (lua_gettop(L) < index || lua_isnil(L, index))
		return "{}";
	LuaJsonReadState state;
	auto request = ReadLuaJsonValue(L, index, state, 0);
	std::ostringstream stream;
	agi::JsonWriter::Write(request, stream);
	return std::move(stream).str();
}

/// aegisub.plugin.invoke(contribution_id, operation_id [, request_table])
int LuaPluginServiceInvoke(lua_State *L) {
	auto contribution_id = RequireTransportString(L, 1, "contribution id", 512);
	auto operation = RequireTransportString(L, 2, "operation id", 256);
	auto request_json = ReadOptionalRequestJson(L, 3);
	auto response_json = InvokePluginServiceContribution(
		contribution_id, operation, request_json);
	if (response_json.size() > kMaxTransportStringBytes)
		throw std::runtime_error(
			"Plugin service response exceeds the size limit");
	std::istringstream stream(response_json);
	json::UnknownElement response;
	json::Reader::Read(response, stream);
	LuaJsonWriter(L).Write(response);
	return 1;
}

} // namespace

// Last-resort loader: require("pkg.Export") may resolve as require("pkg").Export
// when no file exists for the full name. Real path modules still win because this
// searcher is installed after the file loader. Enables single-file modules that
// re-export Logger/Updater-style fields without per-export shim files.
constexpr char kFieldExportSearcher[] = R"lua(
do
  local loading = {}
  local function field_export_searcher(name)
    local parent, field = name:match("^(.*)%.([^%.]+)$")
    if not parent or loading[name] then
      return nil
    end
    return function()
      loading[name] = true
      local ok, mod_or_err = pcall(require, parent)
      loading[name] = nil
      if not ok then
        error(mod_or_err, 0)
      end
      if type(mod_or_err) ~= "table" or mod_or_err[field] == nil then
        error(string.format("module '%s' not found", name), 0)
      end
      return mod_or_err[field]
    end
  end
  local loaders = package.loaders or package.searchers
  loaders[#loaders + 1] = field_export_searcher
end
)lua";

void RegisterPluginLuaApi(lua_State *L) {
	lua_getglobal(L, "aegisub");
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		throw std::logic_error(
			"The Aegisub Lua table must exist before registering plugin Lua APIs");
	}

	lua_newtable(L);
	agi::lua::push_value(
		L,
		agi::lua::exception_wrapper<LuaPluginServiceInvoke>);
	lua_setfield(L, -2, "invoke");
	lua_setfield(L, -2, "plugin");
	lua_pop(L, 1);

	if (luaL_dostring(L, kFieldExportSearcher) != 0) {
		auto message = agi::lua::get_string_or_default(L, -1);
		lua_pop(L, 1);
		throw std::runtime_error(
			"Could not install plugin field-export module searcher: " + message);
	}
}

} // namespace Automation4
