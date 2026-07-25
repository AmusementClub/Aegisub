script_name = "DependencyControl transport limits smoke"
script_description = "Checks the private native transport boundary used by the public facade."
script_author = "Aegisub"
script_version = "1.0.0"
script_namespace = "aegisub.dependency-control.transport-smoke"

local kService = "aegisub.dependency-control.service"
local function native_call(operation, request)
  return aegisub.plugin.invoke(kService, operation, request or {})
end

local function expect_rejection(value, expected)
  local succeeded, message = pcall(
    native_call,
    "record.register",
    value)
  assert(succeeded == false)
  assert(tostring(message):find(expected, 1, true) ~= nil)
end

local cycle = {}
cycle.self = cycle
expect_rejection(cycle, "cyclic tables")

expect_rejection({ [1] = "array", field = "object" }, "mixed array and object keys")
expect_rejection({ [1] = "first", [3] = "third" }, "arrays cannot contain holes")
expect_rejection({ callback = function() end }, "supports only")
expect_rejection({ invalidUtf8 = string.char(0xC0, 0xAF) }, "valid UTF-8")

local DependencyControl = require("l0.DependencyControl")
local version = DependencyControl {}

version:registerMacro(function()
  assert(version.registration.registered == true)
  aegisub.set_status_text("dependency-control-transport-limits=true")
end)
