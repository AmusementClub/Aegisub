// Copyright (c) 2026, MIRIMIRIM

#include <aegisub/fontcollector/fontcollector.h>

#include "ass_file.h"
#include "ass_io_core.h"
#include "font_collector_core.h"
#include "font_file_lister.h"

#include <libaegisub/charset.h>
#include <libaegisub/exception.h>
#include <libaegisub/fs.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <string>
#include <vector>

namespace {
static_assert(static_cast<int>(FontCollectionMode::CheckFontsOnly) == AEGISUB_FONTCOLLECTOR_MODE_CHECK);
static_assert(static_cast<int>(FontCollectionMode::CopyToFolder) == AEGISUB_FONTCOLLECTOR_MODE_COPY_TO_FOLDER);
static_assert(static_cast<int>(FontCollectionMode::CopyToScriptFolder) == AEGISUB_FONTCOLLECTOR_MODE_COPY_TO_SCRIPT_FOLDER);
static_assert(static_cast<int>(FontCollectionMode::CopyToZip) == AEGISUB_FONTCOLLECTOR_MODE_COPY_TO_ZIP);
static_assert(static_cast<int>(FontCollectionMode::SymlinkToFolder) == AEGISUB_FONTCOLLECTOR_MODE_SYMLINK_TO_FOLDER);
static_assert(static_cast<int>(FontCollectorEventType::FontBackendInfo) == AEGISUB_FONTCOLLECTOR_EVENT_FONT_BACKEND_INFO);
static_assert(static_cast<int>(FontCollectorEventType::CollectionNewline) == AEGISUB_FONTCOLLECTOR_EVENT_COLLECTION_NEWLINE);

void WriteError(char *buffer, size_t buffer_size, std::string const& message) {
	if (!buffer || buffer_size == 0)
		return;

	std::snprintf(buffer, buffer_size, "%s", message.c_str());
}

bool ToCoreMode(AegisubFontCollectorMode mode, FontCollectionMode& out) {
	switch (mode) {
		case AEGISUB_FONTCOLLECTOR_MODE_CHECK:
			out = FontCollectionMode::CheckFontsOnly;
			return true;
		case AEGISUB_FONTCOLLECTOR_MODE_COPY_TO_FOLDER:
			out = FontCollectionMode::CopyToFolder;
			return true;
		case AEGISUB_FONTCOLLECTOR_MODE_COPY_TO_SCRIPT_FOLDER:
			out = FontCollectionMode::CopyToScriptFolder;
			return true;
		case AEGISUB_FONTCOLLECTOR_MODE_COPY_TO_ZIP:
			out = FontCollectionMode::CopyToZip;
			return true;
		case AEGISUB_FONTCOLLECTOR_MODE_SYMLINK_TO_FOLDER:
			out = FontCollectionMode::SymlinkToFolder;
			return true;
	}
	return false;
}

int DestinationError(FontCollectionDestinationResult result, char *error_buffer, size_t error_buffer_size) {
	if (result.invalid_destination) {
		WriteError(error_buffer, error_buffer_size, "destination path is an existing file");
		return AEGISUB_FONTCOLLECTOR_INVALID_DESTINATION;
	}

	switch (result.error) {
		case FontCollectionDestinationError::None:
			return AEGISUB_FONTCOLLECTOR_OK;
		case FontCollectionDestinationError::CouldNotCreateDestinationFolder:
			WriteError(error_buffer, error_buffer_size, "could not create destination folder");
			return AEGISUB_FONTCOLLECTOR_INVALID_DESTINATION;
		case FontCollectionDestinationError::InvalidArchivePath:
			WriteError(error_buffer, error_buffer_size, "zip archive destination is not valid");
			return AEGISUB_FONTCOLLECTOR_INVALID_DESTINATION;
	}

	WriteError(error_buffer, error_buffer_size, "unknown destination error");
	return AEGISUB_FONTCOLLECTOR_INVALID_DESTINATION;
}

void EmitCEvent(FontCollectorEvent const& event, AegisubFontCollectorEventCallback callback, void *user_data) {
	if (!callback)
		return;

	std::string path_text;
	if (!event.path.empty())
		path_text = agi::fs::PathToString(event.path);

	std::vector<char const*> styles;
	styles.reserve(event.styles.size());
	for (auto const& style : event.styles)
		styles.push_back(style.c_str());

	AegisubFontCollectorEvent c_event = {};
	c_event.type = static_cast<AegisubFontCollectorEventType>(event.type);
	c_event.face = event.face.c_str();
	c_event.message = event.message.c_str();
	c_event.style = event.style.c_str();
	c_event.path = path_text.c_str();
	c_event.styles = styles.empty() ? nullptr : styles.data();
	c_event.style_count = styles.size();
	c_event.lines = event.lines.empty() ? nullptr : event.lines.data();
	c_event.line_count = event.lines.size();
	c_event.count = event.count;
	c_event.requested_weight = event.requested_weight;
	c_event.requested_italic = event.requested_italic;

	callback(&c_event, user_data);
}

void EmitCUsage(FontCollectorAssFontUsage const& usage,
                AegisubFontCollectorFontUsageCallback callback,
                void *user_data) {
	if (!callback)
		return;

	std::vector<std::string> path_texts;
	path_texts.reserve(usage.matched.paths.size());
	std::vector<char const*> paths;
	paths.reserve(usage.matched.paths.size());
	for (auto const& path : usage.matched.paths) {
		path_texts.push_back(agi::fs::PathToString(path));
		paths.push_back(path_texts.back().c_str());
	}

	std::vector<char const*> styles;
	styles.reserve(usage.styles.size());
	for (auto const& style : usage.styles)
		styles.push_back(style.c_str());

	AegisubFontCollectorFontUsage c_usage = {};
	c_usage.ass_facename = usage.ass_facename.c_str();
	c_usage.ass_bold = usage.ass_bold;
	c_usage.ass_italic = usage.ass_italic;
	c_usage.chars = usage.chars.empty() ? nullptr : usage.chars.data();
	c_usage.char_count = usage.chars.size();
	c_usage.styles = styles.empty() ? nullptr : styles.data();
	c_usage.style_count = styles.size();
	c_usage.override_lines = usage.override_lines.empty() ? nullptr : usage.override_lines.data();
	c_usage.override_line_count = usage.override_lines.size();
	c_usage.matched.facename = usage.matched.facename.c_str();
	c_usage.matched.face_index = usage.matched.face_index;
	c_usage.matched.weight = usage.matched.weight;
	c_usage.matched.bold = usage.matched.bold;
	c_usage.matched.italic = usage.matched.italic;
	c_usage.matched.is_collection = usage.matched.is_collection;
	c_usage.matched.path_source = usage.matched.path_source.c_str();
	c_usage.matched.paths = paths.empty() ? nullptr : paths.data();
	c_usage.matched.path_count = paths.size();
	c_usage.matched.fake_bold = usage.matched.fake_bold;
	c_usage.matched.fake_italic = usage.matched.fake_italic;
	c_usage.matched.libass_fake_bold = usage.matched.libass_fake_bold;
	c_usage.matched.libass_fake_italic = usage.matched.libass_fake_italic;
	c_usage.matched.libass_score = usage.matched.libass_score;
	c_usage.matched.missing_chars = usage.matched.missing_chars.c_str();
	c_usage.matched.requested_weight = usage.matched.requested_weight;

	callback(&c_usage, user_data);
}
}

