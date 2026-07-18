// Copyright (c) 2026, Aegisub Project
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

#include "font_collector_core.h"

#include "font_file_lister.h"
#include "font_matching_libass.h"

#include <libaegisub/exception.h>
#include <libaegisub/fs.h>
#include <libaegisub/io.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {
enum class FileCollectionResult {
	Failed,
	Copied,
	AlreadyExists
};

void Emit(FontCollectorEventSink const& event_sink, FontCollectorEvent event) {
	if (event_sink)
		event_sink(event);
}

void Emit(FontCollectorEventSink const& event_sink, FontCollectorEventType type) {
	FontCollectorEvent event;
	event.type = type;
	Emit(event_sink, std::move(event));
}

std::unique_ptr<IFontFileLister> CreateFontFileLister(FontCollectorMatcher matcher,
                                                       FontCollectorEventSink& event_sink,
                                                       FontProviderOptions const& provider_options) {
	if (matcher == FontCollectorMatcher::Libass) {
		if (!provider_options.include_system_fonts && provider_options.additional_font_files.empty())
			throw std::runtime_error(
				"libass private catalog requires additional font files (include_system_fonts is false)");
#ifdef _WIN32
		return std::make_unique<LibassFontFileLister>(
			event_sink,
			CreateDWriteLibassFontProvider(event_sink, provider_options),
			provider_options.collect_match_candidates);
#elif defined(AEGISUB_FONTCOLLECTOR_ENABLE_FONTCONFIG) || !defined(__APPLE__)
		// Linux: Fontconfig is the platform provider and the libass catalog.
		// macOS: Fontconfig is optional and only used for the libass matcher;
		// platform matching stays on CoreText.
		return std::make_unique<LibassFontFileLister>(
			event_sink,
			std::make_unique<FontConfigFontFileLister>(event_sink, true, provider_options),
			provider_options.collect_match_candidates);
#else
		throw std::runtime_error(
			"libass matcher requires Fontconfig on this platform (not linked in this build)");
#endif
	}

	return std::make_unique<FontFileLister>(event_sink);
}

FileCollectionResult CopyFontToFolder(agi::fs::path const& source,
                                      agi::fs::path const& destination,
                                      FontCollectionMode mode) {
	auto dest = destination / source.filename();

	if (agi::fs::FileExists(dest))
		return FileCollectionResult::AlreadyExists;

	try {
		agi::fs::Copy(source, dest);
		return FileCollectionResult::Copied;
	}
	catch (...) {
		return FileCollectionResult::Failed;
	}
}

FileCollectionResult CopyFontToArchive(FontCollectionArchiveWriter& archive, agi::fs::path const& source) {
	return archive.AddFile(source, source.filename())
		? FileCollectionResult::Copied
		: FileCollectionResult::Failed;
}

std::string_view FontDataExtension(std::span<char const> data) {
	if (data.size() < 4)
		return ".ttf";
	auto const *sig = reinterpret_cast<unsigned char const *>(data.data());
	if (sig[0] == 't' && sig[1] == 't' && sig[2] == 'c' && sig[3] == 'f')
		return ".ttc";
	if (sig[0] == 'O' && sig[1] == 'T' && sig[2] == 'T' && sig[3] == 'O')
		return ".otf";
	if (sig[0] == 'w' && sig[1] == 'O' && sig[2] == 'F' && sig[3] == 'F')
		return ".woff";
	if (sig[0] == 'w' && sig[1] == 'O' && sig[2] == 'F' && sig[3] == '2')
		return ".woff2";
	if (sig[0] == 0x80 && (sig[1] == 0x01 || sig[1] == 0x02))
		return ".pfb";
	if (sig[0] == '%' && sig[1] == '!' && sig[2] == 'P' && sig[3] == 'S')
		return ".pfa";
	if ((sig[0] == 1 || sig[0] == 2) && sig[1] == 0 && sig[2] >= 4)
		return ".cff";
	return ".ttf";
}

