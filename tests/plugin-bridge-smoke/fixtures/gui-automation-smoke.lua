script_name = "GUI automation smoke"
script_description = "Mutates a subtitle line through the GUI scenario host."
script_author = "Aegisub"
script_version = "1.0"

local function mutate(subtitles, selected_lines, active_line)
  assert(#selected_lines > 0)
  assert(active_line > 0)

  local line = subtitles[selected_lines[1]]
  line.text = "GUI automation smoke mutation"
  subtitles[selected_lines[1]] = line
  aegisub.set_status_text("gui-automation-smoke=complete")
  aegisub.set_undo_point("GUI automation smoke")
end

aegisub.register_macro(
  "GUI automation smoke",
  "Mutates a subtitle line for the real-process GUI smoke test.",
  mutate)
