// Copyright (c) 2026
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS; WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

#pragma once

#include <optional>
#include <string>
#include <vector>

namespace Automation4 {
	struct AutomationTemplateLineSnapshot {
		std::optional<int> index;
		std::optional<std::string> line_class;
		std::optional<int> layer;
		std::optional<std::string> style;
		std::optional<std::string> actor;
		std::optional<std::string> effect;
		std::optional<bool> comment;
		std::optional<std::string> text;
		std::optional<int> start_time;
		std::optional<int> end_time;
	};

	struct AutomationTemplateSyllableSnapshot {
		std::optional<int> index;
		std::optional<std::string> text;
		std::optional<std::string> text_stripped;
		std::optional<std::string> inline_fx;
		std::optional<int> start_time;
		std::optional<int> end_time;
		std::optional<int> duration;
		std::optional<bool> is_furi;
		std::optional<double> left;
		std::optional<double> center;
		std::optional<double> right;
		std::optional<double> width;
		std::optional<double> height;
	};

	struct AutomationTemplateHighlightSnapshot {
		std::optional<int> index;
		std::optional<int> start_time;
		std::optional<int> end_time;
		std::optional<int> duration;
	};

	struct AutomationTemplateCharSnapshot {
		std::optional<int> index;
		std::optional<std::string> text;
	};

	struct AutomationTemplateSourceFragment {
		std::optional<int> source_line_index;
		std::optional<std::string> fragment_kind;
		std::optional<std::string> text;
		std::optional<std::string> effect;
	};

	struct AutomationTemplateIdentity {
		std::optional<std::string> owner_script;
		std::optional<int> template_debug_id;
		std::optional<std::string> template_kind;
		std::vector<std::string> template_kinds;
		std::optional<std::string> fragment_kind;
		std::optional<std::string> template_id;
		std::optional<int> source_line_index;
		std::vector<int> source_line_indices;
	};

	struct AutomationTemplateSource {
		std::optional<std::string> style;
		std::optional<std::string> effect;
		std::optional<std::string> text;
		std::vector<AutomationTemplateSourceFragment> fragments;
	};

	struct AutomationTemplateTargetSnapshot {
		std::optional<std::string> scope_kind;
		std::optional<AutomationTemplateLineSnapshot> original_line;
		std::optional<AutomationTemplateLineSnapshot> line;
		std::optional<AutomationTemplateSyllableSnapshot> syllable;
		std::optional<AutomationTemplateSyllableSnapshot> base_syllable;
		std::optional<AutomationTemplateHighlightSnapshot> highlight;
		std::optional<AutomationTemplateCharSnapshot> character;
	};

	struct AutomationGeneratedLineSnapshot {
		std::optional<int> generated_index;
		std::optional<std::string> text;
		std::optional<std::string> style;
		std::optional<int> layer;
		std::optional<std::string> effect;
		std::optional<int> start_time;
		std::optional<int> end_time;
		std::optional<int> source_line_index;
		std::optional<int> template_debug_id;
		std::optional<std::string> template_kind;
		std::optional<std::string> scope_kind;
		std::optional<int> syllable_index;
		std::optional<int> highlight_index;
		std::optional<int> char_index;
	};

	struct AutomationGeneratedLinesSnapshot {
		std::optional<int> count;
		std::optional<AutomationGeneratedLineSnapshot> last_line;
	};

	struct AutomationTemplateDebugState {
		std::optional<std::string> kind;
		std::optional<std::string> template_code;
		std::optional<std::string> template_text;
		std::optional<int> loop_index;
		std::optional<int> loop_count;
		std::optional<std::string> line_text;
		std::optional<std::string> line_style;
		std::optional<std::string> syllable_text;
		std::optional<int> syllable_index;
		std::optional<std::string> base_syllable_text;
		std::optional<std::string> phase;
		std::optional<std::string> scope_kind;
		std::optional<int> highlight_index;
		std::optional<int> char_index;
		std::optional<std::string> char_text;
		std::optional<std::string> expression;
		std::optional<std::string> parse_error;
		std::optional<std::string> runtime_error;
		std::optional<AutomationTemplateIdentity> identity;
		std::optional<AutomationTemplateSource> source;
		std::optional<AutomationTemplateTargetSnapshot> target;
		std::optional<AutomationGeneratedLinesSnapshot> generated;
	};
}