std::string SanitizeFontFileStem(std::string_view facename) {
	std::string result;
	result.reserve(facename.size());
	for (auto value : facename) {
		auto ch = static_cast<unsigned char>(value);
		if (ch < 0x20 || value == '<' || value == '>' || value == ':' || value == '"' ||
		    value == '/' || value == '\\' || value == '|' || value == '?' || value == '*')
			result.push_back('_');
		else
			result.push_back(value);
	}
	while (!result.empty() && (result.back() == ' ' || result.back() == '.'))
		result.pop_back();
	return result.empty() ? "memory-font" : result;
}

agi::fs::path MemoryFontFileName(FontMemoryFont const& font, size_t suffix = 1) {
	auto const& bytes = *font.data;
	auto name = SanitizeFontFileStem(font.facename);
	if (suffix > 1) {
		name += '-';
		name += std::to_string(suffix);
	}
	name += FontDataExtension(bytes);
	return agi::fs::PathFromString(name);
}

bool FileMatchesFontData(agi::fs::path const& path, std::span<char const> data) {
	try {
		if (agi::fs::Size(path) != data.size())
			return false;
		auto stream = agi::io::Open(path, true);
		std::array<char, 64 * 1024> buffer;
		size_t offset = 0;
		while (offset < data.size()) {
			auto count = std::min(buffer.size(), data.size() - offset);
			stream->read(buffer.data(), static_cast<std::streamsize>(count));
			if (static_cast<size_t>(stream->gcount()) != count ||
			    !std::equal(buffer.begin(), buffer.begin() + count, data.begin() + offset))
				return false;
			offset += count;
		}
		return true;
	}
	catch (...) {
		return false;
	}
}

bool SameFontData(FontMemoryFont const& left, FontMemoryFont const& right) {
	if (left.data == right.data)
		return true;
	if (!left.data || !right.data || left.data->size() != right.data->size())
		return false;
	return std::equal(left.data->begin(), left.data->end(), right.data->begin());
}

std::vector<FontMemoryFont> CollectMemoryFonts(FontCollectorDetails const *details) {
	std::vector<FontMemoryFont> fonts;
	if (!details)
		return fonts;
	for (auto const& usage : details->fonts) {
		for (auto const& font : usage.matched.memory_fonts) {
			if (!font.data || font.data->empty())
				continue;
			if (std::none_of(fonts.begin(), fonts.end(), [&](FontMemoryFont const& existing) {
				return SameFontData(existing, font);
			}))
				fonts.push_back(font);
		}
	}
	return fonts;
}

struct FontCopyCache {
	std::map<agi::fs::path, bool> existing_targets;
};

FileCollectionResult CopyMemoryFontToFolder(FontMemoryFont const& font,
	                                         agi::fs::path const& destination,
	                                         FontCopyCache *cache,
	                                         agi::fs::path& target) {
	for (size_t suffix = 1; ; ++suffix) {
		target = destination / MemoryFontFileName(font, suffix);
		auto reserved = cache && cache->existing_targets.find(target) != cache->existing_targets.end();
		if (!reserved && !agi::fs::FileExists(target))
			break;
		if (agi::fs::FileExists(target) && FileMatchesFontData(target, *font.data))
			return FileCollectionResult::AlreadyExists;
	}

	try {
		agi::io::Save output(target, true);
		output.Get().write(font.data->data(), static_cast<std::streamsize>(font.data->size()));
		output.Close();
		if (cache)
			cache->existing_targets.emplace(target, true);
		return FileCollectionResult::Copied;
	}
	catch (...) {
		return FileCollectionResult::Failed;
	}
}

FileCollectionResult CopyFontToFolderCached(agi::fs::path const& source,
                                            agi::fs::path const& destination,
                                            FontCollectionMode mode,
                                            FontCopyCache *cache) {
	if (!cache)
		return CopyFontToFolder(source, destination, mode);

	auto target = destination / source.filename();
	if (cache->existing_targets.find(target) != cache->existing_targets.end())
		return FileCollectionResult::AlreadyExists;

	auto result = CopyFontToFolder(source, destination, mode);
	if (result == FileCollectionResult::Copied ||
	    result == FileCollectionResult::AlreadyExists)
		cache->existing_targets.emplace(std::move(target), true);

	return result;
}

