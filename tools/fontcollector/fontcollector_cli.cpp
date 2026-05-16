// Copyright (c) 2026, MIRIMIRIM

#include <aegisub/fontcollector/fontcollector.h>

#include <CLI/CLI.hpp>

#include <array>
#include <cstdio>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#endif

namespace {
#ifdef _WIN32
std::string WideToUtf8(std::wstring_view value) {
	if (value.empty())
		return {};

	auto len = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
	std::string text(len, '\0');
	WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), text.data(), len, nullptr, nullptr);
	return text;
}

std::vector<std::string> GetUtf8CommandLineArgs() {
	int argc = 0;
	auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	if (!argv)
		return {};

	std::vector<std::string> args;
	args.reserve(argc);
	for (int i = 0; i < argc; ++i)
		args.push_back(WideToUtf8(argv[i]));

	LocalFree(argv);
	return args;
}

std::vector<char *> MakeArgv(std::vector<std::string>& args) {
	std::vector<char *> argv;
	argv.reserve(args.size());
	for (auto& arg : args)
		argv.push_back(arg.data());
	return argv;
}
#endif

std::string Safe(char const *text) {
	return text ? std::string(text) : std::string();
}

std::string JsonEscape(std::string const& input) {
	std::string escaped;
	escaped.reserve(input.size());
	for (unsigned char ch : input) {
		switch (ch) {
			case '\\': escaped += "\\\\"; break;
			case '"': escaped += "\\\""; break;
			case '\b': escaped += "\\b"; break;
			case '\f': escaped += "\\f"; break;
			case '\n': escaped += "\\n"; break;
			case '\r': escaped += "\\r"; break;
			case '\t': escaped += "\\t"; break;
			default:
				if (ch < 0x20) {
					char buffer[7];
					std::snprintf(buffer, sizeof(buffer), "\\u%04X", ch);
					escaped += buffer;
				}
				else
					escaped += static_cast<char>(ch);
		}
	}
	return escaped;
}

void WriteJsonString(std::ostream& out, std::string const& value) {
	out << '"' << JsonEscape(value) << '"';
}

