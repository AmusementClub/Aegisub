#pragma once

#include "ass_dialogue.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace agi::ass {
	struct DialogueToken;
}

namespace aegisub::subtitle_edit_ops {

enum class JoinMode {
	Karaoke,
	Concatenate,
	KeepFirst
};

struct RecombineResult {
	std::vector<AssDialogue *> lines_to_remove;
};

enum class AutoCloseKey {
	OpenBrace,
	CloseBrace,
	OpenParen,
	CloseParen,
	Backspace
};

struct AutoCloseEdit {
	bool handled = false;
	int replace_start = 0;
	int replace_end = 0;
	std::string replacement;
	int caret = 0;
};

struct TextDragPreview {
	std::string text;
	int selection_start = 0;
	int selection_end = 0;
	bool changed = false;
};

bool JoinSelectionIntoFirst(std::vector<AssDialogue *> const& selection, JoinMode mode);
RecombineResult RecombineSelection(std::vector<AssDialogue *> selection);
std::pair<std::string, std::string> SplitTextAtPosition(std::string const& text, int pos);
std::optional<int> EstimateSplitTime(int start_ms, int end_ms, std::string const& first_text, std::string const& second_text);
std::string BuildTagOnlyText(AssDialogue const& line);
std::string ReplaceRangeWithText(std::string text, int start, int end, std::string const& replacement);
AutoCloseEdit BuildAutoCloseEdit(std::string_view text, int selection_start, int selection_end, AutoCloseKey key);

/// Build the document shown while selected text is dragged over a new position.
TextDragPreview BuildTextDragPreview(
	std::string_view text,
	int selection_start,
	int selection_end,
	int drop_position,
	bool copy);

/// Map a position in a drag preview back to the unmodified source document.
int MapTextDragPreviewPosition(
	int preview_position,
	int text_length,
	int selection_start,
	int selection_end,
	int drop_position,
	bool copy,
	bool preview_changed);

/// Get the start position to move to when pressing Home at the given position.
/// Blocks are override tags ({...}), line breaks (\N, \n), and runs of text.
int GetPreviousBlockStart(std::vector<agi::ass::DialogueToken> const& tokens, int pos);

/// Get the end position to move to when pressing End at the given position.
/// Blocks are override tags ({...}), line breaks (\N, \n), and runs of text.
int GetNextBlockEnd(std::vector<agi::ass::DialogueToken> const& tokens, int pos);

}
