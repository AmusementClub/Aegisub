// Copyright (c) 2026
// Copyright (c) 2013, Thomas Goyne <plorkyeran@aegisub.org>
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

/// @file subtitle_matcher.cpp
/// @see subtitle_matcher.h

#include "subtitle_matcher.h"

#include "ass_dialogue.h"

#include <libaegisub/exception.h>
#include <libaegisub/string_utils.h>
#include <libaegisub/util.h>

#include <boost/locale/conversion.hpp>

namespace {
static const size_t bad_pos = -1;
static const MatchState bad_match{nullptr, 0, bad_pos};

auto get_dialogue_field(SearchReplaceSettings::Field field) -> decltype(&AssDialogueBase::Text) {
	switch (field) {
		case SearchReplaceSettings::Field::TEXT: return &AssDialogueBase::Text;
		case SearchReplaceSettings::Field::STYLE: return &AssDialogueBase::Style;
		case SearchReplaceSettings::Field::ACTOR: return &AssDialogueBase::Actor;
		case SearchReplaceSettings::Field::EFFECT: return &AssDialogueBase::Effect;
	}
	throw agi::InternalError("Bad field for search");
}

std::string const& get_field_text(const AssDialogue *diag, decltype(&AssDialogueBase::Text) field) {
	return (diag->*field).get();
}

typedef std::function<MatchState(const AssDialogue *, size_t)> matcher;

class noop_accessor {
	boost::flyweight<std::string> AssDialogueBase::*field;
	size_t start = 0;
	std::string normalized;
	bool do_normalize;

public:
	noop_accessor(SearchReplaceSettings::Field f, bool normalize)
	: field(get_dialogue_field(f)), do_normalize(normalize) { }

	agi::util::strings::view get_view(const AssDialogue *d, size_t s) {
		start = s;
		const auto& raw = get_field_text(d, field);
		normalized = do_normalize ? boost::locale::normalize(raw) : raw;
		return agi::util::strings::subview(normalized, s);
	}

	std::string get_string(const AssDialogue *d, size_t s) {
		return std::string(get_view(d, s));
	}

	MatchState make_match_state(size_t s, size_t e, boost::u32regex *r = nullptr) {
		return {r, s + start, e + start};
	}
};

class skip_tags_accessor {
	boost::flyweight<std::string> AssDialogueBase::*field;
	agi::util::tagless_find_helper helper;
	std::string normalized;
	std::string stripped;
	bool do_normalize;

public:
	skip_tags_accessor(SearchReplaceSettings::Field f, bool normalize)
	: field(get_dialogue_field(f)), do_normalize(normalize) { }

	agi::util::strings::view get_view(const AssDialogue *d, size_t s) {
		const auto& raw = get_field_text(d, field);
		normalized = do_normalize ? boost::locale::normalize(raw) : raw;
		stripped = helper.strip_tags(normalized, s);
		return stripped;
	}

	std::string get_string(const AssDialogue *d, size_t s) {
		return std::string(get_view(d, s));
	}

	MatchState make_match_state(size_t s, size_t e, boost::u32regex *r = nullptr) {
		helper.map_range(s, e);
		return {r, s, e};
	}
};

std::string prepare_search_text(SearchReplaceSettings const& settings) {
	if (!settings.use_unicode_escapes)
		return settings.find;

	std::string expanded;
	if (!agi::util::strings::expand_unicode_codepoint_escapes(settings.find, expanded))
		throw agi::InvalidInputException("Invalid Unicode escape. Use \\uXXXX, \\UXXXXXXXX, u+XXXX, or U+XXXX.");
	return expanded;
}

template<typename Accessor>
matcher get_matcher(SearchReplaceSettings const& settings, Accessor&& a) {
	// Match case: compare raw bytes, so do NOT normalize the needle (and the
	// accessor also skips normalization). Otherwise normalize so that visually
	// identical strings in different Unicode forms still match.
	std::string search_text = prepare_search_text(settings);
	std::string prepared_find = settings.match_case ? std::move(search_text)
	                                                : boost::locale::normalize(search_text);

	if (settings.use_regex) {
		int flags = boost::u32regex::perl;
		if (!settings.match_case)
			flags |= boost::u32regex::icase;

		auto regex = boost::make_u32regex(prepared_find, flags);

		return [=](const AssDialogue *diag, size_t start) mutable -> MatchState {
			boost::smatch result;
			auto str = a.get_string(diag, start);
			if (!u32regex_search(str, result, regex, start > 0 ? boost::match_not_bol : boost::match_default))
				return bad_match;
			return a.make_match_state(result.position(), result.position() + result.length(), &regex);
		};
	}

	bool full_match_only = settings.exact_match;
	bool match_case = settings.match_case;
	std::string look_for = std::move(prepared_find);
#ifdef AEGISUB_USE_STRINGZILLA
	agi::util::strings::utf8_icase_searcher icase_searcher{agi::util::strings::view(look_for)};
#endif

	return [=](const AssDialogue *diag, size_t start) mutable -> MatchState {
		const auto str = a.get_view(diag, start);
		// Rebuild the needle view from the captured string on each call: a
		// view taken over `look_for` before the closure would dangle once
		// get_matcher returns, since the view is not kept alive by capture.
		const agi::util::strings::view look_for_view(look_for);

		if (full_match_only) {
			if (match_case) {
				return str == look_for_view
					? a.make_match_state(0, str.size())
					: bad_match;
			}
#ifdef AEGISUB_USE_STRINGZILLA
			const auto match = icase_searcher.find(str);
			return match && match.offset == 0 && match.length == str.size()
				? a.make_match_state(0, str.size())
				: bad_match;
#else
			const auto pos = agi::util::ifind(std::string(str), look_for);
			return pos.first == 0 && pos.second == str.size()
				? a.make_match_state(pos.first, pos.second)
				: bad_match;
#endif
		}

		if (match_case) {
			const auto pos = agi::util::strings::find(str, look_for_view);
			return pos == agi::util::strings::npos ? bad_match : a.make_match_state(pos, pos + look_for_view.size());
		}

#ifdef AEGISUB_USE_STRINGZILLA
		const auto match = icase_searcher.find(str);
		return match
			? a.make_match_state(match.offset, match.offset + match.length)
			: bad_match;
#else
		const auto pos = agi::util::ifind(std::string(str), look_for);
		return pos.first == bad_pos ? bad_match : a.make_match_state(pos.first, pos.second);
#endif
	};
}

} // namespace

std::function<MatchState(const AssDialogue *, size_t)>
MakeSubtitleMatcher(SearchReplaceSettings const& settings) {
	const bool normalize = !settings.match_case;
	if (settings.skip_tags)
		return get_matcher(settings, skip_tags_accessor(settings.field, normalize));
	return get_matcher(settings, noop_accessor(settings.field, normalize));
}
