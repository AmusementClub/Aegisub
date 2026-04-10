script_name = "Debug Session Test"
script_description = "Headless debug smoke test"
script_author = "test"
script_version = "1"

local function decorate_text(text)
  local prefix = "dbg:"
  return prefix .. text
end

local function debug_smoke(subs, sel, active)
  local row = sel[1]
  local line = subs[row]
  local original = line.text
  line.text = decorate_text(original)
  subs[row] = line
  aegisub.set_undo_point("Debug Session Test")
end

aegisub.register_macro("Debug smoke", "Debug smoke test", debug_smoke)