std::string EventTypeName(AegisubFontCollectorEventType type) {
	switch (type) {
		case AEGISUB_FONTCOLLECTOR_EVENT_FONT_BACKEND_INFO: return "font_backend_info";
		case AEGISUB_FONTCOLLECTOR_EVENT_UPDATING_FONT_CACHE: return "updating_font_cache";
		case AEGISUB_FONTCOLLECTOR_EVENT_FONT_CACHE_ERROR: return "font_cache_error";
		case AEGISUB_FONTCOLLECTOR_EVENT_PARSING_FILE: return "parsing_file";
		case AEGISUB_FONTCOLLECTOR_EVENT_STYLE_MISSING: return "style_missing";
		case AEGISUB_FONTCOLLECTOR_EVENT_SEARCHING_FOR_FONT_FILES: return "searching_for_font_files";
		case AEGISUB_FONTCOLLECTOR_EVENT_FONT_MISSING: return "font_missing";
		case AEGISUB_FONTCOLLECTOR_EVENT_FONT_FOUND: return "font_found";
		case AEGISUB_FONTCOLLECTOR_EVENT_FAKE_BOLD: return "fake_bold";
		case AEGISUB_FONTCOLLECTOR_EVENT_FAKE_ITALIC: return "fake_italic";
		case AEGISUB_FONTCOLLECTOR_EVENT_MISSING_GLYPHS: return "missing_glyphs";
		case AEGISUB_FONTCOLLECTOR_EVENT_USAGE: return "usage";
		case AEGISUB_FONTCOLLECTOR_EVENT_SEARCH_COMPLETE: return "search_complete";
		case AEGISUB_FONTCOLLECTOR_EVENT_ALL_FONTS_FOUND: return "all_fonts_found";
		case AEGISUB_FONTCOLLECTOR_EVENT_FONTS_MISSING: return "fonts_missing";
		case AEGISUB_FONTCOLLECTOR_EVENT_FONTS_MISSING_GLYPHS: return "fonts_missing_glyphs";
		case AEGISUB_FONTCOLLECTOR_EVENT_COLLECTION_SYMLINKING_FONTS_TO_FOLDER: return "collection_symlinking_fonts_to_folder";
		case AEGISUB_FONTCOLLECTOR_EVENT_COLLECTION_COPYING_FONTS_TO_FOLDER: return "collection_copying_fonts_to_folder";
		case AEGISUB_FONTCOLLECTOR_EVENT_COLLECTION_COPYING_FONTS_TO_ARCHIVE: return "collection_copying_fonts_to_archive";
		case AEGISUB_FONTCOLLECTOR_EVENT_COLLECTION_FAILED_CREATE_DIRECTORY: return "collection_failed_create_directory";
		case AEGISUB_FONTCOLLECTOR_EVENT_COLLECTION_FAILED_OPEN: return "collection_failed_open";
		case AEGISUB_FONTCOLLECTOR_EVENT_COLLECTION_COPIED: return "collection_copied";
		case AEGISUB_FONTCOLLECTOR_EVENT_COLLECTION_ALREADY_EXISTS: return "collection_already_exists";
		case AEGISUB_FONTCOLLECTOR_EVENT_COLLECTION_SYMLINKED: return "collection_symlinked";
		case AEGISUB_FONTCOLLECTOR_EVENT_COLLECTION_FAILED_COPY: return "collection_failed_copy";
		case AEGISUB_FONTCOLLECTOR_EVENT_COLLECTION_DONE_ALL_COPIED: return "collection_done_all_copied";
		case AEGISUB_FONTCOLLECTOR_EVENT_COLLECTION_DONE_SOME_NOT_COPIED: return "collection_done_some_not_copied";
		case AEGISUB_FONTCOLLECTOR_EVENT_COLLECTION_OVER_32MB_WARNING: return "collection_over_32mb_warning";
		case AEGISUB_FONTCOLLECTOR_EVENT_COLLECTION_NEWLINE: return "collection_newline";
	}
	return "unknown";
}

std::string FormatCodepoint(uint32_t value) {
	std::ostringstream out;
	out << "U+" << std::uppercase << std::hex << value;
	return out.str();
}

struct JsonEvent {
	AegisubFontCollectorEventType type = AEGISUB_FONTCOLLECTOR_EVENT_PARSING_FILE;
	std::string text;
	std::string face;
	std::string message;
	std::string style;
	std::string path;
	std::vector<std::string> styles;
	std::vector<int> lines;
	int count = 0;
	int requested_weight = 0;
	int requested_italic = 0;
};

struct JsonMatchedFont {
	std::string facename;
	std::string facename_full;
	int face_index = -1;
	int weight = 0;
	bool bold = false;
	bool italic = false;
	bool is_collection = false;
	std::string path_source;
	std::vector<std::string> paths;
	bool fake_bold = false;
	bool fake_italic = false;
	/// libass-style synthetic detection from platform-neutral common layer (opt-in)
	bool libass_fake_bold = false;
	bool libass_fake_italic = false;
	int libass_score = 0;
	std::string missing_text;
	std::vector<uint32_t> missing_codepoints;
	std::vector<std::string> missing_codepoint_names;
	int requested_weight = 0;
};

struct JsonUsage {
	std::string ass_facename;
	int ass_bold = 0;
	bool ass_italic = false;
	std::vector<uint32_t> codepoints;
	std::vector<std::string> codepoint_names;
	std::vector<std::string> styles;
	std::vector<int> override_lines;
	JsonMatchedFont matched;
};

struct JsonContext {
	std::string requested_backend;
	std::string resolved_backend;
	std::vector<JsonEvent> events;
	std::vector<JsonUsage> usages;
};

