// Copyright (c) 2026, MIRIMIRIM

#include <aegisub/fontcollector/fontcollector.h>

#include "ass_file.h"
#include "ass_io_core.h"
#include "ass_dialogue.h"
#include "font_collector_core.h"
#include "font_file_lister.h"
#include "text_file_reader.h"

#include <libaegisub/charset.h>
#include <libaegisub/exception.h>
#include <libaegisub/fs.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {
static_assert(static_cast<int>(FontCollectionMode::CheckFontsOnly) == AEGISUB_FONTCOLLECTOR_MODE_CHECK);
static_assert(static_cast<int>(FontCollectionMode::CopyToFolder) == AEGISUB_FONTCOLLECTOR_MODE_COPY_TO_FOLDER);
static_assert(static_cast<int>(FontCollectionMode::CopyToScriptFolder) == AEGISUB_FONTCOLLECTOR_MODE_COPY_TO_SCRIPT_FOLDER);
static_assert(static_cast<int>(FontCollectionMode::CopyToZip) == AEGISUB_FONTCOLLECTOR_MODE_COPY_TO_ZIP);
static_assert(static_cast<int>(FontCollectionMode::SymlinkToFolder) == AEGISUB_FONTCOLLECTOR_MODE_SYMLINK_TO_FOLDER);
static_assert(static_cast<int>(FontCollectorBackend::Auto) == AEGISUB_FONTCOLLECTOR_BACKEND_AUTO);
static_assert(static_cast<int>(FontCollectorBackend::PlatformDefault) == AEGISUB_FONTCOLLECTOR_BACKEND_PLATFORM_DEFAULT);
static_assert(static_cast<int>(FontCollectorBackend::Fontconfig) == AEGISUB_FONTCOLLECTOR_BACKEND_FONTCONFIG);
static_assert(static_cast<int>(FontCollectorBackend::CoreText) == AEGISUB_FONTCOLLECTOR_BACKEND_CORETEXT);
static_assert(static_cast<int>(FontCollectorEventType::FontBackendInfo) == AEGISUB_FONTCOLLECTOR_EVENT_FONT_BACKEND_INFO);
static_assert(static_cast<int>(FontCollectorEventType::CollectionNewline) == AEGISUB_FONTCOLLECTOR_EVENT_COLLECTION_NEWLINE);

void WriteError(char *buffer, size_t buffer_size, std::string const& message) {
	if (!buffer || buffer_size == 0)
		return;

	std::snprintf(buffer, buffer_size, "%s", message.c_str());
}

bool StartsWith(std::string_view value, std::string_view prefix) {
	return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

bool IsEventLine(std::string_view line) {
	return StartsWith(line, "Dialogue:") || StartsWith(line, "Comment:");
}

bool IsSectionHeader(std::string_view line) {
	return line.size() >= 2 && line.front() == '[' && line.back() == ']';
}

std::string LowerAscii(std::string_view value) {
	std::string result(value);
	std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
		return static_cast<char>(std::tolower(ch));
	});
	return result;
}

void AssignSourceLineNumbers(AssFile& subs, agi::fs::path const& input_path, std::string const& encoding) {
	TextFileReader reader(input_path, encoding);
	auto event = subs.Events.begin();
	bool in_events = false;
	for (int line_number = 1; reader.HasMoreLines() && event != subs.Events.end(); ++line_number) {
		auto line = reader.ReadLineFromFile();
		if (IsSectionHeader(line)) {
			in_events = LowerAscii(line) == "[events]";
			continue;
		}

		if (!in_events || !IsEventLine(line))
			continue;

		event->Row = line_number - 1;
		++event;
	}
}

void ResetSummary(AegisubFontCollectorSummary *summary) {
	if (summary)
		*summary = {};
}

