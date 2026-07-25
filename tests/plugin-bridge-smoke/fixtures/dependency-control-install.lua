script_name = "DependencyControl install transaction smoke"
script_description = "Downloads a missing module through the built-in DependencyControl service."
script_author = "Aegisub"
script_version = "1.0.0"
script_namespace = "aegisub.dependency-control.install-smoke"

local feed = assert(os.getenv("AEGISUB_DEPENDENCY_CONTROL_FIXTURE_FEED_URL"))
local module_name = assert(os.getenv("AEGISUB_DEPENDENCY_CONTROL_FIXTURE_MODULE"))
local expected = assert(os.getenv("AEGISUB_DEPENDENCY_CONTROL_FIXTURE_EXPECTED"))
local Updater = require("l0.DependencyControl.Updater")
local DependencyControl = require("l0.DependencyControl")
assert(DependencyControl.Updater == Updater)
local version = DependencyControl {
  feed = feed,
  {
    { module_name, version = "1.0.0" }
  }
}

version:registerMacro(function()
  local function native_call(operation, request)
    return aegisub.plugin.invoke(
      "aegisub.dependency-control.service",
      operation,
      request or {})
  end
  local update_mode = os.getenv("AEGISUB_DEPENDENCY_CONTROL_FIXTURE_UPDATE_PROXY_MODE")
  if update_mode and update_mode ~= "" then
    local request = {
      mode = update_mode,
      manualProxyUri = os.getenv("AEGISUB_DEPENDENCY_CONTROL_FIXTURE_MANUAL_PROXY") or "",
      useDefaultCredentials = false
    }
    local bypass = os.getenv("AEGISUB_DEPENDENCY_CONTROL_FIXTURE_PROXY_BYPASS")
    if bypass and bypass ~= "" then request.bypass = { bypass } end
    native_call("network.settings.update", request)
  end
  local settings = native_call("network.settings.get", {})
  local expected_mode = os.getenv("AEGISUB_DEPENDENCY_CONTROL_FIXTURE_EXPECTED_PROXY_MODE")
  if expected_mode and expected_mode ~= "" then
    assert(settings.mode == expected_mode)
  end
  local network_test_url = os.getenv("AEGISUB_DEPENDENCY_CONTROL_FIXTURE_NETWORK_TEST_URL")
  if network_test_url and network_test_url ~= "" then
    local test = native_call("network.test", { url = network_test_url })
    assert(test.success == true)
    assert(test.mode == settings.mode)
  end
  if os.getenv("AEGISUB_DEPENDENCY_CONTROL_FIXTURE_CONFIG_TEST") == "1" then
    local ConfigHandler = require("l0.DependencyControl.ConfigHandler")
    assert(ConfigHandler == DependencyControl.ConfigHandler)
    local configuration = ConfigHandler(
      "compatibility.json",
      {
        theme = "light",
        nested = { enabled = true, count = 1 }
      },
      { "profiles", "fixture" },
      true)
    assert(configuration.c.theme == "light")
    assert(configuration.c.nested.enabled == true)
    local migrated = ConfigHandler(
      "legacy.json",
      { legacy = "default" },
      nil,
      false)
    assert(migrated.c.legacy == "from-legacy")
    configuration.c.theme = "dark"
    configuration.c.nested.count = 2
    assert(configuration:import({ imported = 7, _private = "ignored" }) == true)
    assert(configuration:write() == true)

    local reloaded = ConfigHandler(
      "compatibility.json",
      { theme = "light", nested = { enabled = false, count = 0 } },
      { "profiles", "fixture" })
    assert(reloaded.c.theme == "dark")
    assert(reloaded.c.nested.enabled == true)
    assert(reloaded.c.nested.count == 2)
    assert(reloaded.c.imported == 7)
    assert(reloaded.c._private == nil)

    local sibling = reloaded:getSectionHandler(
      { "profiles", "sibling" },
      { enabled = false },
      true)
    sibling.c.enabled = true
    assert(sibling:write() == true)
    assert(sibling:delete() == true)

    local corrupted = ConfigHandler("corrupt.json", {}, nil, true)
    local loaded, message = corrupted:load()
    assert(loaded == false)
    assert(tostring(message):find("corrupted", 1, true) ~= nil)
    corrupted.c.recovered = true
    assert(corrupted:write() == true)

    local escaped = ConfigHandler("../escape.json", {}, nil, true)
    local escaped_write, escaped_error = escaped:write()
    assert(escaped_write == false)
    assert(tostring(escaped_error):find("safe JSON file name", 1, true) ~= nil)
  end
local uninstall_recovery = os.getenv(
  "AEGISUB_DEPENDENCY_CONTROL_FIXTURE_UNINSTALL_RECOVERY") == "1"
local success, module
if uninstall_recovery then
  success, module = true, nil
else
  success, module = pcall(function()
    return version:requireModules()
  end)
end
  if success and not uninstall_recovery then
    assert(type(module) == "table")
    assert(module.value == expected)
    local installed_test = os.getenv(
      "AEGISUB_DEPENDENCY_CONTROL_FIXTURE_INSTALLED_STATE_TEST")
    if installed_test and installed_test ~= "" then
      local installed = native_call("installed.list", {})
      assert(installed.schemaVersion == 1)
      assert(type(installed.revision) == "number" and installed.revision >= 2)
      assert(installed.corrupted == (installed_test == "corrupted"))
      assert(type(installed.totalCount) == "number" and installed.totalCount >= 2)
      local found_summary = nil
      for _, package in ipairs(installed.packages) do
        if package.recordType == "module" and package.namespace == module_name then
          found_summary = package
          break
        end
      end
      assert(found_summary ~= nil)
      local expected_installed_version = os.getenv(
        "AEGISUB_DEPENDENCY_CONTROL_FIXTURE_INSTALLED_VERSION") or "1.0.0"
      local detail = native_call("installed.get", {
        recordType = "module",
        namespace = module_name
      })
      assert(detail.exists == true)
      local found = assert(detail.package)
      assert(found.version == expected_installed_version)
      assert(found.channel == "release")
      assert(found.feed == feed)
      assert(found.source == "transaction")
      local expected_file_count = tonumber(os.getenv(
        "AEGISUB_DEPENDENCY_CONTROL_FIXTURE_INSTALLED_FILE_COUNT") or "1")
      assert(#found.files == expected_file_count)
      local expected_target = "include/" .. module_name:gsub("%.", "/") .. ".lua"
      local found_expected_target = false
      for _, installed_file in ipairs(found.files) do
        if installed_file.target == expected_target then found_expected_target = true end
      end
      assert(found_expected_target == true)
    end
    local catalog_version = os.getenv(
      "AEGISUB_DEPENDENCY_CONTROL_FIXTURE_CATALOG_VERSION")
    if catalog_version and catalog_version ~= "" then
      local catalog = native_call("catalog.lookup", {
        recordType = "module",
        namespace = module_name
      })
      assert(catalog.namespace == module_name)
      assert(catalog.feed == feed)
      local release = nil
      for _, channel in ipairs(catalog.channels) do
        if channel.name == "release" then release = channel break end
      end
      assert(release ~= nil and release.version == catalog_version)
      local update = native_call("updates.check", {
        recordType = "module",
        namespace = module_name
      })
      assert(update.status == "updateAvailable")
      assert(update.installedVersion == "1.0.0")
      assert(update.availableVersion == catalog_version)
      assert(update.channel == "release")
      assert(update.platformSupported == true)

      local apply_version = os.getenv(
        "AEGISUB_DEPENDENCY_CONTROL_FIXTURE_APPLY_VERSION")
      if apply_version and apply_version ~= "" then
        assert(apply_version == catalog_version)
        local update_record = DependencyControl {
          moduleName = module_name,
          name = module_name,
          version = "1.0.0",
          description = "",
          author = "",
          feed = feed,
          activeChannel = "release"
        }
        local task = DependencyControl.updater:addTask(
          update_record, apply_version, {}, false, "release", false)
        assert(task == DependencyControl.updater:addTask(
          update_record, apply_version, {}, false, "release", false))
        local code, applied_version, reload_required = task:run(true)
        if os.getenv("AEGISUB_DEPENDENCY_CONTROL_FIXTURE_APPLY_EXPECT_FAILURE") == "1" then
          assert(code < 0)
          assert(task.updated == false)
          assert(type(applied_version) == "string" and applied_version ~= "")
          assert(DependencyControl.updater:releaseLock() == true)
        else
          assert(code == 1)
          assert(applied_version == apply_version)
          assert(reload_required == true)
          assert(task.updated == true)
          assert(task.requiresReload == true)
          assert(task.ref == module)
          assert(task.result.status == "updated")
          assert(task.result.committed == true)
          assert(task.result.installedVersion == "1.0.0")
          assert(task.result.availableVersion == apply_version)
          assert(task.result.channel == "release")
          assert(task.result.feed == feed)
          assert(task.result.platformSupported == true)
          assert(#task.result.installedFiles >= 1)
          assert(update_record.version == apply_version)
          assert(update_record.activeChannel == "release")
          assert(DependencyControl.updater:releaseLock() == true)

          local required_after_update = DependencyControl.updater:require(update_record)
          assert(required_after_update == module)
          assert(task.status == 0)
          assert(DependencyControl.updater:releaseLock() == true)

          local detail = native_call("installed.get", {
            recordType = "module",
            namespace = module_name
          })
          assert(detail.exists == true)
          assert(detail.package.version == apply_version)

          local repeated = native_call("updates.apply", {
            recordType = "module",
            namespace = module_name
          })
          assert(repeated.status == "upToDate")
          assert(repeated.committed == false)
          assert(repeated.installedVersion == apply_version)
          assert(repeated.availableVersion == apply_version)
          assert(#repeated.installedFiles == 0)
        end
      end
    end
    if os.getenv("AEGISUB_DEPENDENCY_CONTROL_FIXTURE_UNINSTALL_TEST") == "1" then
      local uninstall_record = DependencyControl {
        moduleName = module_name,
        name = module_name,
        version = os.getenv(
          "AEGISUB_DEPENDENCY_CONTROL_FIXTURE_INSTALLED_VERSION") or "1.0.0",
        description = "",
        author = "",
        feed = feed,
        activeChannel = "release"
      }
      local remove_config = os.getenv(
        "AEGISUB_DEPENDENCY_CONTROL_FIXTURE_UNINSTALL_REMOVE_CONFIG") ~= "0"
      local removed, response = uninstall_record:uninstall(remove_config)
      local after = native_call("installed.get", {
        recordType = "module",
        namespace = module_name
      })
      if os.getenv("AEGISUB_DEPENDENCY_CONTROL_FIXTURE_UNINSTALL_EXPECT_FAILURE") == "1" then
        assert(removed == false)
        assert(type(response) == "string" and response ~= "")
        assert(after.exists == true)
      else
        assert(removed == true)
        assert(response.committed == true)
        assert(response.reloadRequired == true)
        assert(uninstall_record.uninstalled == true)
        assert(after.exists == false)
      end
    end
    aegisub.set_status_text("dependency-control-install-transaction=true")
  elseif uninstall_recovery then
    local after = native_call("installed.get", {
      recordType = "module",
      namespace = module_name
    })
    assert(after.exists == false)
    aegisub.set_status_text("dependency-control-uninstall-recovered=true")
  end
  local marker = assert(os.getenv("AEGISUB_DEPENDENCY_CONTROL_FIXTURE_MARKER"))
  local file = assert(io.open(marker, "wb"))
  if uninstall_recovery then
    file:write("ok:uninstall-recovered")
  elseif success then
    file:write("ok:" .. tostring(type(module) == "table" and module.value or module))
  else
    file:write("error:" .. tostring(module))
  end
  file:close()
end)