std::string RequestedBackendName(AegisubFontCollectorBackend backend) {
	switch (backend) {
		case AEGISUB_FONTCOLLECTOR_BACKEND_AUTO: return "auto";
		case AEGISUB_FONTCOLLECTOR_BACKEND_PLATFORM_DEFAULT: return "platform";
		case AEGISUB_FONTCOLLECTOR_BACKEND_FONTCONFIG: return "fontconfig";
		case AEGISUB_FONTCOLLECTOR_BACKEND_CORETEXT: return "coretext";
	}
	return "unknown";
}

std::string PlatformBackendName() {
#if defined(_WIN32)
	return "gdi-dwrite";
#elif defined(__APPLE__)
	return "coretext";
#else
	return "fontconfig";
#endif
}

std::string ResolvedBackendName(AegisubFontCollectorBackend backend) {
	switch (backend) {
		case AEGISUB_FONTCOLLECTOR_BACKEND_AUTO:
		case AEGISUB_FONTCOLLECTOR_BACKEND_PLATFORM_DEFAULT:
			return PlatformBackendName();
		case AEGISUB_FONTCOLLECTOR_BACKEND_FONTCONFIG:
			return "fontconfig";
		case AEGISUB_FONTCOLLECTOR_BACKEND_CORETEXT:
			return "coretext";
	}
	return "unknown";
}

std::string JoinStyles(AegisubFontCollectorEvent const& event) {
	std::ostringstream out;
	for (size_t i = 0; i < event.style_count; ++i) {
		if (i)
			out << ", ";
		out << event.styles[i];
	}
	return out.str();
}

std::string JoinLines(AegisubFontCollectorEvent const& event) {
	std::ostringstream out;
	for (size_t i = 0; i < event.line_count; ++i) {
		if (i)
			out << ", ";
		out << event.lines[i];
	}
	return out.str();
}

