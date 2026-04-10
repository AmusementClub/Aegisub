script_name = "Debug Metatable Safety Test"
script_description = "Ensures debug inspection does not invoke __index metamethods"
script_author = "test"
script_version = "1"

builder_like_global = setmetatable({}, {
  __index = function(_, key)
    error("debug inspector invoked __index for key: " .. tostring(key))
  end
})

local function debug_metatable_safety(subs, sel, active)
  local row = sel[1]
  local line = subs[row]
  line.text = "safe:" .. line.text
  subs[row] = line
  aegisub.set_undo_point("Debug Metatable Safety Test")
end

aegisub.register_macro("Debug metatable safety", "Debug metatable safety", debug_metatable_safety)
