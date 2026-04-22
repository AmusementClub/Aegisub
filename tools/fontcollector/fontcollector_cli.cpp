// Copyright (c) 2026, MIRIMIRIM

#include <aegisub/fontcollector/fontcollector.h>

#include <CLI/CLI.hpp>

#include <array>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
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
		case AEGISUB_FONTCOLLECTOR_EVENT_UPDATING_FONT_CACHE:
			return "Updating font cache";
		case AEGISUB_FONTCOLLECTOR_EVENT_FONT_CACHE_ERROR:
			return "Font cache error: " + Safe(event.message);
		case AEGISUB_FONTCOLLECTOR_EVENT_PARSING_FILE:
			return "Parsing file";
		case AEGISUB_FONTCOLLECTOR_EVENT_STYLE_MISSING:
			return "Style missing: " + Safe(event.style);
		case AEGISUB_FONTCOLLECTOR_EVENT_SEARCHING_FOR_FONT_FILES:
			return "Searching for font files";
		case AEGISUB_FONTCOLLECTOR_EVENT_FONT_MISSING:
			return "Missing font: " + Safe(event.face);
		case AEGISUB_FONTCOLLECTOR_EVENT_FONT_FOUND:
			return "Found font: " + Safe(event.face) + " -> " + Safe(event.path);
		case AEGISUB_FONTCOLLECTOR_EVENT_FAKE_BOLD:
			return "Fake bold required: " + Safe(event.face);
		case AEGISUB_FONTCOLLECTOR_EVENT_FAKE_ITALIC:
			return "Fake italic required: " + Safe(event.face);
		case AEGISUB_FONTCOLLECTOR_EVENT_MISSING_GLYPHS:
			return "Missing glyphs in " + Safe(event.face) + ": " + std::to_string(event.count);
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
	bool check = false;
	bool copy_to_script = false;
	std::string copy_dir;
#ifndef _WIN32
	std::string symlink_dir;
#endif

	app.add_option("input", input, "ASS/SSA subtitle file")->required();
	app.add_option("--encoding", encoding, "Input subtitle encoding; omitted enables BOM/UTF-8 detection");

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
	int result = aegisub_fontcollector_collect(
		&request,
		&PrintEvent,
		nullptr,
		error.data(),
		error.size());

	if (result != AEGISUB_FONTCOLLECTOR_OK) {
		std::cerr << "fontcollector failed";
		if (error[0])
			std::cerr << ": " << error.data();
		std::cerr << "\n";
	}

	return result == AEGISUB_FONTCOLLECTOR_OK ? 0 : result;
}
