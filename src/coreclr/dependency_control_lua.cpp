#include "dependency_control_lua.h"

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
#include <cstring>
#include <cstdint>
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
			"DependencyControl transport strings exceed the size limit");
	state.string_bytes += length;
}

json::UnknownElement ReadLuaJsonValue(
	lua_State *L,
	int index,
	LuaJsonReadState& state,
	size_t depth) {
	if (++state.nodes > kMaxTransportNodes)
		throw std::runtime_error(
			"DependencyControl transport request exceeds the node limit");
	if (depth > kMaxTransportDepth)
		throw std::runtime_error(
			"DependencyControl transport request exceeds the nesting limit");

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
					"DependencyControl transport numbers must be finite");
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
					"DependencyControl transport strings must contain valid UTF-8");
			return std::string(value, length);
		}
		case LUA_TTABLE:
			break;
		default:
			throw std::runtime_error(
				"DependencyControl transport supports only nil, boolean, number, string, and table values");
	}

	auto const* identity = lua_topointer(L, index);
	if (!state.active_tables.insert(identity).second)
		throw std::runtime_error(
			"DependencyControl transport does not accept cyclic tables");
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
					"DependencyControl transport array keys must be contiguous positive integers");
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
				"DependencyControl transport table keys must be strings or positive integers");
		}
	}
	if (has_array_keys && has_object_keys)
		throw std::runtime_error(
			"DependencyControl transport does not accept mixed array and object keys");

	if (has_array_keys) {
		if (array_count != maximum_array_index)
			throw std::runtime_error(
				"DependencyControl transport arrays cannot contain holes");
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
				"DependencyControl transport object keys cannot contain NUL characters");
		if (!IsValidUtf8(key))
			throw std::runtime_error(
				"DependencyControl transport object keys must contain valid UTF-8");
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
				"DependencyControl transport response exceeds the node limit");
		if (depth > kMaxTransportDepth)
			throw std::runtime_error(
				"DependencyControl transport response exceeds the nesting limit");
		value.Accept(*this);
	}

	void CountResponseString(size_t length) {
		if (length > kMaxTransportStringBytes - string_bytes)
			throw std::runtime_error(
				"DependencyControl transport response strings exceed the size limit");
		string_bytes += length;
	}

public:
	explicit LuaJsonWriter(lua_State *L) : L(L) { }

	void Write(json::UnknownElement const& value) { Push(value); }

	void Visit(json::Array const& array) override {
		if (array.size() > kMaxTransportArrayLength)
			throw std::runtime_error(
				"DependencyControl transport response array exceeds the size limit");
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
				"DependencyControl transport response contains a non-finite number");
		lua_pushnumber(L, static_cast<lua_Number>(number));
	}

	void Visit(std::string const& string) override {
		CountResponseString(string.size());
		lua_pushlstring(L, string.data(), string.size());
	}

	void Visit(bool boolean) override { lua_pushboolean(L, boolean); }
	void Visit(json::Null const&) override { lua_pushnil(L); }
};

int LuaDependencyControlCall(lua_State *L) {
	if (!lua_isstring(L, 1))
		throw std::runtime_error(
			"DependencyControl service operation ID must be a string");
	size_t operation_length = 0;
	auto const* operation_value = lua_tolstring(L, 1, &operation_length);
	std::string operation(operation_value, operation_length);
	if (operation.empty() || operation.size() > 256)
		throw std::runtime_error(
			"DependencyControl service operation ID is empty or too long");
	if (operation.find('\0') != std::string::npos || !IsValidUtf8(operation))
		throw std::runtime_error(
			"DependencyControl service operation ID must contain valid UTF-8 without NUL characters");

	std::string request_json = "{}";
	if (lua_gettop(L) >= 2 && !lua_isnil(L, 2)) {
		LuaJsonReadState state;
		auto request = ReadLuaJsonValue(L, 2, state, 0);
		std::ostringstream stream;
		agi::JsonWriter::Write(request, stream);
		request_json = std::move(stream).str();
	}

	auto response_json = InvokeDependencyControlService(operation, request_json);
	if (response_json.size() > kMaxTransportStringBytes)
		throw std::runtime_error(
			"DependencyControl service response exceeds the size limit");
	std::istringstream stream(response_json);
	json::UnknownElement response;
	json::Reader::Read(response, stream);
	LuaJsonWriter(L).Write(response);
	return 1;
}

constexpr std::string_view kDependencyControlFacade = R"lua(
local native_call = assert(aegisub.__dependency_control_call)
local probe = native_call("compatibility.probe", {})
local loaded_modules = {}