std::string FormatEvent(AegisubFontCollectorEvent const& event) {
	switch (event.type) {
		case AEGISUB_FONTCOLLECTOR_EVENT_FONT_BACKEND_INFO:
			return "Font backend: " + Safe(event.message);
		case AEGISUB_FONTCOLLECTOR_EVENT_UPDATING_FONT_CACHE:
			return "Updating font cache";
		case AEGISUB_FONTCOLLECTOR_EVENT_FONT_CACHE_ERROR:
			return "Font cache error: " + Safe(event.message);
		case AEGISUB_FONTCOLLECTOR_EVENT_PARSING_FILE:
			return "Parsing file";
		case AEGISUB_FONTCOLLECTOR_EVENT_STYLE_MISSING:
			return "Style missing: " + Safe(event.style) +
			       (event.line_count == 1 ? " on line " + JoinLines(event) :
			        event.line_count > 1 ? " on lines " + JoinLines(event) : std::string());
		case AEGISUB_FONTCOLLECTOR_EVENT_SEARCHING_FOR_FONT_FILES:
			return "Searching for font files";
		case AEGISUB_FONTCOLLECTOR_EVENT_FONT_MISSING:
			return "Missing font: " + Safe(event.face);
		case AEGISUB_FONTCOLLECTOR_EVENT_FONT_FOUND: {
			auto src = Safe(event.message);
			if (src == "memory")
				return "Found font: " + Safe(event.face) + " (memory)";
			return "Found font: " + Safe(event.face) + " -> " + Safe(event.path) +
			       (src.empty() ? "" : " [" + src + "]");
		}
		case AEGISUB_FONTCOLLECTOR_EVENT_FAKE_BOLD:
			return "Fake bold required: " + Safe(event.face) + (event.requested_weight ? " (requested " + std::to_string(event.requested_weight) + ")" : std::string());
		case AEGISUB_FONTCOLLECTOR_EVENT_FAKE_ITALIC:
			return "Fake italic required: " + Safe(event.face);
		case AEGISUB_FONTCOLLECTOR_EVENT_MISSING_GLYPHS:
			return "Missing glyphs in " + Safe(event.face) + ": " +
			       (event.message && *event.message ? Safe(event.message) : std::to_string(event.count));
		case AEGISUB_FONTCOLLECTOR_EVENT_USAGE: {
			std::ostringstream out;
			if (event.style_count)
				out << "Used in styles: " << JoinStyles(event);
			if (event.line_count) {
				if (event.style_count)
					out << "; ";
				out << "Used on lines: " << JoinLines(event);
			}
			return out.str();
		}
		case AEGISUB_FONTCOLLECTOR_EVENT_SEARCH_COMPLETE:
			return "Search complete";
		case AEGISUB_FONTCOLLECTOR_EVENT_ALL_FONTS_FOUND:
			return "All fonts found";
		case AEGISUB_FONTCOLLECTOR_EVENT_FONTS_MISSING:
			return "Fonts missing: " + std::to_string(event.count);
		case AEGISUB_FONTCOLLECTOR_EVENT_FONTS_MISSING_GLYPHS:
			return "Fonts with missing glyphs: " + std::to_string(event.count);
		case AEGISUB_FONTCOLLECTOR_EVENT_COLLECTION_SYMLINKING_FONTS_TO_FOLDER:
			return "Symlinking fonts to folder";
		case AEGISUB_FONTCOLLECTOR_EVENT_COLLECTION_COPYING_FONTS_TO_FOLDER:
			return "Copying fonts to folder";
		case AEGISUB_FONTCOLLECTOR_EVENT_COLLECTION_COPYING_FONTS_TO_ARCHIVE:
			return "Copying fonts to archive";
		case AEGISUB_FONTCOLLECTOR_EVENT_COLLECTION_FAILED_CREATE_DIRECTORY:
			return "Failed to create directory: " + Safe(event.path) + " " + Safe(event.message);
		case AEGISUB_FONTCOLLECTOR_EVENT_COLLECTION_FAILED_OPEN:
			return "Failed to open: " + Safe(event.path);
		case AEGISUB_FONTCOLLECTOR_EVENT_COLLECTION_COPIED:
			return "Copied: " + Safe(event.path);
		case AEGISUB_FONTCOLLECTOR_EVENT_COLLECTION_ALREADY_EXISTS:
			return "Already exists at destination: " + Safe(event.path);
		case AEGISUB_FONTCOLLECTOR_EVENT_COLLECTION_SYMLINKED:
			return "Symlinked: " + Safe(event.path);
		case AEGISUB_FONTCOLLECTOR_EVENT_COLLECTION_FAILED_COPY:
			return "Failed to copy: " + Safe(event.path);
		case AEGISUB_FONTCOLLECTOR_EVENT_COLLECTION_DONE_ALL_COPIED:
			return "Done. All fonts copied";
		case AEGISUB_FONTCOLLECTOR_EVENT_COLLECTION_DONE_SOME_NOT_COPIED:
			return "Done. Some fonts could not be copied";
		case AEGISUB_FONTCOLLECTOR_EVENT_COLLECTION_OVER_32MB_WARNING:
			return "Warning: copied fonts exceed 32 MB";
		case AEGISUB_FONTCOLLECTOR_EVENT_COLLECTION_NEWLINE:
			return {};
	}

	return {};
}

void PrintEvent(AegisubFontCollectorEvent const *event, void*) {
	if (!event)
		return;

	auto text = FormatEvent(*event);
	if (text.empty())
		std::cout << "\n";
	else
		std::cout << text << "\n";
}

