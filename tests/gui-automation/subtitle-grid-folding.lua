script_name = "Subtitle grid folding regression"
script_description = "Real-command folding, undo, clipboard, and Lua replacement assertions."
script_author = "Aegisub"
script_version = "1.0"

local fold_key = "_aegi_folddata"

local function dialogues(subtitles)
  local rows = {}
  for index = 1, #subtitles do
    local line = subtitles[index]
    if line.class == "dialogue" then
      rows[#rows + 1] = { index = index, line = line }
    end
  end
  return rows
end

local function boundary(line)
  local value = line.extra and line.extra[fold_key]
  if not value then return nil end
  local direction, collapsed, id = value:match("^([01]);([01]);(%d+)$")
  if not direction then return false end
  return { direction = direction, collapsed = collapsed, id = id }
end

local function group(rows, first, last, collapsed)
  local start_marker = boundary(rows[first].line)
  local end_marker = boundary(rows[last].line)
  if not start_marker or not end_marker then return false end
  if start_marker.direction ~= "0" or end_marker.direction ~= "1" then return false end
  if start_marker.collapsed ~= collapsed or end_marker.collapsed ~= collapsed then return false end
  if start_marker.id ~= end_marker.id then return false end
  for row = first + 1, last - 1 do
    if boundary(rows[row].line) ~= nil then return false end
  end
  return start_marker.id
end

local function original(subtitles, collapsed, edited)
  local rows = dialogues(subtitles)
  if #rows ~= 5 then return false end
  if rows[1].line.text ~= "before" or rows[5].line.text ~= "after" then return false end
  if rows[2].line.text ~= (edited and "fold A edited" or "fold A") then return false end
  if rows[3].line.text ~= "fold B" or rows[4].line.text ~= "fold C" then return false end
  if rows[2].line.extra["fold-test"] ~= "retained" then return false end
  return group(rows, 2, 4, collapsed) and boundary(rows[1].line) == nil and boundary(rows[5].line) == nil
end

local function copies(subtitles, partial)
  local rows = dialogues(subtitles)
  if #rows ~= (partial and 9 or 8) then return false end
  local first = group(rows, 2, 4, "1")
  local second = group(rows, 5, 7, "1")
  if not first or not second or first == second then return false end
  for row = 2, 4 do
    if rows[row].line.text ~= rows[row + 3].line.text then return false end
  end
  if rows[5].line.extra["fold-test"] ~= "retained" then return false end
  if partial then
    if rows[8].line.text ~= "fold A edited" or boundary(rows[8].line) ~= nil then return false end
    if rows[8].line.extra["fold-test"] ~= "retained" then return false end
  end
  return rows[1].line.text == "before" and rows[#rows].line.text == "after"
end

local function register_check(name, predicate)
  -- The scenario asserts the validator result, so a data mismatch fails the run.
  aegisub.register_macro(name, name, function() end, predicate)
end

aegisub.register_macro("Prepare", "Create deterministic folding input", function(subtitles)
  local rows = dialogues(subtitles)
  assert(#rows > 0, "default document has no template dialogue")
  local template = rows[1].line
  for row = #rows, 1, -1 do subtitles.delete(rows[row].index) end
  for index, text in ipairs({ "before", "fold A", "fold B", "fold C", "after" }) do
    local line = {}
    for key, value in pairs(template) do line[key] = value end
    line.text = text
    line.start_time = (index - 1) * 1000
    line.end_time = index * 1000
    line.extra = index == 2 and { ["fold-test"] = "retained" } or {}
    subtitles.append(line)
  end
  aegisub.set_undo_point("Prepare folding regression")
  rows = dialogues(subtitles)
  return { rows[2].index, rows[3].index, rows[4].index }, rows[2].index
end)

aegisub.register_macro("Select original", "Select every real member of the original group", function(subtitles)
  local rows = dialogues(subtitles)
  return { rows[2].index, rows[3].index, rows[4].index }, rows[2].index
end)

aegisub.register_macro("Select boundary", "Select only the original start boundary", function(subtitles)
  local rows = dialogues(subtitles)
  return { rows[2].index }, rows[2].index
end)

aegisub.register_macro("Select end boundary", "Select only the original end boundary", function(subtitles)
  local rows = dialogues(subtitles)
  return { rows[4].index }, rows[4].index
end)

aegisub.register_macro("Select insertion", "Select the ungrouped final line", function(subtitles)
  local rows = dialogues(subtitles)
  return { rows[#rows].index }, rows[#rows].index
end)

aegisub.register_macro("Select hidden join", "Join hidden group members with an active line outside the fold", function(subtitles)
  local rows = dialogues(subtitles)
  return { rows[3].index, rows[4].index, rows[5].index }, rows[5].index
end)

aegisub.register_macro("Replace boundary text", "Replace a Lua dialogue while retaining fold identity", function(subtitles)
  local rows = dialogues(subtitles)
  local line = rows[2].line
  assert(line.text == "fold A", "unexpected original boundary text")
  line.text = "fold A edited"
  subtitles[rows[2].index] = line
  aegisub.set_undo_point("Replace folding boundary text")
  return { rows[2].index, rows[3].index, rows[4].index }, rows[2].index
end)

register_check("Original collapsed", function(subtitles) return original(subtitles, "1", false) end)
register_check("Original expanded", function(subtitles) return original(subtitles, "0", false) end)
register_check("Edited collapsed", function(subtitles) return original(subtitles, "1", true) end)
register_check("Full copy has fresh ID", function(subtitles) return copies(subtitles, false) end)
register_check("Partial copy has no marker", function(subtitles) return copies(subtitles, true) end)
register_check("Partial duplicate has no marker", function(subtitles)
  local rows = dialogues(subtitles)
  if #rows ~= 9 then return false end
  local first = group(rows, 2, 4, "1")
  local second = group(rows, 6, 8, "1")
  if not first or not second or first == second then return false end
  for row = 2, 4 do
    if rows[row].line.text ~= rows[row + 4].line.text then return false end
  end
  return rows[5].line.text == "fold C" and boundary(rows[5].line) == nil
    and rows[2].line.extra["fold-test"] == "retained"
    and rows[6].line.extra["fold-test"] == "retained"
end)
register_check("Initial default restored", function(subtitles)
  local rows = dialogues(subtitles)
  return #rows == 1 and rows[1].line.text == "" and boundary(rows[1].line) == nil
end)
register_check("Hidden join selection", function(subtitles, selected, active)
  local rows = dialogues(subtitles)
  if not original(subtitles, "1", false) or #selected ~= 3 then return false end
  return selected[1] == rows[3].index and selected[2] == rows[4].index
    and selected[3] == rows[5].index and active == rows[5].index
end)
register_check("Hidden join cleared boundaries", function(subtitles)
  local rows = dialogues(subtitles)
  if #rows ~= 3 then return false end
  if rows[1].line.text ~= "before" or rows[2].line.text ~= "fold A" or rows[3].line.text ~= "fold B" then return false end
  if rows[2].line.extra["fold-test"] ~= "retained" then return false end
  for _, row in ipairs(rows) do
    if boundary(row.line) ~= nil then return false end
  end
  return true
end)
register_check("All markers cleared", function(subtitles)
  local rows = dialogues(subtitles)
  if #rows ~= 9 or rows[2].line.extra["fold-test"] ~= "retained" then return false end
  for _, row in ipairs(rows) do
    if boundary(row.line) ~= nil then return false end
  end
  return rows[1].line.text == "before" and rows[9].line.text == "after"
end)