void AccumulateSummary(AegisubFontCollectorSummary& summary, FontCollectorEvent const& event) {
	switch (event.type) {
		case FontCollectorEventType::StyleMissing:
			++summary.missing_style_count;
			break;
		case FontCollectorEventType::FontFound:
			++summary.found_font_count;
			break;
		case FontCollectorEventType::FontMissing:
			++summary.missing_font_count;
			break;
		case FontCollectorEventType::MissingGlyphs:
			++summary.missing_glyph_font_count;
			break;
		case FontCollectorEventType::FakeBold:
			++summary.fake_bold_count;
			break;
		case FontCollectorEventType::FakeItalic:
			++summary.fake_italic_count;
			break;
		case FontCollectorEventType::CollectionCopied:
		case FontCollectorEventType::CollectionSymlinked:
		case FontCollectorEventType::CollectionAlreadyExists:
			++summary.copied_font_count;
			break;
		case FontCollectorEventType::CollectionFailedCreateDirectory:
		case FontCollectorEventType::CollectionFailedOpen:
		case FontCollectorEventType::CollectionFailedCopy:
			++summary.collection_failure_count;
			break;
		default:
			break;
	}
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

bool ToCoreBackend(AegisubFontCollectorBackend backend, FontCollectorBackend& out) {
	switch (backend) {
		case AEGISUB_FONTCOLLECTOR_BACKEND_AUTO:
			out = FontCollectorBackend::Auto;
			return true;
		case AEGISUB_FONTCOLLECTOR_BACKEND_PLATFORM_DEFAULT:
			out = FontCollectorBackend::PlatformDefault;
			return true;
		case AEGISUB_FONTCOLLECTOR_BACKEND_FONTCONFIG:
			out = FontCollectorBackend::Fontconfig;
			return true;
		case AEGISUB_FONTCOLLECTOR_BACKEND_CORETEXT:
			out = FontCollectorBackend::CoreText;
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

	std::vector<char const*> matched_names;
	matched_names.reserve(usage.matched.names.size());
	for (auto const& name : usage.matched.names)
		matched_names.push_back(name.c_str());

	std::vector<char const*> styles;
	styles.reserve(usage.styles.size());
	for (auto const& style : usage.styles)
		styles.push_back(style.c_str());

	AegisubFontCollectorFontUsage c_usage = {};
	c_usage.ass_facename = usage.ass_facename.c_str();
	c_usage.ass_bold = usage.ass_bold;
	c_usage.ass_italic = usage.ass_italic;
	c_usage.codepoints = usage.codepoints.empty() ? nullptr : usage.codepoints.data();
	c_usage.codepoint_count = usage.codepoints.size();
	c_usage.styles = styles.empty() ? nullptr : styles.data();
	c_usage.style_count = styles.size();
	c_usage.lines = usage.lines.empty() ? nullptr : usage.lines.data();
	c_usage.line_count = usage.lines.size();
	c_usage.override_lines = usage.override_lines.empty() ? nullptr : usage.override_lines.data();
	c_usage.override_line_count = usage.override_lines.size();
	if (usage.matched.paths.empty() && usage.matched.raw_data.bytes.empty())
		c_usage.matched.match_status = AEGISUB_FONTCOLLECTOR_MATCH_MISSING;
	else if (usage.matched.paths.empty())
		c_usage.matched.match_status = AEGISUB_FONTCOLLECTOR_MATCH_MEMORY_ONLY;
	else
		c_usage.matched.match_status = AEGISUB_FONTCOLLECTOR_MATCH_FOUND;
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
	c_usage.matched.missing_text = usage.matched.missing_text.c_str();
	c_usage.matched.missing_codepoints = usage.matched.missing_codepoints.empty() ? nullptr : usage.matched.missing_codepoints.data();
	c_usage.matched.missing_codepoint_count = usage.matched.missing_codepoints.size();
	c_usage.matched.requested_weight = usage.matched.requested_weight;
	c_usage.matched.missing_lines = usage.matched.missing_lines.empty() ? nullptr : usage.matched.missing_lines.data();
	c_usage.matched.missing_line_count = usage.matched.missing_lines.size();
	c_usage.matched_facename_full = usage.matched.facename_full.c_str();
	c_usage.matched_names = matched_names.empty() ? nullptr : matched_names.data();
	c_usage.matched_name_count = matched_names.size();

	callback(&c_usage, user_data);
}

int CollectWithSession(FontCollectorSession& session,
                       AegisubFontCollectorRequest const *request,
                       AegisubFontCollectorEventCallback callback,
                       void *user_data,
                       AegisubFontCollectorFontUsageCallback usage_callback,
                       void *usage_user_data,
                       AegisubFontCollectorSummary *summary,
                       char *error_buffer,
                       size_t error_buffer_size) {
	WriteError(error_buffer, error_buffer_size, "");
	ResetSummary(summary);

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
		AssignSourceLineNumbers(subs, input_path, encoding);
		FontCollectorDetails details;
		AegisubFontCollectorSummary local_summary = {};

		CollectFonts(
			session,
			&subs,
			destination,
			mode,
			[&](FontCollectorEvent const& event) {
				AccumulateSummary(local_summary, event);
				EmitCEvent(event, callback, user_data);
			},
			(usage_callback || summary) ? &details : nullptr,
			{},
			/* enable_libass_compat = */ true);

		local_summary.font_usage_count = details.fonts.size();
		if (summary)
			*summary = local_summary;

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

struct BatchLoadedItem {
	AegisubFontCollectorBatchItem *item = nullptr;
	FontCollectionMode mode = FontCollectionMode::CheckFontsOnly;
	agi::fs::path destination;
	std::unique_ptr<AssFile> subs;
	FontCollectorDetails details;
	AegisubFontCollectorSummary summary = {};
};

void SetBatchItemResult(AegisubFontCollectorBatchItem& item, int result, std::string const& error = {}) {
	item.result = result;
	WriteError(item.error_buffer, item.error_buffer_size, error);
	ResetSummary(item.summary);
}

void FinishBatchItem(BatchLoadedItem& loaded) {
	auto& item = *loaded.item;
	loaded.summary.font_usage_count = loaded.details.fonts.size();
	if (item.summary)
		*item.summary = loaded.summary;

	for (auto const& usage : loaded.details.fonts)
		EmitCUsage(usage, item.usage_callback, item.usage_user_data);

	item.result = AEGISUB_FONTCOLLECTOR_OK;
}

}

struct AegisubFontCollectorSession {
	FontCollectorSession core;
	AegisubFontCollectorBackend backend;

	AegisubFontCollectorSession(FontCollectorBackend core_backend,
	                            AegisubFontCollectorBackend api_backend,
	                            FontCollectorEventSink event_sink)
	: core(core_backend, std::move(event_sink))
	, backend(api_backend)
	{
	}
};

extern "C" int aegisub_fontcollector_session_create(
	AegisubFontCollectorBackend backend,
	AegisubFontCollectorEventCallback callback,
	void *user_data,
	AegisubFontCollectorSession **session,
	char *error_buffer,
	size_t error_buffer_size) {
	WriteError(error_buffer, error_buffer_size, "");

	if (!session) {
		WriteError(error_buffer, error_buffer_size, "session output pointer is required");
		return AEGISUB_FONTCOLLECTOR_INVALID_ARGUMENT;
	}
	*session = nullptr;

	FontCollectorBackend core_backend;
	if (!ToCoreBackend(backend, core_backend)) {
		WriteError(error_buffer, error_buffer_size, "invalid font backend");
		return AEGISUB_FONTCOLLECTOR_INVALID_ARGUMENT;
	}

	try {
		auto event_sink = [callback, user_data](FontCollectorEvent const& event) {
			EmitCEvent(event, callback, user_data);
		};
		auto handle = std::make_unique<AegisubFontCollectorSession>(core_backend, backend, std::move(event_sink));
		*session = handle.release();
		return AEGISUB_FONTCOLLECTOR_OK;
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

extern "C" int aegisub_fontcollector_session_collect(
	AegisubFontCollectorSession *session,
	AegisubFontCollectorRequest const *request,
	AegisubFontCollectorEventCallback callback,
	void *user_data,
	AegisubFontCollectorFontUsageCallback usage_callback,
	void *usage_user_data,
	AegisubFontCollectorSummary *summary,
	char *error_buffer,
	size_t error_buffer_size) {
	if (!session) {
		WriteError(error_buffer, error_buffer_size, "session is required");
		ResetSummary(summary);
		return AEGISUB_FONTCOLLECTOR_INVALID_ARGUMENT;
	}

	return CollectWithSession(
		session->core,
		request,
		callback,
		user_data,
		usage_callback,
		usage_user_data,
		summary,
		error_buffer,
		error_buffer_size);
}

extern "C" int aegisub_fontcollector_session_collect_batch(
	AegisubFontCollectorSession *session,
	AegisubFontCollectorBatchItem *items,
	size_t item_count,
	char *error_buffer,
	size_t error_buffer_size) {
	WriteError(error_buffer, error_buffer_size, "");

	if (!session) {
		WriteError(error_buffer, error_buffer_size, "session is required");
		return AEGISUB_FONTCOLLECTOR_INVALID_ARGUMENT;
	}
	if (!items && item_count) {
		WriteError(error_buffer, error_buffer_size, "batch items are required");
		return AEGISUB_FONTCOLLECTOR_INVALID_ARGUMENT;
	}

	std::vector<BatchLoadedItem> loaded_items;
	loaded_items.reserve(item_count);
	bool have_batch_mode = false;
	FontCollectionMode batch_mode = FontCollectionMode::CheckFontsOnly;

	for (size_t i = 0; i < item_count; ++i) {
		auto& item = items[i];
		item.result = AEGISUB_FONTCOLLECTOR_OK;
		WriteError(item.error_buffer, item.error_buffer_size, "");
		ResetSummary(item.summary);

		auto const& request = item.request;
		if (!request.input_path || !*request.input_path) {
			SetBatchItemResult(item, AEGISUB_FONTCOLLECTOR_INVALID_ARGUMENT, "input_path is required");
			continue;
		}

		FontCollectionMode mode;
		if (!ToCoreMode(request.mode, mode)) {
			SetBatchItemResult(item, AEGISUB_FONTCOLLECTOR_INVALID_MODE, "invalid collection mode");
			continue;
		}

		if (mode == FontCollectionMode::CopyToZip) {
			SetBatchItemResult(item, AEGISUB_FONTCOLLECTOR_UNSUPPORTED_MODE, "zip collection is not implemented by this library yet");
			continue;
		}

#ifdef _WIN32
		if (mode == FontCollectionMode::SymlinkToFolder) {
			SetBatchItemResult(item, AEGISUB_FONTCOLLECTOR_UNSUPPORTED_MODE, "symlink collection is not supported on Windows");
			continue;
		}
#endif

		if (!have_batch_mode) {
			batch_mode = mode;
			have_batch_mode = true;
		}
		else if (mode != batch_mode) {
			SetBatchItemResult(item, AEGISUB_FONTCOLLECTOR_INVALID_MODE, "all batch items must use the same collection mode");
			continue;
		}

		try {
			auto input_path = agi::fs::PathFromString(request.input_path);
			auto destination = request.destination_path && *request.destination_path
				? agi::fs::PathFromString(request.destination_path)
				: agi::fs::path();

			if (mode == FontCollectionMode::CopyToScriptFolder) {
				destination = input_path.parent_path();
				if (destination.empty())
					destination = std::filesystem::current_path();
			}

			if ((mode == FontCollectionMode::CopyToFolder || mode == FontCollectionMode::SymlinkToFolder) && destination.empty()) {
				SetBatchItemResult(item, AEGISUB_FONTCOLLECTOR_INVALID_ARGUMENT, "destination_path is required for this collection mode");
				continue;
			}

			auto destination_result = PrepareFontCollectionDestination(mode, destination);
			if (auto destination_error = DestinationError(destination_result, item.error_buffer, item.error_buffer_size)) {
				ResetSummary(item.summary);
				item.result = destination_error;
				continue;
			}

			auto encoding = request.encoding && *request.encoding
				? std::string(request.encoding)
				: agi::charset::Detect(input_path);

			BatchLoadedItem loaded;
			loaded.item = &item;
			loaded.mode = mode;
			loaded.destination = destination;
			loaded.subs = std::make_unique<AssFile>(ReadAssFileForCore(input_path, encoding));
			AssignSourceLineNumbers(*loaded.subs, input_path, encoding);
			loaded_items.push_back(std::move(loaded));
		}
		catch (agi::Exception const& e) {
			SetBatchItemResult(item, AEGISUB_FONTCOLLECTOR_COLLECT_FAILED, e.GetMessage());
		}
		catch (std::exception const& e) {
			SetBatchItemResult(item, AEGISUB_FONTCOLLECTOR_COLLECT_FAILED, e.what());
		}
		catch (...) {
			SetBatchItemResult(item, AEGISUB_FONTCOLLECTOR_COLLECT_FAILED, "unknown error");
		}
	}

	if (loaded_items.empty())
		return AEGISUB_FONTCOLLECTOR_OK;

	std::vector<FontCollectionBatchSource> sources;
	sources.reserve(loaded_items.size());
	for (auto& loaded : loaded_items) {
		auto *loaded_ptr = &loaded;
		FontCollectionBatchSource source;
		source.subs = loaded_ptr->subs.get();
		source.destination = loaded_ptr->destination;
		source.details = (loaded_ptr->item->usage_callback || loaded_ptr->item->summary) ? &loaded_ptr->details : nullptr;
		source.font_event_sink = [loaded_ptr](FontCollectorEvent const& event) {
			AccumulateSummary(loaded_ptr->summary, event);
			EmitCEvent(event, loaded_ptr->item->event_callback, loaded_ptr->item->event_user_data);
		};
		sources.push_back(std::move(source));
	}

	try {
		CollectFonts(
			session->core,
			sources,
			batch_mode,
			{},
			/* enable_libass_compat = */ true);

		for (auto& loaded : loaded_items)
			FinishBatchItem(loaded);
	}
	catch (agi::Exception const& e) {
		for (auto& loaded : loaded_items)
			SetBatchItemResult(*loaded.item, AEGISUB_FONTCOLLECTOR_COLLECT_FAILED, e.GetMessage());
	}
	catch (std::exception const& e) {
		for (auto& loaded : loaded_items)
			SetBatchItemResult(*loaded.item, AEGISUB_FONTCOLLECTOR_COLLECT_FAILED, e.what());
	}
	catch (...) {
		for (auto& loaded : loaded_items)
			SetBatchItemResult(*loaded.item, AEGISUB_FONTCOLLECTOR_COLLECT_FAILED, "unknown error");
	}

	return AEGISUB_FONTCOLLECTOR_OK;
}

extern "C" void aegisub_fontcollector_session_destroy(AegisubFontCollectorSession *session) {
	delete session;
}

extern "C" int aegisub_fontcollector_collect(
	AegisubFontCollectorRequest const *request,
	AegisubFontCollectorEventCallback callback,
	void *user_data,
	AegisubFontCollectorFontUsageCallback usage_callback,
	void *usage_user_data,
	AegisubFontCollectorSummary *summary,
	char *error_buffer,
	size_t error_buffer_size) {
	WriteError(error_buffer, error_buffer_size, "");
	ResetSummary(summary);

	if (!request || !request->input_path || !*request->input_path) {
		WriteError(error_buffer, error_buffer_size, "input_path is required");
		return AEGISUB_FONTCOLLECTOR_INVALID_ARGUMENT;
	}

	FontCollectionMode mode;
	if (!ToCoreMode(request->mode, mode)) {
		WriteError(error_buffer, error_buffer_size, "invalid collection mode");
		return AEGISUB_FONTCOLLECTOR_INVALID_MODE;
	}

	FontCollectorBackend backend;
	if (!ToCoreBackend(request->backend, backend)) {
		WriteError(error_buffer, error_buffer_size, "invalid font backend");
		return AEGISUB_FONTCOLLECTOR_INVALID_ARGUMENT;
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
		AssignSourceLineNumbers(subs, input_path, encoding);
		FontCollectorDetails details;
		AegisubFontCollectorSummary local_summary = {};

		CollectFonts(
			&subs,
			destination,
			mode,
			[&](FontCollectorEvent const& event) {
				AccumulateSummary(local_summary, event);
				EmitCEvent(event, callback, user_data);
			},
			(usage_callback || summary) ? &details : nullptr,
			{},
			/* enable_libass_compat = */ true,
			backend);

		local_summary.font_usage_count = details.fonts.size();
		if (summary)
			*summary = local_summary;

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
