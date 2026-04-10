script_name = "Debug Template Scope Test"
script_description = "Exercises template debug scopes"
script_author = "test"
script_version = "1"

local function debug_template_scope(subs, sel, active)
  local row = sel[1] or active
  local line = subs[row]

  aegisub.__set_debug_template_context({
    kind = "expression-run",
    template_code = "return line.text",
    template_text = "!$line.text!",
    expression = "line.text",
    j = 2,
    maxj = 4,
    line_text = line.text,
    line_style = line.style,
    syl_text = "scope",
    syl_i = 1,
    basesyl_text = "scope",
    template_phase = "run",
    debug_scope = "syl",
    highlight_i = 1,
    char_i = 2,
    char_text = "c",
    template_identity = {
      owner_script = "kara-templater.lua",
      template_debug_id = 7,
      template_kind = "line",
      template_kinds = { "line", "syl" },
      fragment_kind = "template",
      template_id = "tpl-7",
      source_line_index = row,
      source_line_indices = { row, row + 1 },
    },
    template_source = {
      style = line.style,
      effect = line.effect,
      text = line.text,
      fragments = {
        { source_line_index = row, fragment_kind = "template", text = line.text, effect = line.effect }
      },
    },
    target = {
      scope_kind = "syl",
      orgline = line,
      line = line,
      syl = {
        i = 1,
        text = "scope",
        text_stripped = "scope",
        inline_fx = "fx",
        start_time = line.start_time,
        end_time = line.end_time,
        duration = line.end_time - line.start_time,
        isfuri = false,
        left = 10,
        center = 20,
        right = 30,
        width = 20,
        height = 10,
      },
      basesyl = {
        i = 1,
        text = "scope",
        text_stripped = "scope",
        start_time = line.start_time,
        end_time = line.end_time,
        duration = line.end_time - line.start_time,
      },
      highlight = {
        i = 1,
        start_time = line.start_time,
        end_time = line.start_time + 250,
        duration = 250,
      },
      char = {
        i = 2,
        text = "c",
      },
    },
    generated = {
      count = 3,
      last_line = {
        generated_index = 3,
        text = "{\\pos(320,240)}generated",
        style = line.style,
        layer = line.layer,
        effect = "fx",
        start_time = line.start_time,
        end_time = line.end_time,
        source_line_index = row,
        template_debug_id = 7,
        template_kind = "line",
        scope_kind = "syl",
        syl_i = 1,
        highlight_i = 1,
        char_i = 2,
      },
    },
  })

  aegisub.progress.task("Template scope ready")
  aegisub.progress.set(55)
  local marker = line.text
  aegisub.debug.out(4, marker)
  aegisub.__set_debug_template_context(nil)
end

aegisub.register_macro("Debug template scope", "Prepare template debug scope data", debug_template_scope)
