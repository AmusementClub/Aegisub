#include "ass_tag_scanner.h"

#include "ass_compat.h"

#include <cctype>
#include <charconv>

namespace aegisub::ass_tag_scanner {

bool NameHasPrefix(std::string_view name, std::string_view token) {
	return name.size() >= token.size() && name.substr(0, token.size()) == token;
}

bool NameIs(std::string_view name, std::string_view token) {
	if (!NameHasPrefix(name, token))
		return false;
	if (name.size() == token.size())
		return true;
	unsigned char const next = static_cast<unsigned char>(name[token.size()]);
	return !std::isalpha(next);
}

namespace {

std::string_view TrimArg(std::string_view value) {
	size_t begin = 0;
	while (begin < value.size() && (value[begin] == ' ' || value[begin] == '\t'))
		++begin;
	size_t end = value.size();
	while (end > begin && (value[end - 1] == ' ' || value[end - 1] == '\t'))
		--end;
	return value.substr(begin, end - begin);
}

}

std::vector<std::string_view> SplitLibassArgs(std::string_view region) {
	std::vector<std::string_view> parts;
	size_t q = 0;
	while (q < region.size()) {
		while (q < region.size() && (region[q] == ' ' || region[q] == '\t'))
			++q;
		size_t r = q;
		while (r < region.size() && region[r] != ',' && region[r] != '\\')
			++r;
		if (r < region.size() && region[r] == ',') {
			auto const part = TrimArg(region.substr(q, r - q));
			if (!part.empty())
				parts.push_back(part);
			q = r + 1;
			continue;
		}
		auto const part = TrimArg(region.substr(q));
		if (!part.empty())
			parts.push_back(part);
		break;
	}
	return parts;
}

double ArgToDouble(std::string_view value) {
	// AssCompat::ParseFloat is already this conversion: it skips leading
	// whitespace, accepts a leading '+', and parses through std::from_chars
	// (locale-independent by construction). It additionally rejects
	// non-finite results, which is what libass does too -- ass_strtod is a
	// digit/exponent parser with no inf/nan path, so it reads "nan" as 0 --
	// while a bare std::from_chars call accepts "nan", "inf" and
	// "infinity", and on MSVC writes inf even when it reports
	// result_out_of_range for "1e400". Leaving the zero in place on any
	// such failure matches both argtod's "conversion failure yields 0 and
	// is still a value" and AssOverrideParameter::Get<double>(), so the
	// same tag bytes now convert identically on the raw and parsed paths.
	double out = 0.0;
	AssCompat::ParseFloat(value, out);
	return out;
}

int ArgToInt(std::string_view value) {
	// Deliberately NOT AssCompat::ParseInteger: argtoi32 (ass_parse.c) hands
	// strtoll a base of 10, so libass reads "&H10" and "0x10" as 0 and 0,
	// while ParseInteger honors both prefixes as hex. ParseDecimalInteger is
	// base 10 but requires the whole subject to be consumed, where strtoll
	// takes the longest numeric prefix and ignores the rest. Neither is
	// equivalent, so this stays a separate conversion.
	//
	// The leading-whitespace skip is load-bearing rather than cosmetic: a
	// non-parenthesized tag hands its argument over untrimmed, so "\an 5"
	// arrives here as " 5" and libass renders it as alignment 5.
	// Parenthesized arguments are already trimmed by SplitLibassArgs.
	value = TrimArg(value);
	if (value.empty())
		return 0;
	if (value.front() == '+')
		value.remove_prefix(1);
	int out = 0;
	std::from_chars(value.data(), value.data() + value.size(), out);
	return out;
}

} // namespace aegisub::ass_tag_scanner