void CollectResolvedFonts(std::vector<agi::fs::path> paths,
                          std::vector<FontMemoryFont> memory_fonts,
                          agi::fs::path const& destination,
                          FontCollectionMode mode,
                          FontCollectorEventSink const& font_event_sink,
                          FontCollectionArchiveFactory const& archive_factory,
                          FontCopyCache *copy_cache = nullptr) {
	if (paths.empty() && memory_fonts.empty())
		return;

	switch (mode) {
		case FontCollectionMode::CheckFontsOnly:
			return;
		case FontCollectionMode::CopyToScriptFolder:
		case FontCollectionMode::CopyToFolder:
			Emit(font_event_sink, FontCollectorEventType::CollectionCopyingFontsToFolder);
			break;
		case FontCollectionMode::CopyToZip:
			Emit(font_event_sink, FontCollectorEventType::CollectionCopyingFontsToArchive);
			break;
	}

	std::unique_ptr<FontCollectionArchiveWriter> archive;
	if (mode == FontCollectionMode::CopyToZip) {
		try {
			agi::fs::CreateDirectory(destination.parent_path());
		}
		catch (agi::fs::FileSystemError const& e) {
			FontCollectorEvent event;
			event.type = FontCollectorEventType::CollectionFailedCreateDirectory;
			event.path = destination.parent_path();
			event.message = e.GetMessage();
			Emit(font_event_sink, std::move(event));
			return;
		}

		if (archive_factory)
			archive = archive_factory(destination);

		if (!archive || !archive->IsOk()) {
			FontCollectorEvent event;
			event.type = FontCollectorEventType::CollectionFailedOpen;
			event.path = destination;
			Emit(font_event_sink, std::move(event));
			return;
		}
	}

	bool all_ok = true;
	auto report_result = [&](FileCollectionResult result, agi::fs::path const& path) {
		FontCollectorEvent event;
		event.path = path;
		switch (result) {
			case FileCollectionResult::Copied:
				event.type = FontCollectorEventType::CollectionCopied;
				break;
			case FileCollectionResult::AlreadyExists:
				event.type = FontCollectorEventType::CollectionAlreadyExists;
				break;
			case FileCollectionResult::Failed:
				event.type = FontCollectorEventType::CollectionFailedCopy;
				all_ok = false;
				break;
		}
		Emit(font_event_sink, std::move(event));
	};

	std::set<agi::fs::path> archive_names;
	for (auto path : paths) {
		path.make_preferred();

		auto result = mode == FontCollectionMode::CopyToZip
			? CopyFontToArchive(*archive, path)
			: CopyFontToFolderCached(path, destination, mode, copy_cache);

		report_result(result, path);
		if (mode == FontCollectionMode::CopyToZip)
			archive_names.insert(path.filename());
	}

	for (auto const& font : memory_fonts) {
		agi::fs::path output_path;
		FileCollectionResult result;
		if (mode == FontCollectionMode::CopyToZip) {
			for (size_t suffix = 1; ; ++suffix) {
				output_path = MemoryFontFileName(font, suffix);
				if (archive_names.insert(output_path).second)
					break;
			}
			result = archive->AddMemory(output_path, *font.data)
				? FileCollectionResult::Copied
				: FileCollectionResult::Failed;
		}
		else {
			result = CopyMemoryFontToFolder(font, destination, copy_cache, output_path);
		}
		report_result(result, output_path);
	}

	if (all_ok)
		Emit(font_event_sink, FontCollectorEventType::CollectionDoneAllCopied);
	else
		Emit(font_event_sink, FontCollectorEventType::CollectionDoneSomeNotCopied);

	Emit(font_event_sink, FontCollectorEventType::CollectionNewline);
}
}

FontCollectionDestinationResult PrepareFontCollectionDestination(FontCollectionMode mode, agi::fs::path const& destination) {
	FontCollectionDestinationResult result;

	if (mode == FontCollectionMode::CheckFontsOnly)
		return result;

	if (mode == FontCollectionMode::CopyToZip) {
		if (agi::fs::DirectoryExists(destination) || destination.filename().empty())
			result.error = FontCollectionDestinationError::InvalidArchivePath;
		return result;
	}

	if (agi::fs::FileExists(destination))
		result.invalid_destination = true;

	try {
		agi::fs::CreateDirectory(destination);
	}
	catch (agi::Exception const&) {
		result.error = FontCollectionDestinationError::CouldNotCreateDestinationFolder;
	}

	return result;
}