void CollectJsonEvent(AegisubFontCollectorEvent const *event, void *user_data) {
	if (!event || !user_data)
		return;

	auto& context = *static_cast<JsonContext *>(user_data);
	auto& item = context.events.emplace_back();
	item.type = event->type;
	item.text = FormatEvent(*event);
	item.face = Safe(event->face);
	item.message = Safe(event->message);
	item.style = Safe(event->style);
	item.path = Safe(event->path);
	item.styles.reserve(event->style_count);
	for (size_t i = 0; i < event->style_count; ++i)
		item.styles.emplace_back(event->styles[i]);
	if (event->line_count)
		item.lines.assign(event->lines, event->lines + event->line_count);
	item.count = event->count;
	item.requested_weight = event->requested_weight;
	item.requested_italic = event->requested_italic;
}

std::string JoinPaths(AegisubFontCollectorMatchedFont const& font) {
	std::ostringstream out;
	for (size_t i = 0; i < font.path_count; ++i) {
		if (i)
			out << ", ";
		out << font.paths[i];
	}
	return out.str();
}

std::string JoinCodepoints(AegisubFontCollectorFontUsage const& usage) {
	std::ostringstream out;
	for (size_t i = 0; i < usage.codepoint_count; ++i) {
		if (i)
			out << ' ';
		out << "U+" << std::uppercase << std::hex << usage.codepoints[i] << std::dec;
	}
	return out.str();
}

void PrintUsage(AegisubFontCollectorFontUsage const *usage, void*) {
	if (!usage)
		return;

	std::cout << "Font usage: " << Safe(usage->ass_facename)
		<< " weight=" << usage->ass_bold
		<< " italic=" << usage->ass_italic << "\n";
	if (usage->style_count) {
		AegisubFontCollectorEvent event = {};
		event.styles = usage->styles;
		event.style_count = usage->style_count;
		std::cout << "  styles: " << JoinStyles(event) << "\n";
	}
	if (usage->override_line_count) {
		AegisubFontCollectorEvent event = {};
		event.lines = usage->override_lines;
		event.line_count = usage->override_line_count;
		std::cout << "  override lines: " << JoinLines(event) << "\n";
	}
	std::cout << "  codepoints: " << JoinCodepoints(*usage) << "\n";
	auto facename_full = Safe(usage->matched_facename_full);
	std::cout << "  matched: " << Safe(usage->matched.facename)
		<< (!facename_full.empty() && facename_full != Safe(usage->matched.facename) ? " full=" + facename_full : std::string())
		<< " face_index=" << usage->matched.face_index
		<< " weight=" << usage->matched.weight
		<< " italic=" << usage->matched.italic
		<< " fake_bold=" << usage->matched.fake_bold
		<< " fake_italic=" << usage->matched.fake_italic << "\n";
	if (usage->matched.path_count)
		std::cout << "  paths: " << JoinPaths(usage->matched) << "\n";
	if (usage->matched.missing_text && *usage->matched.missing_text)
		std::cout << "  missing codepoints: " << usage->matched.missing_text << "\n";
}