extern "C" int aegisub_fontcollector_collect(
	AegisubFontCollectorRequest const *request,
	AegisubFontCollectorEventCallback callback,
	void *user_data,
	AegisubFontCollectorFontUsageCallback usage_callback,
	void *usage_user_data,
	char *error_buffer,
	size_t error_buffer_size) {
	WriteError(error_buffer, error_buffer_size, "");

	if (!request || !request->input_path || !*request->input_path) {
		WriteError(error_buffer, error_buffer_size, "input_path is required");
		return AEGISUB_FONTCOLLECTOR_INVALID_ARGUMENT;
	}

	FontCollectionMode mode;
	if (!ToCoreMode(request->mode, mode)) {
		WriteError(error_buffer, error_buffer_size, "invalid collection mode");
		return AEGISUB_FONTCOLLECTOR_INVALID_MODE;
	}

	if (mode == FontCollectionMode::CopyToZip) {
		WriteError(error_buffer, error_buffer_size, "zip collection is not implemented by this library yet");
		return AEGISUB_FONTCOLLECTOR_UNSUPPORTED_MODE;
	}

#ifdef _WIN32
	if (mode == FontCollectionMode::SymlinkToFolder) {
		WriteError(error_buffer, error_buffer_size, "symlink collection is not supported on Windows");
		return AEGISUB_FONTCOLLECTOR_UNSUPPORTED_MODE;
	}
#endif

	try {
		auto input_path = agi::fs::PathFromString(request->input_path);
		auto destination = request->destination_path && *request->destination_path
			? agi::fs::PathFromString(request->destination_path)
			: agi::fs::path();

		if (mode == FontCollectionMode::CopyToScriptFolder) {
			destination = input_path.parent_path();
			if (destination.empty())
				destination = std::filesystem::current_path();
		}

		if ((mode == FontCollectionMode::CopyToFolder || mode == FontCollectionMode::SymlinkToFolder) && destination.empty()) {
			WriteError(error_buffer, error_buffer_size, "destination_path is required for this collection mode");
			return AEGISUB_FONTCOLLECTOR_INVALID_ARGUMENT;
		}

		auto destination_result = PrepareFontCollectionDestination(mode, destination);
		if (auto error = DestinationError(destination_result, error_buffer, error_buffer_size))
			return error;

		auto encoding = request->encoding && *request->encoding
			? std::string(request->encoding)
			: agi::charset::Detect(input_path);
		auto subs = ReadAssFileForCore(input_path, encoding);
		FontCollectorDetails details;

		CollectFonts(
			&subs,
			destination,
			mode,
			[&](FontCollectorEvent const& event) {
				EmitCEvent(event, callback, user_data);
			},
			usage_callback ? &details : nullptr,
			{},
			/* enable_libass_compat = */ true);

		for (auto const& usage : details.fonts)
			EmitCUsage(usage, usage_callback, usage_user_data);

		return AEGISUB_FONTCOLLECTOR_OK;
	}
	catch (agi::Exception const& e) {
		WriteError(error_buffer, error_buffer_size, e.GetMessage());
		return AEGISUB_FONTCOLLECTOR_COLLECT_FAILED;
	}
	catch (std::exception const& e) {
		WriteError(error_buffer, error_buffer_size, e.what());
		return AEGISUB_FONTCOLLECTOR_COLLECT_FAILED;
	}
	catch (...) {
		WriteError(error_buffer, error_buffer_size, "unknown error");
		return AEGISUB_FONTCOLLECTOR_COLLECT_FAILED;
	}
}