FontCollectorSession::FontCollectorSession(FontCollectorEventSink font_event_sink,
                                           FontCollectorMatcher matcher,
                                           FontProviderOptions provider_options)
: matcher(matcher)
, init_event_sink(std::move(font_event_sink))
, lister(CreateFontFileLister(matcher, init_event_sink, provider_options))
{
}

FontCollectorSession::~FontCollectorSession() = default;

std::vector<agi::fs::path> FontCollectorSession::GetFontPaths(AssFile const *subs,
                                                              FontCollectorEventSink font_event_sink,
                                                              FontCollectorDetails *details) {
	FontCollector collector(font_event_sink, *lister);
	return collector.GetFontPaths(subs, details);
}

std::vector<std::vector<agi::fs::path>> FontCollectorSession::GetFontPaths(std::vector<FontCollectionBatchSource> const& sources) {
	FontCollector collector(init_event_sink, *lister);

	std::vector<FontCollectorBatchSource> collector_sources;
	collector_sources.reserve(sources.size());
	for (auto const& source : sources) {
		FontCollectorBatchSource collector_source;
		collector_source.file = source.subs;
		collector_source.event_sink = source.font_event_sink;
		collector_source.details = source.details;
		collector_sources.push_back(std::move(collector_source));
	}

	return collector.GetFontPaths(collector_sources);
}

void CollectFonts(AssFile const *subs,
                  agi::fs::path const& destination,
                  FontCollectionMode mode,
                  FontCollectorEventSink font_event_sink,
                  FontCollectorDetails *details,
                  FontCollectionArchiveFactory archive_factory,
                  FontCollectorMatcher matcher) {
	FontCollectorSession session(font_event_sink, matcher);
	CollectFonts(session, subs, destination, mode, std::move(font_event_sink), details, std::move(archive_factory));
}

void CollectFonts(FontCollectorSession& session,
                  AssFile const *subs,
                  agi::fs::path const& destination,
                  FontCollectionMode mode,
                  FontCollectorEventSink font_event_sink,
                  FontCollectorDetails *details,
                  FontCollectionArchiveFactory archive_factory) {
	FontCollectorDetails local_details;
	auto *effective_details = details;
	if (!effective_details && mode != FontCollectionMode::CheckFontsOnly)
		effective_details = &local_details;
	auto paths = session.GetFontPaths(subs, font_event_sink, effective_details);
	CollectResolvedFonts(
		std::move(paths), CollectMemoryFonts(effective_details), destination,
		mode, font_event_sink, archive_factory);
}

void CollectFonts(FontCollectorSession& session,
                  std::vector<FontCollectionBatchSource> const& sources,
                  FontCollectionMode mode,
                  FontCollectionArchiveFactory archive_factory) {
	auto effective_sources = sources;
	std::vector<FontCollectorDetails> local_details(sources.size());
	if (mode != FontCollectionMode::CheckFontsOnly) {
		for (size_t i = 0; i < effective_sources.size(); ++i)
			if (!effective_sources[i].details)
				effective_sources[i].details = &local_details[i];
	}
	auto paths_by_source = session.GetFontPaths(effective_sources);
	FontCopyCache copy_cache;
	auto *copy_cache_ptr = mode == FontCollectionMode::CopyToFolder ||
	                       mode == FontCollectionMode::CopyToScriptFolder
		? &copy_cache
		: nullptr;
	for (size_t i = 0; i < effective_sources.size() && i < paths_by_source.size(); ++i)
		CollectResolvedFonts(
			std::move(paths_by_source[i]),
			CollectMemoryFonts(effective_sources[i].details),
			effective_sources[i].destination,
			mode,
			effective_sources[i].font_event_sink,
			archive_factory,
			copy_cache_ptr);
}