void CollectJsonUsage(AegisubFontCollectorFontUsage const *usage, void *user_data) {
	if (!usage || !user_data)
		return;

	auto& context = *static_cast<JsonContext *>(user_data);
	auto& item = context.usages.emplace_back();
	item.ass_facename = Safe(usage->ass_facename);
	item.ass_bold = usage->ass_bold;
	item.ass_italic = usage->ass_italic != 0;
	if (usage->codepoint_count)
		item.codepoints.assign(usage->codepoints, usage->codepoints + usage->codepoint_count);
	item.codepoint_names.reserve(item.codepoints.size());
	for (auto codepoint : item.codepoints)
		item.codepoint_names.push_back(FormatCodepoint(codepoint));
	item.styles.reserve(usage->style_count);
	for (size_t i = 0; i < usage->style_count; ++i)
		item.styles.emplace_back(usage->styles[i]);
	if (usage->override_line_count)
		item.override_lines.assign(usage->override_lines, usage->override_lines + usage->override_line_count);
	item.matched.facename = Safe(usage->matched.facename);
	item.matched.facename_full = Safe(usage->matched_facename_full);
	item.matched.face_index = usage->matched.face_index;
	item.matched.weight = usage->matched.weight;
	item.matched.bold = usage->matched.bold != 0;
	item.matched.italic = usage->matched.italic != 0;
	item.matched.is_collection = usage->matched.is_collection != 0;
	item.matched.path_source = Safe(usage->matched.path_source);
	item.matched.paths.reserve(usage->matched.path_count);
	for (size_t i = 0; i < usage->matched.path_count; ++i)
		item.matched.paths.emplace_back(usage->matched.paths[i]);
	item.matched.fake_bold = usage->matched.fake_bold != 0;
	item.matched.fake_italic = usage->matched.fake_italic != 0;
	item.matched.libass_fake_bold = usage->matched.libass_fake_bold != 0;
	item.matched.libass_fake_italic = usage->matched.libass_fake_italic != 0;
	item.matched.libass_score = usage->matched.libass_score;
	item.matched.missing_text = Safe(usage->matched.missing_text);
	if (usage->matched.missing_codepoint_count)
		item.matched.missing_codepoints.assign(usage->matched.missing_codepoints, usage->matched.missing_codepoints + usage->matched.missing_codepoint_count);
	item.matched.missing_codepoint_names.reserve(item.matched.missing_codepoints.size());
	for (auto codepoint : item.matched.missing_codepoints)
		item.matched.missing_codepoint_names.push_back(FormatCodepoint(codepoint));
	item.matched.requested_weight = usage->matched.requested_weight;
}

void WriteJsonStringArray(std::ostream& out, std::vector<std::string> const& values) {
	out << '[';
	for (size_t i = 0; i < values.size(); ++i) {
		if (i)
			out << ", ";
		WriteJsonString(out, values[i]);
	}
	out << ']';
}

void WriteJsonIntArray(std::ostream& out, std::vector<int> const& values) {
	out << '[';
	for (size_t i = 0; i < values.size(); ++i) {
		if (i)
			out << ", ";
		out << values[i];
	}
	out << ']';
}

void WriteJsonUInt32Array(std::ostream& out, std::vector<uint32_t> const& values) {
	out << '[';
	for (size_t i = 0; i < values.size(); ++i) {
		if (i)
			out << ", ";
		out << values[i];
	}
	out << ']';
}

void WriteJsonBool(std::ostream& out, bool value) {
	out << (value ? "true" : "false");
}

