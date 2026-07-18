script_name = "DependencyControl Lua compatibility smoke"
script_description = "Exercises the built-in l0.DependencyControl facade without private APIs."
script_author = "Aegisub"
script_version = "1.0.0"
script_namespace = "aegisub.dependency-control.smoke"

local Logger = require("l0.DependencyControl.Logger")
local UpdateFeed = require("l0.DependencyControl.UpdateFeed")
local Updater = require("l0.DependencyControl.Updater")
local DependencyControl = require("l0.DependencyControl")
assert(DependencyControl.Logger == Logger)
assert(DependencyControl.UpdateFeed == UpdateFeed)
assert(DependencyControl.Updater == Updater)
assert(getmetatable(DependencyControl.updater) == Updater)

package.preload["fixture.depctrl.cycle_a"] = function()
  local record = DependencyControl {
    moduleName = "fixture.depctrl.cycle_a",
    version = "1.0.0",
    requiredModules = { "fixture.depctrl.cycle_b" }
  }
  local cycle_b = record:requireModules()
  return record:register({ name = "cycle-a", cycle_b = cycle_b })
end

package.preload["fixture.depctrl.cycle_b"] = function()
  local record = DependencyControl {
    moduleName = "fixture.depctrl.cycle_b",
    version = "1.0.0",
    requiredModules = { "fixture.depctrl.cycle_a" }
  }
  local cycle_a = record:requireModules()
  return record:register({ name = "cycle-b", cycle_a = cycle_a })
end

local cycle_a = require("fixture.depctrl.cycle_a")
assert(cycle_a.name == "cycle-a")
assert(cycle_a.cycle_b.name == "cycle-b")
assert(cycle_a.cycle_b.cycle_a.name == "cycle-a")
assert(cycle_a.version:getVersionString() == "1.0.0")

local version = DependencyControl {
  feed = "https://example.invalid/DependencyControl.json",
  {
    {
      "aegisub.dependency-control.optional-fixture",
      version = "1.0.0",
      optional = true
    }
  }
}

version:registerMacro(function()
  assert(DependencyControl.capabilities.recordRegistration == true)
  assert(DependencyControl.capabilities.packageUninstall == true)
  assert(version.registration.registered == true)
  assert(version.registration.requiredModuleCount == 1)
  local optional_module = version:requireModules()
  assert(optional_module == nil)
  local have_optional, optional_message = version:checkOptionalModules(
    "aegisub.dependency-control.optional-fixture")
  assert(have_optional == false)
  assert(optional_message:find("optional-fixture", 1, true) ~= nil)

  local logger = version:getLogger {
    toWindow = false,
    toFile = true,
    fileBaseName = "DependencyControl-Lua-Smoke"
  }
  assert(logger:warn("warning %d", 1) == true)
  local deleted_files = logger:trimFiles(true)
  assert(deleted_files >= 1)
  assert(logger:assert(true, "unused") == true)
  local logged, log_error = pcall(function()
    logger:assert(false, "failure %d", 2)
  end)
  assert(logged == false)
  assert(tostring(log_error):find("failure 2", 1, true) ~= nil)
  local recursive = {}
  recursive.self = recursive
  assert(logger:dumpToString(recursive):find("@1", 1, true) ~= nil)

  local update_feed = UpdateFeed("https://example.invalid/DependencyControl.json", false)
  update_feed.data = {
    name = "fixture feed",
    knownFeeds = { modules = "https://example.invalid/modules.json" },
    macros = {},
    modules = {}
  }
  assert(update_feed:expand().name == "fixture feed")
  assert(update_feed:getKnownFeeds()[1] == "https://example.invalid/modules.json")

  local compatibility = DependencyControl {
    moduleName = "fixture.depctrl.compatibility",
    version = "0.5.1",
    virtual = true
  }
  assert(compatibility.__class == DependencyControl)
  local standalone_updater = Updater("dependency-control-lua-smoke")
  local first_task = standalone_updater:addTask(compatibility, "1.0.0")
  local same_task = standalone_updater:addTask(compatibility, "1.1.0")
  assert(first_task == same_task)
  assert(same_task.targetVersion == "1.1.0")
  assert(standalone_updater:scheduleUpdate(compatibility) == -3)
  assert(standalone_updater:getLock(false) == true)
  assert(standalone_updater:releaseLock() == true)
  assert(standalone_updater:releaseLock() == false)
  local actual = DependencyControl {
    moduleName = "fixture.depctrl.actual",
    version = 0x000702,
    virtual = true
  }
  assert(actual:getVersionString() == "0.7.2")
  assert(compatibility:checkVersion(actual, "major") == true)
  assert(actual:checkVersion(compatibility, "minor") == true)
  assert(compatibility:checkVersion(actual, "minor") == false)
  assert(compatibility:setVersion("1.2") == "1.2.0")
  aegisub.set_status_text("dependency-control-lua-compatibility=true")
end)
