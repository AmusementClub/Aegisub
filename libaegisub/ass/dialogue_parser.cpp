// Copyright (c) 2012, Thomas Goyne <plorkyeran@aegisub.org>
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
//
// Aegisub Project http://www.aegisub.org/

#include "libaegisub/ass/dialogue_parser.h"

#include "libaegisub/ass/drawing.h"
#include "libaegisub/spellchecker.h"

#include <boost/locale/boundary/index.hpp>
#include <boost/locale/boundary/segment.hpp>
#include <boost/locale/boundary/types.hpp>

#include <string_view>

namespace {

typedef std::vector<agi::ass::DialogueToken> TokenVec;
using namespace agi::ass;
namespace dt = DialogueTokenType;
namespace ss = SyntaxStyle;

bool IsClipTag(std::string const& str, size_t pos, size_t len) {
	return (len == 4 && str.compare(pos, 4, "clip") == 0) ||
		(len == 5 && str[pos] == 'i' && str.compare(pos + 1, 4, "clip") == 0);
}

bool TryMarkVectorClipDrawing(std::string const& str, std::vector<DialogueToken>& tokens, size_t tag_index, size_t tag_pos, size_t& last_ovr_end) {
	size_t len = tokens[tag_index].length;
	if (!IsClipTag(str, tag_pos, len) || tag_index + 3 >= last_ovr_end || tokens[tag_index + 1].type != dt::OPEN_PAREN)
		return false;

	size_t drawing_start = 0;
	size_t drawing_end = 0;

	for (size_t j = tag_index + 2; j < last_ovr_end; ++j) {
		if (tokens[j].type == dt::ARG_SEP) {
			if (drawing_start)
				break;
			drawing_start = j + 1;
		}
		else if (tokens[j].type == dt::CLOSE_PAREN) {
			drawing_end = j;
			break;
		}
		else if (tokens[j].type != dt::WHITESPACE && tokens[j].type != dt::ARG) {
			break;
		}
	}

	if (!drawing_end)
		return false;
	if (!drawing_start)
		drawing_start = tag_index + 2;
	if (drawing_end == drawing_start)
		return false;

	size_t token_len = 0;
	for (size_t j = drawing_start; j < drawing_end; ++j)
		token_len += tokens[j].length;

	tokens[drawing_start].length = token_len;
	tokens[drawing_start].type = dt::DRAWING;
	tokens.erase(tokens.begin() + drawing_start + 1, tokens.begin() + drawing_end);
	last_ovr_end -= drawing_end - drawing_start - 1;
	return true;
}

class SyntaxHighlighter {
	TokenVec ranges;
	std::string const& text;
	agi::SpellChecker *spellchecker;

	void SetStyling(size_t len, int type) {
		if (ranges.size() && ranges.back().type == type)
			ranges.back().length += len;
		else
			ranges.push_back(DialogueToken{type, len});
	}

	void HighlightDrawing(size_t pos, size_t len) {
		for (auto const& lexeme : drawing::LexDrawing(std::string_view(text).substr(pos, len))) {
			switch (lexeme.type) {
				case drawing::LexemeType::Normal:    SetStyling(lexeme.length, ss::NORMAL);             break;
				case drawing::LexemeType::Command:   SetStyling(lexeme.length, ss::DRAWING_CMD);        break;
				case drawing::LexemeType::X:         SetStyling(lexeme.length, ss::DRAWING_X);          break;
				case drawing::LexemeType::Y:         SetStyling(lexeme.length, ss::DRAWING_Y);          break;
				case drawing::LexemeType::EndpointX: SetStyling(lexeme.length, ss::DRAWING_ENDPOINT_X); break;
				case drawing::LexemeType::EndpointY: SetStyling(lexeme.length, ss::DRAWING_ENDPOINT_Y); break;
				case drawing::LexemeType::Error:     SetStyling(lexeme.length, ss::ERROR);              break;
			}
		}
	}

public:
	SyntaxHighlighter(std::string const& text, agi::SpellChecker *spellchecker)
	: text(text)
	, spellchecker(spellchecker)
	{ }

