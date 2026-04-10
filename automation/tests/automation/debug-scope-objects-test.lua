script_name = "Debug Scope Objects Test"
script_description = "Exercises progress and karaskel debug objects"
script_author = "test"
script_version = "1"

include "karaskel.lua"

local function debug_scope_objects(subs, sel, active)
  aegisub.progress.title("Debug Scope Objects")
  aegisub.progress.task("Collecting header data")
  aegisub.progress.set(12.5)

  local meta, styles = karaskel.collect_head(subs, true)
  local row = sel[1] or active
  local line = subs[row]
  karaskel.preproc_line_pos(meta, styles, line)

  local styleref = line.styleref
  local kara = line.kara
  local furi = line.furi
  local syl = line.kara[0]

  aegisub.progress.task("Karaskel objects ready")
  aegisub.progress.set(37.5)

  if styleref and kara and furi and syl then
    aegisub.debug.out(4, "Debug scope objects ready")
  end
end

aegisub.register_macro("Debug scope objects", "Prepare progress and karaskel debug objects", debug_scope_objects)
