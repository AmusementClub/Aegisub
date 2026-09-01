#pragma once

// A libass-faithful raw scanner for override-tag bytes. The prototype-table
// parser (AssOverrideTag::ParseTags) classifies tags by prefix-matching its
// proto table against each backslash span; libass itself skips spaces after
// the backslash, terminates arguments at the first closing parenthesis
// unless a backslash swallows the rest of the region, and re-parses \t
// argument regions as tag lists. Spellings like "{\ pos(12,34)}" or a
// position nested in \t are therefore real tags to libass and invisible to
// the proto table. Consumers that must agree with the renderer read their
// tags from here; see motion_track/apply_plan.cpp and
// perspective_ass_state.cpp.

#include <string_view>
#include <vector>

namespace aegisub::ass_tag_scanner {

/// One raw override tag, cut exactly where libass's ass_parse_tags stops
/// consuming: the backslash, any spaces after it, the name up to '(' / '\'
/// / end, then the parenthesized argument region through its terminator.
/// A '\' inside the region swallows everything through the next ')'
/// (memchr semantics), so "\t(\pos(1,2)\frz30)" scans as the \t tag with
/// args "\pos(1,2" followed by a separate top-level "\frz30)" -- the
/// trailing ')' is part of that name span because names only end at '(',
/// '\' or the end of the body.
struct RawTag {
	std::string_view bytes; // the consumed span, '\' included
	std::string_view name;  // the tag name as written, the '\' not included
	std::string_view args;  // inside the parens; empty when has_paren is false
	bool has_paren = false; // an argument region was opened
};

/// Scans one brace-free override-block body and visits every raw tag in
/// textual order; non-tag bytes are skipped. Does not recurse: for a tag
/// libass parses as a nested tag list (\t), re-invoke the scan on
/// raw.args. Depth capping stays with the caller -- 32 levels is the
/// established practice (perspective_ass_state.cpp, motion_track).
template <typename Visitor>
void ScanRawTags(std::string_view body, Visitor&& visit) {
	size_t i = 0;
	while (i < body.size()) {
		if (body[i] != '\\') {
			++i;
			continue;
		}
		size_t const begin = i;
		++i; // consume the backslash
		while (i < body.size() && (body[i] == ' ' || body[i] == '\t'))
			++i; // libass skips spaces/tabs between '\' and the tag name
		size_t const name_start = i;
		while (i < body.size() && body[i] != '(' && body[i] != '\\')
			++i;
		if (i == name_start)
			continue; // no name: '\' before '(' / '\' / end-of-body
		RawTag tag;
		tag.name = body.substr(name_start, i - name_start);
		size_t end = i;
		if (i < body.size() && body[i] == '(') {
			tag.has_paren = true;
			size_t const close = body.find(')', i + 1);
			tag.args = close == std::string_view::npos
				? body.substr(i + 1)
				: body.substr(i + 1, close - i - 1);
			end = close == std::string_view::npos ? body.size() : close + 1;
		}
		tag.bytes = body.substr(begin, end - begin);
		visit(tag);
		i = end;
	}
}

/// True when `name` (a RawTag::name, the '\' not included) has `token` as
/// a pure prefix -- libass's mystrcmp, so "\posx(1,2)" identifies as \pos
/// exactly as it does there.
bool NameHasPrefix(std::string_view name, std::string_view token);

/// True when `name` (a RawTag::name, the '\' not included) identifies
/// `token`: a prefix match that rejects a letter immediately after the
/// token, so "an" does not steal "\alpha". libass resolves the same
/// conflict by chain order (alpha is tested before a/an); callers that
/// classify a handful of tags use this rule instead of the full chain.
bool NameIs(std::string_view name, std::string_view token);

/// libass's argument split (ass_parse.c): commas until a backslash, which
/// swallows the rest of the region into one argument; surrounding spaces
/// are trimmed and empty arguments are not counted (push_arg).
std::vector<std::string_view> SplitLibassArgs(std::string_view region);

/// argtod/argtoi32 semantics: skip leading whitespace, accept a leading
/// '+', then parse the longest numeric prefix; conversion failure yields 0
/// and is still a value. The whitespace skip matters for the tags whose
/// argument is not parenthesized -- libass hands "\an 5" to strtoll as
/// " 5" and renders alignment 5 -- while parenthesized arguments arrive
/// already trimmed from SplitLibassArgs. Both are locale-independent, and
/// neither reads hex floats; ArgToDouble also rejects inf/nan subjects,
/// which ass_strtod has no path for and therefore reads as 0. ArgToDouble
/// is AssCompat::ParseFloat, so the raw and prototype-table paths convert
/// identical bytes identically; ArgToInt cannot share ParseInteger because
/// argtoi32 parses base 10 and would not take its '&H' / '0x' prefixes.
double ArgToDouble(std::string_view value);
int ArgToInt(std::string_view value);

} // namespace aegisub::ass_tag_scanner
