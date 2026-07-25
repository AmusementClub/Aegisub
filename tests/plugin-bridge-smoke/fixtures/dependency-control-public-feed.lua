script_name = "DependencyControl public feed smoke"
script_description = "Discover, install, and update-check packages from TypesettingTools public feeds."
script_author = "Aegisub"
script_version = "1.0.0"
script_namespace = "aegisub.dependency-control.public-feed-smoke"

-- Five public feeds under TypesettingTools, one install target each.
-- Prefer packages with no external DepCtrl requiredModules so install can finish
-- without resolving built-in aegisub.* modules as feed packages.
-- Host catalog API uses recordType "module" | "automation" (not "macro").
local packages = {
  {
    feed_id = "CoffeeFlux",
    feed = "https://raw.githubusercontent.com/TypesettingTools/CoffeeFlux-Aegisub-Scripts/master/DependencyControl.json",
    recordType = "automation",
    namespace = "Flux.DialogSwapper",
    repo = "TypesettingTools/CoffeeFlux-Aegisub-Scripts",
  },
  {
    feed_id = "lyger",
    feed = "https://raw.githubusercontent.com/TypesettingTools/lyger-Aegisub-Scripts/master/DependencyControl.json",
    recordType = "automation",
    namespace = "lyger.LayerIncrement",
    repo = "TypesettingTools/lyger-Aegisub-Scripts",
  },
  {
    feed_id = "arch1t3cht",
    feed = "https://raw.githubusercontent.com/TypesettingTools/arch1t3cht-Aegisub-Scripts/master/DependencyControl.json",
    recordType = "automation",
    namespace = "arch.RWTools",
    repo = "TypesettingTools/arch1t3cht-Aegisub-Scripts",
  },
  {
    feed_id = "Myaamori",
    feed = "https://raw.githubusercontent.com/TypesettingTools/Myaamori-Aegisub-Scripts/master/DependencyControl.json",
    recordType = "module",
    namespace = "myaa.pl",
    repo = "TypesettingTools/Myaamori-Aegisub-Scripts",
  },
  {
    feed_id = "a-mo",
    feed = "https://raw.githubusercontent.com/TypesettingTools/Aegisub-Motion/DepCtrl/DependencyControl.json",
    recordType = "module",
    namespace = "a-mo.Log",
    repo = "TypesettingTools/Aegisub-Motion",
  },
}

local function native_call(operation, request)
  return aegisub.plugin.invoke(
    "aegisub.dependency-control.service",
    operation,
    request or {})
end

local function must(value, message)
  if not value then error(message or "assertion failed", 2) end
  return value
end

local function report(label, ok, detail)
  local status = ok and "ok" or "FAIL"
  aegisub.log(3, "[public-feed] %s: %s%s\n", label, status, detail and (" — " .. detail) or "")
end