local function parse_version_parts(value)
  if type(value) == "table" and value.version ~= nil then value = value.version end
  if type(value) == "number" then
    assert(value >= 0 and value == math.floor(value),
      "DependencyControl numeric versions must be non-negative integers")
    return {
      math.floor(value / 65536) % 256,
      math.floor(value / 256) % 256,
      value % 256
    }
  end
  assert(type(value) == "string", "DependencyControl versions must be strings or numbers")
  value = value:match("^%s*(.-)%s*$")
  if value:sub(1, 1) == "v" or value:sub(1, 1) == "V" then value = value:sub(2) end
  local core, suffix = value:match("^([0-9][0-9%.]*)(.*)$")
  assert(core and core:sub(-1) ~= "." and not core:find("..", 1, true),
    "DependencyControl version is invalid: " .. tostring(value))
  assert(suffix == "" or suffix:sub(1, 1) == "-" or suffix:sub(1, 1) == "+",
    "DependencyControl version suffix is invalid: " .. tostring(value))
  local result = {}
  for part in core:gmatch("[^.]+") do
    assert(#result < 3, "DependencyControl versions support major, minor, and patch fields")
    result[#result + 1] = assert(tonumber(part))
  end
  while #result < 3 do result[#result + 1] = 0 end
  return result
end

local function version_string(value, precision)
  local parts = parse_version_parts(value)
  local count = ({ major = 1, minor = 2, patch = 3 })[precision or "patch"]
  assert(count, "DependencyControl version precision must be major, minor, or patch")
  local result = {}
  for index = 1, count do result[index] = tostring(parts[index]) end
  return table.concat(result, ".")
end

local Logger = {}
Logger.__index = Logger
Logger.__name = "Logger"

setmetatable(Logger, {
  __call = function(_, args)
    local logger = setmetatable({
      defaultLevel = 2,
      maxToFileLevel = 5,
      prefix = "",
      indent = 0,
      indentStr = "—",
      toWindow = true,
      toFile = false
    }, Logger)
    if args ~= nil then
      assert(type(args) == "table", "DependencyControl Logger options must be a table")
      for key, value in pairs(args) do logger[key] = value end
      if args.usePrefix ~= nil then
        logger.usePrefixWindow = not not args.usePrefix
        logger.usePrefixFile = not not args.usePrefix
      end
    end
    if logger.usePrefixWindow == nil then logger.usePrefixWindow = true end
    if logger.usePrefixFile == nil then logger.usePrefixFile = true end
    return logger
  end
})

function Logger:format(message, indent, ...)
  if type(message) == "table" then message = table.concat(message, "\n") end
  message = tostring(message or "")
  if select("#", ...) > 0 then message = string.format(message, ...) end
  indent = indent or 0
  if indent <= 0 then return message end
  local indentation = string.rep(self.indentStr or "—", indent) .. " "
  return indentation .. message:gsub("\n", "\n" .. indentation)
end

function Logger:logEx(level, message, insert_line_feed, prefix, indent, ...)
  level = level == nil and self.defaultLevel or level
  insert_line_feed = insert_line_feed ~= false
  prefix = prefix == nil and self.prefix or prefix
  indent = indent == nil and self.indent or indent
  local text = self:format(message, indent, ...)
  if self.usePrefixWindow and prefix and prefix ~= "" then text = prefix .. text end
  if self.toFile and level <= (self.maxToFileLevel or 5) then
    local success, error_message = pcall(native_call, "logs.append", {
      level = level,
      source = (self.fileBaseName or script_namespace or "DependencyControl") ..
        (self.fileSubName or ""),
      message = text
    })
    if not success then
      self.toFile = false
      if self.toWindow and aegisub.log then
        aegisub.log(2, "DependencyControl file logging disabled: " ..
          tostring(error_message) .. "\n")
      end
    end
  end
  if level <= 1 then error("Error: " .. text, 2) end
  if self.toWindow and aegisub.log then
    aegisub.log(level, text .. (insert_line_feed and "\n" or ""))
  end
  return text ~= ""
end

function Logger:log(level, message, ...)
  if type(level) == "number" then
    return self:logEx(level, message, true, nil, nil, ...)
  end
  if message == nil and select("#", ...) == 0 then
    return self:logEx(self.defaultLevel, level, true)
  end
  return self:logEx(self.defaultLevel, level, true, nil, nil, message, ...)
end

function Logger:fatal(...) return self:log(0, ...) end
function Logger:error(...) return self:log(1, ...) end
function Logger:warn(...) return self:log(2, ...) end
function Logger:hint(...) return self:log(3, ...) end
function Logger:debug(...) return self:log(4, ...) end
function Logger:trace(...) return self:log(5, ...) end

function Logger:assert(condition, ...)
  if condition then return condition end
  return self:error(...)
end

function Logger:dumpToString(value, ignored_key)
  if type(value) ~= "table" then return tostring(value) end
  local seen = {}
  local next_id = 0
  local function dump(item, indentation)
    if type(item) ~= "table" then
      return type(item) == "string" and string.format("%q", item) or tostring(item)
    end
    if seen[item] then return "@" .. seen[item] end
    next_id = next_id + 1
    seen[item] = next_id
    local lines = { "{ @" .. next_id }
    for key, child in pairs(item) do
      if key ~= ignored_key then
        lines[#lines + 1] = indentation .. tostring(key) .. ": " ..
          dump(child, indentation .. "    ")
      end
    end
    lines[#lines + 1] = indentation:sub(1, math.max(0, #indentation - 4)) .. "}"
    return table.concat(lines, "\n")
  end
  return dump(value, "    "):gsub("%%", "%%%%")
end

function Logger:dump(value, ignored_key, level)
  return self:log(level or self.defaultLevel, self:dumpToString(value, ignored_key))
end

function Logger:progress(progress, message, ...)
  if progress and aegisub.progress and aegisub.progress.set then
    aegisub.progress.set(progress)
  end
  if message and message ~= "" and aegisub.progress and aegisub.progress.task then
    aegisub.progress.task(string.format(message, ...))
  end
  return true
end

function Logger:trimFiles(doWipe, maxAge, maxSize, maxFiles)
  local response = native_call("logs.trim", {
    wipe = not not doWipe,
    maximumAgeSeconds = maxAge or 604800,
    maximumBytes = maxSize or 100 * 10^6,
    maximumFiles = maxFiles or 200
  })
  return response.deletedFiles or 0,
    response.deletedBytes or 0,
    response.totalFiles or 0,
    response.totalBytes or 0
end

local function deep_copy(value, seen, skip_private)
  if type(value) ~= "table" then return value end
  seen = seen or {}
  if seen[value] then return seen[value] end
  local result = {}
  seen[value] = result
  for key, child in pairs(value) do
    if not (skip_private and type(key) == "string" and key:sub(1, 1) == "_") then
      result[deep_copy(key, seen, skip_private)] = deep_copy(child, seen, skip_private)
    end
  end
  return result
end

local function value_at(root, path)
  local current = root
  for index = 1, #path do
    if type(current) ~= "table" then return nil, false end
    current = current[path[index]]
    if current == nil then return nil, false end
  end
  return current, true
end

local function effective_config_value(handler, path)
  local value, found = value_at(handler.userConfig, path)
  if found then return value, true end
  return value_at(handler.defaults, path)
end

local function ensure_user_config_path(handler, path)
  handler.userConfig = handler.userConfig or {}
  local current = handler.userConfig
  local prefix = {}
  for index = 1, #path do
    local key = path[index]
    prefix[index] = key
    if type(current[key]) ~= "table" then
      local default_value = value_at(handler.defaults, prefix)
      current[key] = type(default_value) == "table" and deep_copy(default_value) or {}
    end
    current = current[key]
  end
  return current
end

local function config_proxy(handler, path)
  path = path or {}
  return setmetatable({}, {
    __index = function(_, key)
      local child_path = {}
      for index = 1, #path do child_path[index] = path[index] end
      child_path[#child_path + 1] = key
      local value, found = effective_config_value(handler, child_path)
      if not found then return nil end
      if type(value) == "table" then return config_proxy(handler, child_path) end
      return value
    end,
    __newindex = function(_, key, value)
      local parent = ensure_user_config_path(handler, path)
      parent[key] = deep_copy(value)
    end,
    __len = function()
      local value = effective_config_value(handler, path)
      return type(value) == "table" and #value or 0
    end,
    __pairs = function()
      local defaults = value_at(handler.defaults, path)
      local user = value_at(handler.userConfig, path)
      local merged = type(defaults) == "table" and deep_copy(defaults) or {}
      if type(user) == "table" then
        for key, value in pairs(user) do merged[key] = deep_copy(value) end
      end
      return next, merged, nil
    end
  })
end

local ConfigHandler = { handlers = {} }
ConfigHandler.__index = ConfigHandler
ConfigHandler.__name = "ConfigHandler"

setmetatable(ConfigHandler, {
  __call = function(_, file_name, defaults, section, no_load, logger)
    local handler = setmetatable({}, ConfigHandler)
    handler.defaults = deep_copy(defaults or {})
    handler.section = type(section) == "table" and deep_copy(section) or
      (section == nil and {} or { section })
    handler.logger = logger or Logger { fileBaseName = "ConfigHandler" }
    handler.userConfig = {}
    handler.config = config_proxy(handler)
    handler.c = handler.config
    handler:setFile(file_name)
    if not no_load then handler:load() end
    return handler
  end
})

function ConfigHandler:setFile(file_name)
  if file_name == nil then return false end
  assert(type(file_name) == "string", "DependencyControl config file name must be a string")
  if self.file and ConfigHandler.handlers[self.file] then
    local retained = {}
    for _, handler in ipairs(ConfigHandler.handlers[self.file]) do
      if handler ~= self then retained[#retained + 1] = handler end
    end
    ConfigHandler.handlers[self.file] = #retained > 0 and retained or nil
  end
  self.file = file_name
  ConfigHandler.handlers[file_name] = ConfigHandler.handlers[file_name] or {}
  ConfigHandler.handlers[file_name][#ConfigHandler.handlers[file_name] + 1] = self
  return true
end

function ConfigHandler:unsetFile()
  if self.file == nil then return true end
  local file_name = self.file
  self.file = nil
  local handlers = ConfigHandler.handlers[file_name] or {}
  local retained = {}
  for _, handler in ipairs(handlers) do
    if handler ~= self then retained[#retained + 1] = handler end
  end
  ConfigHandler.handlers[file_name] = #retained > 0 and retained or nil
  return true
end

function ConfigHandler:readFile(file_name)
  file_name = file_name or self.file
  if not file_name then return false, "No config file defined." end
  local request = { fileName = file_name }
  if #self.section > 0 then request.section = self.section end
  local success, response = pcall(native_call, "config.read", request)
  if not success then return false, tostring(response) end
  if response.corrupted then
    return false, "Configuration was corrupted and moved to '" ..
      tostring(response.backupName or "<backup>") .. "'."
  end
  if not response.exists then return nil end
  return response.value
end

function ConfigHandler:load()
  if not self.file then return false, "No config file defined." end
  local value, message = self:readFile()
  if value == false or value == nil then return value, message end
  self.userConfig = deep_copy(value)
  return true
end

function ConfigHandler:write()
  if not self.file then return false, "No config file defined." end
  local request = {
    fileName = self.file,
    value = self.userConfig or {}
  }
  if #self.section > 0 then request.section = self.section end
  local success, response = pcall(native_call, "config.write", request)
  if not success then return false, tostring(response) end
  return response.written == true
end

function ConfigHandler:delete()
  if not self.file then return false, "No config file defined." end
  local request = { fileName = self.file }
  if #self.section > 0 then request.section = self.section end
  local success, response = pcall(native_call, "config.delete", request)
  if not success then return false, tostring(response) end
  self.userConfig = nil
  return response.deleted == true
end

function ConfigHandler:getSectionHandler(section, defaults, no_load)
  return ConfigHandler(self.file, defaults, section, no_load, self.logger)
end

function ConfigHandler:deepCopy(value)
  return deep_copy(value, nil, true)
end

function ConfigHandler:import(value, keys, update_only, skip_same_length_tables)
  if type(value) == "table" and getmetatable(value) == ConfigHandler then
    value = value.userConfig
  end
  assert(type(value) == "table", "DependencyControl configuration import requires a table")
  local selected = nil
  if keys then
    selected = {}
    for _, key in ipairs(keys) do selected[key] = true end
  end
  self.userConfig = self.userConfig or {}
  local changed = false
  for key, child in pairs(value) do
    local is_private = type(key) == "string" and key:sub(1, 1) == "_"
    local current = self.userConfig[key]
    local skip_same = type(child) == "table" and skip_same_length_tables and
      type(current) == "table" and #child == #current
    if not is_private and (not selected or selected[key]) and not skip_same and
        (not update_only or self.config[key] ~= nil) and current ~= child then
      self.userConfig[key] = deep_copy(child, nil, true)
      changed = true
    end
  end
  return changed
end

function ConfigHandler:mergeSection(config)
  assert(type(config) == "table", "DependencyControl configuration merge requires a table")
  local target = config
  for _, key in ipairs(self.section) do
    if target[key] == nil then target[key] = {} end
    if type(target[key]) ~= "table" then
      return false, "Configuration section crosses a non-table value."
    end
    target = target[key]
  end
  if self.userConfig == nil then
    return config
  end
  for key, value in pairs(self.userConfig) do target[key] = deep_copy(value) end
  return config
end

function ConfigHandler:getLock()
  self.hasLock = true
  return 0
end

function ConfigHandler:releaseLock()
  local had_lock = self.hasLock
  self.hasLock = false
  if had_lock then return true end
  return false, "ConfigHandler doesn't have a lock"
end

local function normalized_requirement(value)
  if type(value) == "string" then
    return { moduleName = value }
  end
  assert(type(value) == "table", "DependencyControl module requirements must be strings or tables")
  local module_name = value.moduleName or value[1]
  assert(type(module_name) == "string" and module_name ~= "",
    "DependencyControl module requirements need a module name")
  local result = { moduleName = module_name }
  if value.version ~= nil then result.version = tostring(value.version) end
  if value.feed ~= nil then result.feed = tostring(value.feed) end
  if value.channel ~= nil then result.channel = tostring(value.channel) end
  if value.optional ~= nil then result.optional = not not value.optional end
  return result
end

local function try_require(module_name)
  local cached = loaded_modules[module_name]
  if cached ~= nil then return true, cached end
  local success, module = pcall(require, module_name)
  if success then
    loaded_modules[module_name] = module
    return true, module
  end
  local partial = loaded_modules[module_name]
  if type(partial) == "table" and partial.__depCtrlDummy then
    loaded_modules[module_name] = nil
  end
  package.loaded[module_name] = nil
  return false, module
end

local Record = {}
Record.__index = Record

function Record:requireModules(modules)
  modules = modules or self.requiredModules or {}
  local count = #modules
  if count == 0 then return end
  local loaded = {}
  local failures = {}
  local missing_required = {}
  self._moduleStates = {}
  for index = 1, count do
    local requirement = normalized_requirement(modules[index])
    local success, module = try_require(requirement.moduleName)
    self._moduleStates[index] = {
      requirement = requirement,
      loaded = success,
      value = success and module or nil,
      error = success and nil or module
    }
    if success then
      loaded[index] = module
    else
      failures[index] = module
      if not requirement.optional then
        missing_required[#missing_required + 1] = requirement
      end
    end
  end

  if #missing_required > 0 then
    local resolution = native_call("module.ensure", {
      recordType = self.moduleName and "module" or "automation",
      namespace = self.namespace,
      requiredModules = missing_required
    })
    local automation_root = resolution.automationRoot
    if type(automation_root) == "string" and automation_root ~= "" then
      automation_root = automation_root:gsub("\\", "/")
      local include_path = automation_root .. "/include/?.lua;" ..
        automation_root .. "/include/?/init.lua;"
      if not package.path:find(include_path, 1, true) then
        package.path = include_path .. package.path
      end
    end

    for index = 1, count do
      if loaded[index] == nil then
        local requirement = normalized_requirement(modules[index])
        local success, module = try_require(requirement.moduleName)
        local state = self._moduleStates[index]
        state.loaded = success
        state.value = success and module or nil
        state.error = success and nil or module
        if success then
          loaded[index] = module
          failures[index] = nil
        else
          failures[index] = module
        end
      end
    end
  end

  for index = 1, count do
    local requirement = normalized_requirement(modules[index])
    if loaded[index] == nil and not requirement.optional then
      error("DependencyControl could not load required module '" .. requirement.moduleName ..
        "' after package resolution: " .. tostring(failures[index]), 2)
    end
  end
  return unpack(loaded, 1, count)
end

function Record:register(self_reference, ...)
  if self.moduleName then
    local dummy = self._dummyReference
    if type(self_reference) == "table" then
      if self_reference.version == nil then self_reference.version = self end
      if type(dummy) == "table" and dummy.__depCtrlDummy then
        local proxy = getmetatable(dummy) or {}
        proxy.__index = self_reference
        setmetatable(dummy, proxy)
      end
    end
    loaded_modules[self.moduleName] = self_reference
    package.loaded[self.moduleName] = self_reference
  end
  return self_reference
end

function Record:getVersionString(value, precision)
  return version_string(value == nil and self.version or value, precision)
end

function Record:checkVersion(value, precision)
  local current = parse_version_parts(self.version)
  local required = parse_version_parts(value)
  local count = ({ major = 1, minor = 2, patch = 3 })[precision or "patch"]
  assert(count, "DependencyControl version precision must be major, minor, or patch")
  for index = 1, count do
    if current[index] ~= required[index] then return current[index] > required[index] end
  end
  return true
end

function Record:setVersion(value)
  local success, result = pcall(version_string, value)
  if not success then return nil, result end
  self.version = result
  return result
end

function Record:uninstall(remove_config)
  if remove_config == nil then remove_config = true end
  local success, response = pcall(native_call, "packages.uninstall", {
    recordType = self.moduleName and "module" or "automation",
    namespace = self.namespace,
    removeConfig = not not remove_config
  })
  if not success then return false, response end
  if response.committed ~= true then
    return false, "DependencyControl uninstall did not commit"
  end
  self.uninstalled = true
  self.requiresReload = response.reloadRequired == true
  if self.moduleName then
    loaded_modules[self.moduleName] = nil
    package.loaded[self.moduleName] = nil
  end
  return true, response
end

function Record:getLogger(args)
  args = args or {}
  assert(type(args) == "table", "DependencyControl Logger options must be a table")
  if args.fileBaseName == nil then args.fileBaseName = self.namespace end
  if args.prefix == nil and self.moduleName then args.prefix = "[" .. self.name .. "] " end
  return Logger(args)
end

function Record:getConfigFileName()
  return (self.configDir or "?user/config") .. "/" .. self.configFile
end

function Record:getConfigHandler(defaults, section, no_load)
  return ConfigHandler(self.configFile, defaults, section, no_load)
end

function Record:checkOptionalModules(modules)
  local selected = {}
  if type(modules) == "string" then
    selected[modules] = true
  elseif type(modules) == "table" then
    for _, name in pairs(modules) do selected[name] = true end
  elseif modules ~= nil then
    error("DependencyControl optional module selection must be a string or table", 2)
  end

  local missing = {}
  for index = 1, #(self.requiredModules or {}) do
    local raw = self.requiredModules[index]
    local requirement = normalized_requirement(raw)
    local display_name = type(raw) == "table" and raw.name or nil
    display_name = display_name or requirement.moduleName
    local requested = modules == nil or selected[display_name] or selected[requirement.moduleName]
    if requirement.optional and requested then
      local state = self._moduleStates and self._moduleStates[index]
      local available = state and state.loaded
      if state == nil then available = select(1, try_require(requirement.moduleName)) end
      if not available then
        missing[#missing + 1] = display_name ..
          (requirement.version and " (v" .. requirement.version .. ")" or "")
      end
    end
  end
  if #missing == 0 then return true end
  return false, "Error: " .. self.name ..
    " requires optional modules that are not available:\n— " ..
    table.concat(missing, "\n— ")
end

function Record:registerMacro(name, description, process, validate, is_active, submenu)
  if type(name) == "function" then
    submenu = validate
    is_active = process
    validate = description
    process = name
    name = self.name
    description = self.description
  end
  assert(type(name) == "string" and name ~= "", "DependencyControl Macro name is required")
  assert(type(process) == "function", "DependencyControl Macro process callback is required")
  if submenu == true then submenu = self.name end
  if type(submenu) == "string" and submenu ~= "" then
    name = submenu .. "/" .. name
  end
  return aegisub.register_macro(name, description or "", process, validate, is_active)
end

function Record:registerMacros(macros, submenu_default)
  macros = macros or {}
  if submenu_default == nil then submenu_default = true end
  for index = 1, #macros do
    local macro = macros[index]
    assert(type(macro) == "table", "DependencyControl Macro definitions must be tables")
    local submenu_index = type(macro[1]) == "function" and 4 or 6
    if macro[submenu_index] == nil then macro[submenu_index] = submenu_default end
    self:registerMacro(unpack(macro, 1, 6))
  end
end

local UpdateTask = {}
UpdateTask.__index = UpdateTask
UpdateTask.__name = "UpdateTask"

local function update_target_version(value)
  if value == nil or value == 0 or value == "" then return "" end
  return version_string(value)
end

function UpdateTask:set(target_version, add_feeds, exhaustive, channel, optional)
  self.targetVersion = update_target_version(target_version)
  self.addFeeds = add_feeds or {}
  self.exhaustive = not not exhaustive
  self.channel = channel or ""
  self.optional = not not optional
  return self
end

function UpdateTask:run(wait_lock, exhaustive)
  if self.running then return -10, "DependencyControl update task is already running" end
  local locked, lock_error = self.updater:getLock(wait_lock)
  if not locked then return -5, lock_error end

  self.running = true
  local request = {
    recordType = self.record.moduleName and "module" or "automation",
    namespace = self.record.namespace
  }
  if self.targetVersion ~= "" then request.targetVersion = self.targetVersion end
  if self.channel ~= "" then request.channel = self.channel end
  local success, response = pcall(native_call, "updates.apply", request)
  self.running = false
  if not success then
    self.status = -100
    self.error = tostring(response)
    return self.status, self.error
  end

  self.result = response
  if response.status == "updated" then
    self.updated = true
    self.status = 1
    self.record.version = response.availableVersion
    self.record.activeChannel = response.channel
    self.record.feed = response.feed
    self.ref = self.record.moduleName and
      (loaded_modules[self.record.moduleName] or package.loaded[self.record.moduleName]) or nil
    -- Replacing a module on disk while it is executing is safe, but treating the
    -- current Lua object graph as the new module is not. Consumers can inspect
    -- this flag and reload the Automation script/Lua State at a safe boundary.
    self.requiresReload = true
    return self.status, response.availableVersion, true
  end
  if response.status == "upToDate" or response.status == "installedNewer" then
    self.status = 0
    self.record.version = response.installedVersion
    self.record.activeChannel = response.channel
    self.record.feed = response.feed
    self.ref = self.record.moduleName and
      (loaded_modules[self.record.moduleName] or package.loaded[self.record.moduleName]) or nil
    return self.status, response.installedVersion
  end
  if response.status == "unsupportedPlatform" then
    self.status = -6
    self.error = "DependencyControl update is not available for this platform"
    return self.status, self.error
  end
  self.status = -100
  self.error = "DependencyControl returned an unknown update status: " ..
    tostring(response.status)
  return self.status, self.error
end

local ScriptUpdateRecord = {}
ScriptUpdateRecord.__index = ScriptUpdateRecord
ScriptUpdateRecord.__name = "ScriptUpdateRecord"

function ScriptUpdateRecord:getChannels()
  local channels, default = {}, nil
  for name, channel in pairs(self.data.channels or {}) do
    channels[#channels + 1] = name
    if channel.default and not default then default = name end
  end
  table.sort(channels)
  return channels, default
end

function ScriptUpdateRecord:setChannel(channelName)
  local config = self.config and self.config.c or {}
  local channels, default = self:getChannels()
  config.lastChannel = channelName or config.activeChannel or config.lastChannel or default
  local channel = config.lastChannel and self.data.channels[config.lastChannel]
  if not channel then
    self.activeChannel = config.lastChannel
    return false, self.activeChannel
  end
  self.activeChannel = config.lastChannel
  for key, value in pairs(channel) do self[key] = value end
  self.files = self.files or {}
  return true, self.activeChannel
end

function ScriptUpdateRecord:checkPlatform()
  if not self.activeChannel then
    return self.logger:assert(false, "No active channel.")
  end
  local supported = self.platformSupported
  if supported == nil then supported = true end
  return supported, self.platform
end

function ScriptUpdateRecord:getChangelog()
  -- Feed changelog entries are not yet part of the bounded catalog DTO.
  return ""
end

local UpdateFeed = {}
UpdateFeed.__index = UpdateFeed
UpdateFeed.__name = "UpdateFeed"
UpdateFeed.cache = {}

setmetatable(UpdateFeed, {
  __call = function(_, url, autoFetch, fileName, config, logger)
    assert(type(url) == "string" and url ~= "", "DependencyControl UpdateFeed URL is required")
    local instance = setmetatable({
      url = url,
      config = config or {},
      logger = logger or Logger { fileBaseName = "DepCtrl.UpdateFeed" },
      fileName = fileName
    }, UpdateFeed)
    if UpdateFeed.cache[url] then
      instance.data = UpdateFeed.cache[url]
    elseif autoFetch ~= false then
      instance:fetch(fileName)
    end
    return instance
  end
})

function UpdateFeed:fetch(fileName)
  if fileName ~= nil then self.fileName = fileName end
  local success, response = pcall(native_call, "feeds.inspect", { url = self.url })
  if not success then return false, tostring(response) end
  local data = {
    dependencyControlFeedFormatVersion = response.formatVersion,
    name = response.name,
    description = response.description,
    maintainer = response.maintainer,
    knownFeeds = response.knownFeeds or {},
    macros = {},
    modules = {}
  }
  self.data = data
  UpdateFeed.cache[self.url] = data
  return data
end

function UpdateFeed:expand()
  return self.data
end

function UpdateFeed:getKnownFeeds()
  local result = {}
  for _, url in pairs((self.data and self.data.knownFeeds) or {}) do
    result[#result + 1] = url
  end
  return result
end

function UpdateFeed:getScript(namespace, isModule, config, autoChannel)
  local recordType = isModule and "module" or "automation"
  local response, offset = nil, 0
  local data = {
    channels = {}
  }
  repeat
    local success, page = pcall(native_call, "catalog.lookup", {
      recordType = recordType,
      namespace = namespace,
      feed = self.url,
      channelOffset = offset,
      channelLimit = 16
    })
    if not success then return false, tostring(page) end
    response = page
    data.name = page.name
    data.description = page.description
    data.author = page.author
    data.url = page.url
    for _, channel in ipairs(page.channels or {}) do
      data.channels[channel.name] = {
        version = channel.version,
        released = channel.released,
        default = channel.default,
        platforms = channel.platforms,
        platformSupported = channel.platformSupported,
        files = {},
        requiredModules = {}
      }
    end
    offset = offset + #(page.channels or {})
  until offset >= (response.channelCount or 0)
  local record = setmetatable({
    namespace = namespace,
    data = data,
    config = config or { c = {} },
    logger = self.logger,
    moduleName = isModule and namespace or nil
  }, ScriptUpdateRecord)
  if autoChannel ~= false then record:setChannel() end
  return record
end

function UpdateFeed:getMacro(namespace, config, autoChannel)
  return self:getScript(namespace, false, config, autoChannel)
end

function UpdateFeed:getModule(namespace, config, autoChannel)
  return self:getScript(namespace, true, config, autoChannel)
end

local Updater = {}
Updater.__index = Updater
Updater.__name = "Updater"

setmetatable(Updater, {
  __call = function(_, host, config, logger)
    local instance = setmetatable({}, Updater)
    instance.host = host or script_namespace or "DependencyControl"
    instance.config = config or {
      c = {
        updaterEnabled = true,
        tryAllFeeds = false,
        extraFeeds = {}
      }
    }
    instance.logger = logger or Logger { fileBaseName = "DependencyControl.Updater" }
    instance.tasks = {}
    instance.hasLock = false
    return instance
  end
})

function Updater:addTask(record, target_version, add_feeds, exhaustive, channel, optional)
  assert(type(record) == "table" and type(record.namespace) == "string",
    "DependencyControl Updater requires a version record")
  local key = (record.moduleName and "module\n" or "automation\n") .. record.namespace
  local task = self.tasks[key]
  if task then
    return task:set(target_version, add_feeds, exhaustive, channel, optional)
  end
  task = setmetatable({
    record = record,
    updater = self,
    updated = false,
    running = false,
    requiresReload = false
  }, UpdateTask)
  task:set(target_version, add_feeds, exhaustive, channel, optional)
  self.tasks[key] = task
  return task
end

function Updater:require(record, ...)
  assert(type(record) == "table" and record.moduleName,
    "DependencyControl Updater can only require module records")
  local task, task_error = self:addTask(record, ...)
  if not task then return nil, task_error end
  local code, detail = task:run(true)
  if code < 0 then return nil, code, detail end
  if task.ref ~= nil then return task.ref end
  local success, module = try_require(record.moduleName)
  if success then
    task.ref = module
    return module
  end
  return nil, -55, module
end

function Updater:scheduleUpdate(record)
  if self.config.c.updaterEnabled == false then return -1 end
  if record.virtual then return -3 end
  local task, task_error = self:addTask(record)
  if not task then return nil, task_error end
  return task:run(false)
end

function Updater:getLock(wait_lock)
  if self.hasLock then return true end
  self.hasLock = true
  return true
end

function Updater:releaseLock()
  if not self.hasLock then return false end
  self.hasLock = false
  return true
end

local DependencyControl = {
  capabilities = probe.capabilities,
  serviceVersion = probe.version,
  __name = "DependencyControl"
}
DependencyControl.__index = DependencyControl

setmetatable(DependencyControl, {
  __call = function(_, args)
    assert(type(args) == "table", "DependencyControl expects a record table")
    local module_name = args.moduleName
    local namespace = module_name or args.namespace or script_namespace
    assert(type(namespace) == "string" and namespace ~= "",
      "DependencyControl requires script_namespace or an explicit namespace")
    local name = args.name or (module_name and module_name or script_name) or namespace
    local version = args.version
    if version == nil then version = script_version or 0 end
    version = version_string(version)
    local modules = args.requiredModules or args[1] or {}
    assert(type(modules) == "table", "DependencyControl requiredModules must be a table")
    local config_file = args.configFile or (namespace .. ".json")
    assert(type(config_file) == "string" and config_file ~= "",
      "DependencyControl configFile must be a non-empty string")
    if config_file:sub(-5):lower() ~= ".json" then
      config_file = config_file .. ".json"
    end
    local config_dir = args.configDir or "?user/config"
    assert(type(config_dir) == "string" and config_dir ~= "",
      "DependencyControl configDir must be a non-empty string")

    local request = {
      recordType = module_name and "module" or "automation",
      namespace = namespace,
      name = name,
      version = version,
      configFile = config_file,
      virtual = not not args.virtual
    }
    if args.description or script_description then
      request.description = args.description or script_description
    end
    if args.author or script_author then request.author = args.author or script_author end
    if args.feed then request.feed = args.feed end
    if args.activeChannel then request.activeChannel = args.activeChannel end
    if #modules > 0 then
      request.requiredModules = {}
      for index = 1, #modules do
        request.requiredModules[index] = normalized_requirement(modules[index])
      end
    end

    local registration = native_call("record.register", request)
    local record = {}
    for key, value in pairs(args) do
      if type(key) == "string" then record[key] = value end
    end
    record.name = name
    record.description = request.description or ""
    record.author = request.author or ""
    record.version = version
    record.namespace = namespace
    record.moduleName = module_name
    record.requiredModules = modules
    record.registration = registration
    record.configFile = config_file
    record.configDir = config_dir
    record.__class = DependencyControl
    if module_name and not args.virtual and loaded_modules[module_name] == nil then
      local proxy = {}
      local dummy = setmetatable({
        __depCtrlDummy = true,
        version = record
      }, proxy)
      record._dummyReference = dummy
      record._dummyProxy = proxy
      loaded_modules[module_name] = dummy
    end
    if script_namespace == nil then script_namespace = namespace end
    return setmetatable(record, Record)
  end
})

DependencyControl.Logger = Logger
DependencyControl.ConfigHandler = ConfigHandler
DependencyControl.UpdateFeed = UpdateFeed
DependencyControl.Updater = Updater
DependencyControl.updater = Updater(script_namespace)
DependencyControl.getVersionString = version_string
Record.updater = DependencyControl.updater

return DependencyControl
)lua";

int LoadDependencyControlExport(lua_State *L) {
	auto const* module_name = luaL_checkstring(L, 1);
	auto const* separator = std::strrchr(module_name, '.');
	if (!separator || !separator[1])
		return luaL_error(L, "DependencyControl export module name is invalid");
	lua_getglobal(L, "require");
	lua_pushliteral(L, "l0.DependencyControl");
	if (lua_pcall(L, 1, 1, 0) != 0)
		return lua_error(L);
	lua_getfield(L, -1, separator + 1);
	lua_remove(L, -2);
	if (lua_isnil(L, -1))
		return luaL_error(
			L,
			"DependencyControl export '%s' is not implemented",
			separator + 1);
	return 1;
}

int LoadDependencyControlFacade(lua_State *L) {
	if (luaL_loadbuffer(
		L,
		kDependencyControlFacade.data(),
		kDependencyControlFacade.size(),
		"@<built-in l0.DependencyControl>") != 0) {
		auto message = agi::lua::get_string_or_default(L, -1);
		lua_pop(L, 1);
		throw std::runtime_error(
			"Could not compile the built-in DependencyControl facade: " + message);
	}
	if (lua_pcall(L, 0, 1, 0) != 0) {
		auto message = agi::lua::get_string_or_default(L, -1);
		lua_pop(L, 1);
		throw std::runtime_error(
			"Could not initialize the built-in DependencyControl facade: " + message);
	}
	return 1;
}

} // namespace

void RegisterDependencyControlLuaFacade(lua_State *L) {
	lua_getglobal(L, "aegisub");
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		throw std::logic_error(
			"The Aegisub Lua table must exist before registering DependencyControl");
	}
	agi::lua::push_value(
		L,
		agi::lua::exception_wrapper<LuaDependencyControlCall>);
	lua_setfield(L, -2, "__dependency_control_call");
	lua_pop(L, 1);

	lua_getglobal(L, "package");
	lua_getfield(L, -1, "preload");
	if (!lua_istable(L, -1)) {
		lua_pop(L, 2);
		throw std::logic_error(
			"Lua package.preload is unavailable for DependencyControl");
	}
	agi::lua::push_value(
		L,
		agi::lua::exception_wrapper<LoadDependencyControlFacade>);
	lua_setfield(L, -2, "l0.DependencyControl");
	lua_pushcfunction(L, LoadDependencyControlExport);
	lua_setfield(L, -2, "l0.DependencyControl.Logger");
	lua_pushcfunction(L, LoadDependencyControlExport);
	lua_setfield(L, -2, "l0.DependencyControl.ConfigHandler");
	lua_pushcfunction(L, LoadDependencyControlExport);
	lua_setfield(L, -2, "l0.DependencyControl.UpdateFeed");
	lua_pushcfunction(L, LoadDependencyControlExport);
	lua_setfield(L, -2, "l0.DependencyControl.Updater");
	lua_pop(L, 2);
}

} // namespace Automation4