void WriteJsonReport(std::ostream& out, int result, std::string const& error, JsonContext const& context) {
	out << "{\n";
	out << "  \"ok\": ";
	WriteJsonBool(out, result == AEGISUB_FONTCOLLECTOR_OK);
	out << ",\n  \"result\": " << result << ",\n  \"error\": ";
	WriteJsonString(out, error);
	out << ",\n  \"backend\": ";
	WriteJsonString(out, context.requested_backend);
	out << ",\n  \"requested_backend\": ";
	WriteJsonString(out, context.requested_backend);
	out << ",\n  \"resolved_backend\": ";
	WriteJsonString(out, result == AEGISUB_FONTCOLLECTOR_OK ? context.resolved_backend : std::string());
	out << ",\n  \"events\": [\n";
	for (size_t i = 0; i < context.events.size(); ++i) {
		auto const& event = context.events[i];
		out << "    {\n";
		out << "      \"type\": " << static_cast<int>(event.type) << ",\n";
		out << "      \"type_name\": ";
		WriteJsonString(out, EventTypeName(event.type));
		out << ",\n      \"text\": ";
		WriteJsonString(out, event.text);
		out << ",\n      \"face\": ";
		WriteJsonString(out, event.face);
		out << ",\n      \"message\": ";
		WriteJsonString(out, event.message);
		out << ",\n      \"style\": ";
		WriteJsonString(out, event.style);
		out << ",\n      \"path\": ";
		WriteJsonString(out, event.path);
		out << ",\n      \"styles\": ";
		WriteJsonStringArray(out, event.styles);
		out << ",\n      \"lines\": ";
		WriteJsonIntArray(out, event.lines);
		out << ",\n      \"count\": " << event.count;
		out << ",\n      \"requested_weight\": " << event.requested_weight;
		out << ",\n      \"requested_italic\": " << event.requested_italic << "\n";
		out << "    }" << (i + 1 == context.events.size() ? "\n" : ",\n");
	}
	out << "  ],\n  \"font_usage\": [\n";
	for (size_t i = 0; i < context.usages.size(); ++i) {
		auto const& usage = context.usages[i];
		out << "    {\n";
		out << "      \"ass_font\": {\n";
		out << "        \"facename\": ";
		WriteJsonString(out, usage.ass_facename);
		out << ",\n        \"bold\": " << usage.ass_bold << ",\n        \"italic\": ";
		WriteJsonBool(out, usage.ass_italic);
		out << "\n      },\n      \"codepoints\": {\n";
		out << "        \"values\": ";
		WriteJsonUInt32Array(out, usage.codepoints);
		out << ",\n        \"names\": ";
		WriteJsonStringArray(out, usage.codepoint_names);
		out << "\n      },\n      \"styles\": ";
		WriteJsonStringArray(out, usage.styles);
		out << ",\n      \"override_lines\": ";
		WriteJsonIntArray(out, usage.override_lines);
		out << ",\n      \"matched_font\": {\n";
		out << "        \"facename\": ";
		WriteJsonString(out, usage.matched.facename);
		out << ",\n        \"facename_full\": ";
		WriteJsonString(out, usage.matched.facename_full);
		out << ",\n        \"face_index\": " << usage.matched.face_index;
		out << ",\n        \"weight\": " << usage.matched.weight;
		out << ",\n        \"requested_weight\": " << usage.matched.requested_weight;
		out << ",\n        \"bold\": ";
		WriteJsonBool(out, usage.matched.bold);
		out << ",\n        \"italic\": ";
		WriteJsonBool(out, usage.matched.italic);
		out << ",\n        \"is_collection\": ";
		WriteJsonBool(out, usage.matched.is_collection);
		out << ",\n        \"path_source\": ";
		WriteJsonString(out, usage.matched.path_source);
		out << ",\n        \"paths\": ";
		WriteJsonStringArray(out, usage.matched.paths);
		out << ",\n        \"fake_bold\": ";
		WriteJsonBool(out, usage.matched.fake_bold);
		out << ",\n        \"fake_italic\": ";
		WriteJsonBool(out, usage.matched.fake_italic);
		out << ",\n        \"libass_fake_bold\": ";
		WriteJsonBool(out, usage.matched.libass_fake_bold);
		out << ",\n        \"libass_fake_italic\": ";
		WriteJsonBool(out, usage.matched.libass_fake_italic);
		out << ",\n        \"libass_score\": " << usage.matched.libass_score;
		out << ",\n        \"missing_text\": ";
		WriteJsonString(out, usage.matched.missing_text);
		out << ",\n        \"missing_codepoints\": {\n";
		out << "          \"values\": ";
		WriteJsonUInt32Array(out, usage.matched.missing_codepoints);
		out << ",\n          \"names\": ";
		WriteJsonStringArray(out, usage.matched.missing_codepoint_names);
		out << "\n        }";
		out << "\n      }\n";
		out << "    }" << (i + 1 == context.usages.size() ? "\n" : ",\n");
	}
	out << "  ]\n}\n";
}
}