local function process(subtitles, selected_lines, active_line)
  local results = {}
  local failures = {}

  -- 1) Register feeds
  for _, pkg in ipairs(packages) do
    local ok, err = pcall(function()
      local upserted = native_call("feeds.upsert", {
        id = pkg.feed_id,
        label = pkg.repo,
        url = pkg.feed,
        enabled = true,
      })
      must(upserted.saved == true or upserted.id == pkg.feed_id,
        "feeds.upsert did not save " .. pkg.feed_id)
      must(type(upserted.url) == "string" and upserted.url ~= "", "feeds.upsert missing url")
    end)
    if not ok then
      table.insert(failures, "feeds.upsert " .. pkg.feed_id .. ": " .. tostring(err))
      report("feeds.upsert " .. pkg.feed_id, false, tostring(err))
    else
      report("feeds.upsert " .. pkg.feed_id, true)
    end
  end

  local listed = native_call("feeds.list", {})
  must(type(listed.feeds) == "table", "feeds.list missing feeds")
  must((listed.totalCount or 0) >= #packages, "feeds.list totalCount too small")
  report("feeds.list", true, "totalCount=" .. tostring(listed.totalCount))

  -- 2) Discover: test + inspect each feed
  for _, pkg in ipairs(packages) do
    local ok, err = pcall(function()
      local tested = native_call("feeds.test", { url = pkg.feed })
      must(type(tested) == "table", "feeds.test empty for " .. pkg.feed_id)

      local inspected = native_call("feeds.inspect", { url = pkg.feed })
      must(type(inspected) == "table", "feeds.inspect empty for " .. pkg.feed_id)
    end)
    if not ok then
      table.insert(failures, "discover " .. pkg.feed_id .. ": " .. tostring(err))
      report("discover " .. pkg.feed_id, false, tostring(err))
    else
      report("discover " .. pkg.feed_id, true)
    end
  end

  -- 3) Install one package per feed
  for _, pkg in ipairs(packages) do
    local ok, err = pcall(function()
      local installed = native_call("packages.install", {
        recordType = pkg.recordType,
        namespace = pkg.namespace,
        feed = pkg.feed,
      })
      must(type(installed) == "table", "packages.install returned non-table")
      local status = installed.status or ""
      must(
        status == "installed"
          or status == "alreadyInstalled"
          or status == "updated",
        "packages.install status=" .. tostring(status) .. " for " .. pkg.namespace)
      if status == "unsupportedPlatform" then
        error("unsupportedPlatform for " .. pkg.namespace)
      end

      local detail = native_call("installed.get", {
        recordType = pkg.recordType,
        namespace = pkg.namespace,
      })
      must(detail.exists == true, "installed.get missing " .. pkg.namespace)
      must(type(detail.package) == "table", "installed.get package missing")
      must(
        type(detail.package.version) == "string" and detail.package.version ~= "",
        "installed version missing for " .. pkg.namespace)

      results[pkg.namespace] = {
        status = status,
        version = detail.package.version,
        feed = pkg.feed,
        recordType = pkg.recordType,
        repo = pkg.repo,
      }
    end)
    if not ok then
      table.insert(failures, "install " .. pkg.namespace .. ": " .. tostring(err))
      report("install " .. pkg.namespace, false, tostring(err))
    else
      report(
        "install " .. pkg.namespace,
        true,
        "status=" .. results[pkg.namespace].status
          .. " version=" .. results[pkg.namespace].version)
    end
  end

  -- 4) Update check for each successfully installed package
  for namespace, info in pairs(results) do
    local ok, err = pcall(function()
      local update = native_call("updates.check", {
        recordType = info.recordType,
        namespace = namespace,
      })
      must(type(update) == "table", "updates.check empty for " .. namespace)
      local status = update.status or ""
      must(
        status == "upToDate"
          or status == "updateAvailable"
          or status == "unknown",
        "updates.check status=" .. tostring(status) .. " for " .. namespace)
      info.updateStatus = status
      info.availableVersion = update.availableVersion
        or update.installedVersion
        or info.version
    end)
    if not ok then
      table.insert(failures, "updates.check " .. namespace .. ": " .. tostring(err))
      report("updates.check " .. namespace, false, tostring(err))
    else
      report(
        "updates.check " .. namespace,
        true,
        "status=" .. tostring(info.updateStatus)
          .. " available=" .. tostring(info.availableVersion))
    end
  end

  local inventory = native_call("installed.list", {})
  must(type(inventory.packages) == "table", "installed.list missing packages")
  report(
    "installed.list",
    true,
    "totalCount=" .. tostring(inventory.totalCount or #inventory.packages))

  local installed_count = 0
  for _ in pairs(results) do
    installed_count = installed_count + 1
  end

  local summary_parts = {
    "public-feed-smoke",
    "installed=" .. tostring(installed_count) .. "/" .. tostring(#packages),
    "failures=" .. tostring(#failures),
  }
  for namespace, info in pairs(results) do
    table.insert(
      summary_parts,
      string.format(
        "%s@%s(%s/%s)",
        namespace,
        info.version,
        info.status,
        tostring(info.updateStatus or "?")))
  end
  local text = table.concat(summary_parts, " | ")
  aegisub.set_status_text(text)
  aegisub.log(3, "[public-feed] %s\n", text)

  if #failures > 0 then
    error("Public feed smoke failures:\n- " .. table.concat(failures, "\n- "))
  end
end

aegisub.register_macro(script_name, script_description, process)
