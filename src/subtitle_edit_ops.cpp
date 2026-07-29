#include "subtitle_edit_ops.h"

#include <libaegisub/ass/dialogue_parser.h>
#include <libaegisub/string_utils.h>

#include <algorithm>
#include <string_view>
#include <utility>

namespace {

bool is_inside_override_block(std::string_view text, int pos) {
	pos = std::clamp(pos, 0, static_cast<int>(text.size()));
	if (pos == 0)
		return false;

	auto const last_open = text.rfind('{', static_cast<size_t>(pos - 1));
	if (last_open == std::string_view::npos)
		return false;

	auto const last_close = text.rfind('}', static_cast<size_t>(pos - 1));
	return last_close == std::string_view::npos || last_open > last_close;
}

bool has_char_at(std::string_view text, int pos, char value) {
	return pos >= 0 && pos < static_cast<int>(text.size()) && text[static_cast<size_t>(pos)] == value;
}

aegisub::subtitle_edit_ops::AutoCloseEdit make_replace_edit(int start, int end, std::string replacement, int caret) {
	return {true, start, end, std::move(replacement), caret};
}

bool match_trim_token_from_start(std::string const& text, size_t start, size_t& token_len) {
	if (start >= text.size())
		return false;

	if (text[start] == ' ' || text[start] == '\t') {
		token_len = 1;
		return true;
	}

	if (start + 1 < text.size() && text[start] == '\\') {
		char const next = text[start + 1];
		if (next == 'n' || next == 'N' || next == 'h') {
			token_len = 2;
			return true;
		}
	}

	return false;
}

bool match_trim_token_from_end(std::string const& text, size_t end, size_t& token_len) {
	if (end == 0)
		return false;

	if (text[end - 1] == ' ' || text[end - 1] == '\t') {
		token_len = 1;
		return true;
	}

	if (end >= 2 && text[end - 2] == '\\') {
		char const next = text[end - 1];
		if (next == 'n' || next == 'N' || next == 'h') {
			token_len = 2;
			return true;
		}
	}

	return false;
}

std::string trim_recombine_text(std::string text) {
	size_t begin = 0;
	size_t end = text.size();
	size_t token_len = 0;

	while (begin < end && match_trim_token_from_start(text, begin, token_len))
		begin += token_len;

	while (begin < end && match_trim_token_from_end(text, end, token_len))
		end -= token_len;

	return text.substr(begin, end - begin);
}

void expand_times(AssDialogue *src, AssDialogue *dst) {
	dst->Start = std::min(dst->Start, src->Start);
	dst->End = std::max(dst->End, src->End);
}

bool check_start(AssDialogue *line, AssDialogue *other) {
	if (!agi::util::strings::starts_with(line->Text.get(), other->Text.get()))
		return false;

	line->Text = trim_recombine_text(line->Text.get().substr(other->Text.get().size()));
	expand_times(line, other);
	return true;
}

bool check_end(AssDialogue *line, AssDialogue *other) {
	if (!agi::util::strings::ends_with(line->Text.get(), other->Text.get()))
		return false;

	line->Text = trim_recombine_text(line->Text.get().substr(0, line->Text.get().size() - other->Text.get().size()));
	expand_times(line, other);
	return true;
}

void apply_join(AssDialogue *first, AssDialogue *second, aegisub::subtitle_edit_ops::JoinMode mode) {
	switch (mode) {
	case aegisub::subtitle_edit_ops::JoinMode::Karaoke:
		if (second)
			first->Text = first->Text.get() + "{\\k" + std::to_string((second->End - second->Start) / 10) + "}" + second->Text.get();
		else
			first->Text = "{\\k" + std::to_string((first->End - first->Start) / 10) + "}" + first->Text.get();
		break;
	case aegisub::subtitle_edit_ops::JoinMode::Concatenate:
		if (second)
			first->Text = first->Text.get() + " " + second->Text.get();
		break;
	case aegisub::subtitle_edit_ops::JoinMode::KeepFirst:
		break;
	}
}

bool is_text_block_token(int type) {
	namespace dt = agi::ass::DialogueTokenType;

	switch (type) {
	case dt::TEXT:
	case dt::WORD:
	case dt::DRAWING:
	case dt::KARAOKE_TEMPLATE:
	case dt::KARAOKE_VARIABLE:
		return true;
	default:
		return false;
	}
}

}

