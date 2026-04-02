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
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

#include "headless_cli_internal.h"
#include "headless_cli_parse.h"

#include <libaegisub/fs.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <utility>

namespace headless_cli::detail {
namespace {

bool TryParseInteger(std::string const& value) {
	if (value.empty())
		return false;
	size_t index = (value[0] == '-' || value[0] == '+') ? 1 : 0;
	if (index == value.size())
		return false;
	for (; index < value.size(); ++index) {
		if (!std::isdigit(static_cast<unsigned char>(value[index])))
			return false;
	}
	return true;
}

bool TryParseFloat(std::string const& value) {
	if (value.empty() || value.find_first_of(".eE") == std::string::npos)
		return false;
	char *end = nullptr;
	std::strtod(value.c_str(), &end);
	return end && *end == '\0';
}

void AppendJsonTypedValue(std::ostringstream& out, std::string const& value) {
	if (value == "true" || value == "false") {
		out << value;
		return;
	}
	if (TryParseInteger(value) || TryParseFloat(value)) {
		out << value;
		return;
	}
	out << '"' << JsonEscape(value) << '"';
}

std::string KeyValueMapToJson(std::map<std::string, std::string> const& values, int indent) {
	std::ostringstream out;
	std::string padding(indent, ' ');
	std::string next_padding(indent + 2, ' ');
	out << "{\n";
	bool first = true;
	for (auto const& [key, value] : values) {
		if (!first)
			out << ",\n";
		first = false;
		out << next_padding << '"' << JsonEscape(key) << "\": ";
		AppendJsonTypedValue(out, value);
	}
	if (!values.empty())
		out << '\n';
	out << padding << '}';
	return out.str();
}

}

std::string JsonEscape(std::string const& input) {
	std::string escaped;
	escaped.reserve(input.size());
	for (unsigned char c : input) {
		switch (c) {
		case '\\': escaped += "\\\\"; break;
		case '"': escaped += "\\\""; break;
		case '\b': escaped += "\\b"; break;
		case '\f': escaped += "\\f"; break;
		case '\n': escaped += "\\n"; break;
		case '\r': escaped += "\\r"; break;
		case '\t': escaped += "\\t"; break;
		default:
			if (c < 0x20) {
				char buffer[7];
				snprintf(buffer, sizeof(buffer), "\\u%04X", static_cast<unsigned>(c));
				escaped += buffer;
			}
			else {
				escaped += static_cast<char>(c);
			}
			break;
		}
	}
	return escaped;
}

std::string ToGenericString(agi::fs::path const& path) {
	return agi::fs::PathToGenericString(path);
}

std::optional<std::string> RequireValue(std::vector<std::string> const& args, size_t& index, std::string const& flag, std::string& error) {
	if (index + 1 >= args.size()) {
		error = flag + " requires a value\n" + Usage();
		return std::nullopt;
	}
	++index;
	return args[index];
}

std::string BuildTraceInspectJson(aegisub::trace_inspect_service::TraceSessionSummary const& session) {
	std::ostringstream out;
	out << "{\n";
	out << "  \"session_dir\": \"" << JsonEscape(ToGenericString(session.session_dir)) << "\",\n";
	out << "  \"manifest\": " << KeyValueMapToJson(session.manifest, 2) << ",\n";
	out << "  \"summary\": " << KeyValueMapToJson(session.summary, 2) << "\n";
	out << "}\n";
	return out.str();
}

std::string Trim(std::string value) {
	auto const not_space = [](unsigned char ch) { return !std::isspace(ch); };
	value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
	value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
	return value;
}

std::vector<std::string> SplitWhitespace(std::string const& text) {
	std::istringstream in(text);
	std::vector<std::string> tokens;
	std::string token;
	while (in >> token)
		tokens.push_back(token);
	return tokens;
}

std::optional<int> ParseIntegerValue(std::string const& text) {
	if (!TryParseInteger(text))
		return std::nullopt;
	try {
		return std::stoi(text);
	}
	catch (...) {
		return std::nullopt;
	}
}

std::optional<bool> ParseBoolValue(std::string value) {
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
	if (value == "true")
		return true;
	if (value == "false")
		return false;
	return std::nullopt;
}

std::optional<aegisub::playback_session_service::PlaybackAuthorityKind> ParseAuthorityValue(std::string value) {
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
	if (value == "audio")
		return aegisub::playback_session_service::PlaybackAuthorityKind::Audio;
	if (value == "video")
		return aegisub::playback_session_service::PlaybackAuthorityKind::Video;
	return std::nullopt;
}

}
