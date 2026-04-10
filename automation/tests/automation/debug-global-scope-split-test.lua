script_name = "Debug Global Scope Split Test"
script_description = "Ensures script globals are separated from runtime globals"
script_author = "test"
script_version = "1"

for i = 1, 80 do
  _G[string.format("helper_%03d", i)] = function()
    return i
  end
end

tags = ""
res = { value = 42 }

local function debug_global_scope_split(subs, sel, active)
  tags = "{\\bord2}"
  local row = sel[1]
  local line = subs[row]
  line.text = "split:" .. line.text
  subs[row] = line
  aegisub.set_undo_point("Debug Global Scope Split Test")
end

aegisub.register_macro("Debug global scope split", "Debug global scope split", debug_global_scope_split)