namespace aegisub::subtitle_edit_ops {

bool JoinSelectionIntoFirst(std::vector<AssDialogue *> const& selection, JoinMode mode) {
	if (selection.empty())
		return false;

	AssDialogue *first = selection.front();
	apply_join(first, nullptr, mode);
	for (size_t i = 1; i < selection.size(); ++i) {
		apply_join(first, selection[i], mode);
		first->End = std::max(first->End, selection[i]->End);
	}

	return true;
}

RecombineResult RecombineSelection(std::vector<AssDialogue *> selection) {
	RecombineResult result;
	if (selection.size() < 2)
		return result;

	std::sort(selection.begin(), selection.end(), [](AssDialogue const* a, AssDialogue const* b) {
		return a->Start < b->Start;
	});

	for (auto *line : selection)
		line->Text = trim_recombine_text(line->Text.get());

	for (size_t i = 0; i + 1 < selection.size();) {
		auto *d1 = selection[i];
		auto *d2 = selection[i + 1];

		if (d1->Text == d2->Text) {
			expand_times(d1, d2);
			result.lines_to_remove.push_back(d1);
			selection.erase(selection.begin() + i);
			continue;
		}

		if (d1->Text.get().empty()) {
			result.lines_to_remove.push_back(d1);
			selection.erase(selection.begin() + i);
			continue;
		}

		if (i + 1 == selection.size() - 1 && d2->Text.get().empty()) {
			result.lines_to_remove.push_back(d2);
			selection.erase(selection.begin() + i + 1);
			continue;
		}

		size_t j = i + 1;

		while (j < selection.size() && check_start(selection[j], d1))
			++j;

		while (j < selection.size() && check_end(selection[j], d1))
			++j;

		while (j < selection.size() && check_end(d1, selection[j]))
			++j;

		while (j < selection.size() && check_start(d1, selection[j]))
			++j;

		++i;
	}

	return result;
}

std::pair<std::string, std::string> SplitTextAtPosition(std::string const& text, int pos) {
	size_t const split_pos = static_cast<size_t>(std::clamp(pos, 0, static_cast<int>(text.size())));
	return {
		agi::util::strings::trim_utf8_right_copy(std::string_view(text).substr(0, split_pos)),
		agi::util::strings::trim_utf8_left_copy(std::string_view(text).substr(split_pos))
	};
}

std::optional<int> EstimateSplitTime(int start_ms, int end_ms, std::string const& first_text, std::string const& second_text) {
	size_t const combined_length = first_text.size() + second_text.size();
	if (!combined_length)
		return std::nullopt;

	double const split_pos = static_cast<double>(first_text.size()) / combined_length;
	return static_cast<int>((end_ms - start_ms) * split_pos) + start_ms;
}

std::string BuildTagOnlyText(AssDialogue const& line) {
	auto blocks = line.ParseTags();
	std::string text;
	for (auto const& block : blocks) {
		if (block->GetType() != AssBlockType::PLAIN)
			text += block->GetText();
	}
	return text;
}

std::string ReplaceRangeWithText(std::string text, int start, int end, std::string const& replacement) {
	int const clamped_start_int = std::clamp(start, 0, static_cast<int>(text.size()));
	int const clamped_end_int = std::clamp(end, clamped_start_int, static_cast<int>(text.size()));
	size_t const clamped_start = static_cast<size_t>(clamped_start_int);
	size_t const clamped_end = static_cast<size_t>(clamped_end_int);
	agi::util::strings::replace_range_inplace(text, clamped_start, clamped_end, replacement);
	return text;
}

AutoCloseEdit BuildAutoCloseEdit(std::string_view text, int selection_start, int selection_end, AutoCloseKey key) {
	int const text_size = static_cast<int>(text.size());
	selection_start = std::clamp(selection_start, 0, text_size);
	selection_end = std::clamp(selection_end, 0, text_size);
	if (selection_start > selection_end)
		std::swap(selection_start, selection_end);

	if (selection_start != selection_end && key != AutoCloseKey::OpenBrace)
		return {};

	int const pos = selection_start;
	switch (key) {
	case AutoCloseKey::OpenBrace:
		if (selection_start != selection_end) {
			auto selected = text.substr(static_cast<size_t>(selection_start), static_cast<size_t>(selection_end - selection_start));
			return make_replace_edit(selection_start, selection_end, "{" + std::string(selected) + "}", selection_end + 2);
		}
		return make_replace_edit(pos, pos, "{}", pos + 1);
	case AutoCloseKey::OpenParen:
		if (is_inside_override_block(text, pos))
			return make_replace_edit(pos, pos, "()", pos + 1);
		return {};
	case AutoCloseKey::CloseBrace:
		if (has_char_at(text, pos, '}'))
			return make_replace_edit(pos, pos, "", pos + 1);
		return {};
	case AutoCloseKey::CloseParen:
		if (has_char_at(text, pos, ')') && is_inside_override_block(text, pos))
			return make_replace_edit(pos, pos, "", pos + 1);
		return {};
	case AutoCloseKey::Backspace:
		if (has_char_at(text, pos - 1, '{') && has_char_at(text, pos, '}'))
			return make_replace_edit(pos - 1, pos + 1, "", pos - 1);
		if (has_char_at(text, pos - 1, '(') && has_char_at(text, pos, ')') && is_inside_override_block(text, pos))
			return make_replace_edit(pos - 1, pos + 1, "", pos - 1);
		return {};
	}

	return {};
}

TextDragPreview BuildTextDragPreview(
	std::string_view text,
	int selection_start,
	int selection_end,
	int drop_position,
	bool copy) {
	int const text_length = static_cast<int>(text.size());
	selection_start = std::clamp(selection_start, 0, text_length);
	selection_end = std::clamp(selection_end, 0, text_length);
	if (selection_start > selection_end)
		std::swap(selection_start, selection_end);
	drop_position = std::clamp(drop_position, 0, text_length);

	TextDragPreview preview{std::string(text), selection_start, selection_end, false};
	if (selection_start == selection_end)
		return preview;

	bool const drop_inside_selection = drop_position > selection_start && drop_position < selection_end;
	bool const drop_on_selection_edge = drop_position == selection_start || drop_position == selection_end;
	if (drop_inside_selection || (!copy && drop_on_selection_edge))
		return preview;

	std::string const selected(text.substr(
		static_cast<size_t>(selection_start),
		static_cast<size_t>(selection_end - selection_start)));
	int insertion_position = drop_position;
	if (!copy) {
		preview.text.erase(
			static_cast<size_t>(selection_start),
			static_cast<size_t>(selection_end - selection_start));
		if (drop_position > selection_end)
			insertion_position -= selection_end - selection_start;
	}

	preview.text.insert(static_cast<size_t>(insertion_position), selected);
	preview.selection_start = insertion_position;
	preview.selection_end = insertion_position + static_cast<int>(selected.size());
	preview.changed = true;
	return preview;
}

int MapTextDragPreviewPosition(
	int preview_position,
	int text_length,
	int selection_start,
	int selection_end,
	int drop_position,
	bool copy,
	bool preview_changed) {
	text_length = std::max(text_length, 0);
	selection_start = std::clamp(selection_start, 0, text_length);
	selection_end = std::clamp(selection_end, 0, text_length);
	if (selection_start > selection_end)
		std::swap(selection_start, selection_end);
	drop_position = std::clamp(drop_position, 0, text_length);

	int const selection_length = selection_end - selection_start;
	int const preview_length = text_length + (copy && preview_changed ? selection_length : 0);
	preview_position = std::clamp(preview_position, 0, preview_length);
	if (!preview_changed || selection_length == 0)
		return std::min(preview_position, text_length);

	if (copy) {
		if (preview_position <= drop_position)
			return preview_position;
		if (preview_position <= drop_position + selection_length)
			return drop_position;
		return preview_position - selection_length;
	}

	if (drop_position < selection_start) {
		if (preview_position <= drop_position)
			return preview_position;
		if (preview_position <= drop_position + selection_length)
			return drop_position;
		if (preview_position < selection_end)
			return preview_position - selection_length;
		return preview_position;
	}

	if (preview_position <= selection_start)
		return preview_position;
	if (preview_position < drop_position - selection_length)
		return preview_position + selection_length;
	if (preview_position <= drop_position)
		return drop_position;
	return preview_position;
}

int GetPreviousBlockStart(std::vector<agi::ass::DialogueToken> const& tokens, int pos) {
	namespace dt = agi::ass::DialogueTokenType;

	if (tokens.empty() || pos <= 0)
		return 0;

	std::vector<int> block_starts;
	int offset = 0;
	bool in_override = false;
	bool in_text_block = false;

	auto add_block_start = [&] {
		if (block_starts.empty() || block_starts.back() != offset)
			block_starts.push_back(offset);
	};

	for (auto const& tok : tokens) {
		int const len = static_cast<int>(tok.length);

		if (!in_override) {
			if (tok.type == dt::OVR_BEGIN) {
				add_block_start();
				in_override = true;
				in_text_block = false;
			}
			else if (tok.type == dt::LINE_BREAK) {
				add_block_start();
				in_text_block = false;
			}
			else if (is_text_block_token(tok.type)) {
				if (!in_text_block)
					add_block_start();
				in_text_block = true;
			}
			else {
				in_text_block = false;
			}
		}
		else {
			if (tok.type == dt::OVR_END) {
				in_override = false;
				in_text_block = false;
			}
		}

		offset += len;
	}

	if (block_starts.empty())
		return 0;

	int target = 0;
	for (int bs : block_starts) {
		if (bs < pos)
			target = bs;
		else
			break;
	}

	return target;
}

int GetNextBlockEnd(std::vector<agi::ass::DialogueToken> const& tokens, int pos) {
	namespace dt = agi::ass::DialogueTokenType;

	if (tokens.empty())
		return 0;

	std::vector<int> block_ends;
	int offset = 0;
	bool in_override = false;
	bool in_text_block = false;

	auto add_block_end = [&] {
		if (block_ends.empty() || block_ends.back() != offset)
			block_ends.push_back(offset);
	};

	for (auto const& tok : tokens) {
		int const len = static_cast<int>(tok.length);

		if (!in_override) {
			if (is_text_block_token(tok.type)) {
				if (!in_text_block)
					in_text_block = true;
			}
			else {
				if (in_text_block) {
					add_block_end();
					in_text_block = false;
				}

				if (tok.type == dt::OVR_BEGIN) {
					in_override = true;
				}
				else if (tok.type == dt::LINE_BREAK) {
					offset += len;
					add_block_end();
					continue;
				}
			}
		}
		else {
			if (tok.type == dt::OVR_END) {
				offset += len;
				add_block_end();
				in_override = false;
				continue;
			}
		}

		offset += len;
	}

	if (in_text_block)
		add_block_end();

	if (block_ends.empty())
		return 0;

	int text_len = offset;
	for (int be : block_ends) {
		if (be > pos)
			return be;
	}

	return text_len;
}

}