	TokenVec Highlight(TokenVec const& tokens) {
		if (tokens.empty()) return ranges;

		size_t pos = 0;

		for (auto tok : tokens) {
			switch (tok.type) {
				case dt::KARAOKE_TEMPLATE: SetStyling(tok.length, ss::KARAOKE_TEMPLATE); break;
				case dt::KARAOKE_VARIABLE: SetStyling(tok.length, ss::KARAOKE_VARIABLE); break;
				case dt::LINE_BREAK: SetStyling(tok.length, ss::LINE_BREAK); break;
				case dt::ERROR:      SetStyling(tok.length, ss::ERROR);      break;
				case dt::ARG:        SetStyling(tok.length, ss::PARAMETER);  break;
				case dt::COMMENT:    SetStyling(tok.length, ss::COMMENT);    break;
				case dt::DRAWING:    HighlightDrawing(pos, tok.length);      break;
				case dt::TEXT:       SetStyling(tok.length, ss::NORMAL);     break;
				case dt::TAG_NAME:   SetStyling(tok.length, ss::TAG);        break;
				case dt::OPEN_PAREN: case dt::CLOSE_PAREN: case dt::ARG_SEP: case dt::TAG_START:
					SetStyling(tok.length, ss::PUNCTUATION);
					break;
				case dt::OVR_BEGIN: case dt::OVR_END:
					SetStyling(tok.length, ss::OVERRIDE);
					break;
				case dt::WHITESPACE:
					if (ranges.size() && ranges.back().type == ss::PARAMETER)
						SetStyling(tok.length, ss::PARAMETER);
					else
						SetStyling(tok.length, ss::NORMAL);
					break;
				case dt::WORD:
					if (spellchecker && !spellchecker->CheckWord(text.substr(pos, tok.length)))
						SetStyling(tok.length, ss::SPELLING);
					else
						SetStyling(tok.length, ss::NORMAL);
					break;
			}

			pos += tok.length;
		}

		return ranges;
	}
};

class WordSplitter {
	std::string const& text;
	std::vector<DialogueToken> &tokens;
	size_t pos = 0;

	void SwitchTo(size_t &i, int type, size_t len) {
		auto old = tokens[i];
		tokens[i].type = type;
		tokens[i].length = len;

		if (old.length != (size_t)len) {
			tokens.insert(tokens.begin() + i + 1, DialogueToken{old.type, old.length - len});
			++i;
		}
	}

	void SplitText(size_t &i) {
		using namespace boost::locale::boundary;
		ssegment_index map(word, text.begin() + pos, text.begin() + pos + tokens[i].length);
		for (auto const& segment : map) {
			auto len = static_cast<size_t>(distance(begin(segment), end(segment)));
			if (segment.rule() & word_letters)
				SwitchTo(i, dt::WORD, len);
			else
				SwitchTo(i, dt::TEXT, len);
		}
	}

public:
	WordSplitter(std::string const& text, std::vector<DialogueToken> &tokens)
	: text(text)
	, tokens(tokens)
	{ }

	void SplitWords() {
		if (tokens.empty()) return;

		for (size_t i = 0; i < tokens.size(); ++i) {
			size_t len = tokens[i].length;
			if (tokens[i].type == dt::TEXT)
				SplitText(i);
			pos += len;
		}
	}
};
}

namespace agi {
namespace ass {

std::vector<DialogueToken> SyntaxHighlight(std::string const& text, std::vector<DialogueToken> const& tokens, SpellChecker *spellchecker) {
	return SyntaxHighlighter(text, spellchecker).Highlight(tokens);
}

void MarkDrawings(std::string const& str, std::vector<DialogueToken> &tokens) {
	if (tokens.empty()) return;

	size_t last_ovr_end = 0;
	for (size_t i = tokens.size(); i > 0; --i) {
		if (tokens[i - 1].type == dt::OVR_END) {
			last_ovr_end = i;
			break;
		}
	}

	size_t pos = 0;
	bool in_drawing = false;

	for (size_t i = 0; i < last_ovr_end; ++i) {
		size_t len = tokens[i].length;
		switch (tokens[i].type) {
			case dt::TEXT:
				if (in_drawing)
					tokens[i].type = dt::DRAWING;
				break;
			case dt::TAG_NAME:
				TryMarkVectorClipDrawing(str, tokens, i, pos, last_ovr_end);

				if (len != 1 || i + 1 >= tokens.size() || str[pos] != 'p')
					break;

				in_drawing = false;

				if (i + 1 == last_ovr_end || tokens[i + 1].type != dt::ARG)
					break;

				for (size_t j = pos + len; j < pos + len + tokens[i + 1].length; ++j) {
					char c = str[j];
					// I have no idea why one would use leading zeros for
					// the scale, but vsfilter allows it
					if (c >= '1' && c <= '9')
						in_drawing = true;
					else if (c != '0')
						break;
				}
				break;
			default: break;
		}

		pos += len;
	}

	// VSFilter treats unclosed override blocks as plain text, so merge all
	// the tokens after the last override block into a single TEXT (or DRAWING)
	// token
	for (size_t i = last_ovr_end; i < tokens.size(); ++i) {
		switch (tokens[i].type) {
			case dt::KARAOKE_TEMPLATE: break;
			case dt::KARAOKE_VARIABLE: break;
			case dt::LINE_BREAK: break;
			default:
				tokens[i].type = in_drawing ? dt::DRAWING : dt::TEXT;
				if (i > 0 && tokens[i - 1].type == tokens[i].type) {
					tokens[i - 1].length += tokens[i].length;
					tokens.erase(tokens.begin() + i);
					--i;
				}
		}
	}
}

void SplitWords(std::string const& str, std::vector<DialogueToken> &tokens) {
	MarkDrawings(str, tokens);
	WordSplitter(str, tokens).SplitWords();
}

}
}