int main(int argc, char **argv) {
#ifdef _WIN32
	auto utf8_args = GetUtf8CommandLineArgs();
	std::vector<char *> utf8_argv;
	if (!utf8_args.empty()) {
		utf8_argv = MakeArgv(utf8_args);
		argc = static_cast<int>(utf8_argv.size());
		argv = utf8_argv.data();
	}
#endif

	CLI::App app{"Collect or check font files used by an ASS subtitle script"};

	std::string input;
	std::string encoding;
	std::string backend = "auto";
	bool check = false;
	bool copy_to_script = false;
	bool details = false;
	bool json = false;
	std::string copy_dir;
#ifndef _WIN32
	std::string symlink_dir;
#endif

	app.add_option("input", input, "ASS/SSA subtitle file")->required();
	app.add_option("--encoding", encoding, "Input subtitle encoding; omitted enables BOM/UTF-8 detection");
	app.add_option("--backend", backend, "Font backend: auto, platform, fontconfig, or coretext")
		->check(CLI::IsMember({"auto", "platform", "fontconfig", "coretext"}));
	app.add_flag("--details", details, "Print ASS font usage and matched font details");
	app.add_flag("--json", json, "Print structured JSON output; implies --details");

	auto *mode = app.add_option_group("Mode");
	mode->add_flag("--check", check, "Only check required fonts");
	mode->add_option("--copy", copy_dir, "Copy fonts to directory")->option_text("DIR");
	mode->add_flag("--copy-to-script-dir", copy_to_script, "Copy fonts next to the subtitle file");
#ifndef _WIN32
	mode->add_option("--symlink", symlink_dir, "Symlink fonts to directory")->option_text("DIR");
#endif
	mode->require_option(0, 1);

	CLI11_PARSE(app, argc, argv);

	AegisubFontCollectorRequest request = {};
	request.input_path = input.c_str();
	request.encoding = encoding.c_str();
	if (backend == "platform")
		request.backend = AEGISUB_FONTCOLLECTOR_BACKEND_PLATFORM_DEFAULT;
	else if (backend == "fontconfig")
		request.backend = AEGISUB_FONTCOLLECTOR_BACKEND_FONTCONFIG;
	else if (backend == "coretext")
		request.backend = AEGISUB_FONTCOLLECTOR_BACKEND_CORETEXT;
	else
		request.backend = AEGISUB_FONTCOLLECTOR_BACKEND_AUTO;

	if (!copy_dir.empty()) {
		request.mode = AEGISUB_FONTCOLLECTOR_MODE_COPY_TO_FOLDER;
		request.destination_path = copy_dir.c_str();
	}
	else if (copy_to_script) {
		request.mode = AEGISUB_FONTCOLLECTOR_MODE_COPY_TO_SCRIPT_FOLDER;
	}
#ifndef _WIN32
	else if (!symlink_dir.empty()) {
		request.mode = AEGISUB_FONTCOLLECTOR_MODE_SYMLINK_TO_FOLDER;
		request.destination_path = symlink_dir.c_str();
	}
#endif
	else {
		(void)check;
		request.mode = AEGISUB_FONTCOLLECTOR_MODE_CHECK;
	}

	std::array<char, 4096> error = {};
	AegisubFontCollectorSummary summary = {};
	JsonContext json_context;
	json_context.requested_backend = RequestedBackendName(request.backend);
	json_context.resolved_backend = ResolvedBackendName(request.backend);
	int result = aegisub_fontcollector_collect(
		&request,
		json ? &CollectJsonEvent : &PrintEvent,
		json ? static_cast<void *>(&json_context) : nullptr,
		(json || details) ? (json ? &CollectJsonUsage : &PrintUsage) : nullptr,
		json ? static_cast<void *>(&json_context) : nullptr,
		&summary,
		error.data(),
		error.size());

	if (json)
		WriteJsonReport(std::cout, result, error.data(), json_context);

	if (result != AEGISUB_FONTCOLLECTOR_OK) {
		if (!json) {
			std::cerr << "fontcollector failed";
			if (error[0])
				std::cerr << ": " << error.data();
			std::cerr << "\n";
		}
	}

	return result == AEGISUB_FONTCOLLECTOR_OK ? 0 : result;
}
