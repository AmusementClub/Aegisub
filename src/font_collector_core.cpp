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

#include <libaegisub/exception.h>
#include <libaegisub/fs.h>

#include <cstdint>
#include <map>
#include <stdexcept>
#include <utility>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace {
enum class FileCollectionResult {
	Failed,
	Copied,
	AlreadyExists,
	Symlinked
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

std::unique_ptr<IFontFileLister> CreateFontFileLister(FontCollectorBackend backend, FontCollectorEventSink& event_sink) {
	switch (backend) {
		case FontCollectorBackend::Auto:
		case FontCollectorBackend::PlatformDefault:
			return std::make_unique<FontFileLister>(event_sink);
		case FontCollectorBackend::Fontconfig:
#if !defined(_WIN32) && !defined(__APPLE__) || defined(AEGISUB_FONTCOLLECTOR_ENABLE_FONTCONFIG)
			return std::make_unique<FontConfigFontFileLister>(event_sink);
#else
			throw std::runtime_error("fontconfig backend is not enabled in this build");
#endif
		case FontCollectorBackend::CoreText:
#if defined(__APPLE__)
			return std::make_unique<CoreTextFontFileLister>(event_sink);
#else
			throw std::runtime_error("coretext backend is not enabled in this build");
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

#ifndef _WIN32
	if (mode == FontCollectionMode::SymlinkToFolder)
		return symlink(source.c_str(), dest.c_str())
			? FileCollectionResult::Failed
			: FileCollectionResult::Symlinked;
#endif

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

struct FontCopyCache {
	std::map<agi::fs::path, bool> existing_targets;
};

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
	    result == FileCollectionResult::AlreadyExists ||
	    result == FileCollectionResult::Symlinked)
		cache->existing_targets.emplace(std::move(target), true);

	return result;
}

void CollectResolvedFontPaths(std::vector<agi::fs::path> paths,
                              agi::fs::path const& destination,
                              FontCollectionMode mode,
                              FontCollectorEventSink const& font_event_sink,
                              FontCollectionArchiveFactory const& archive_factory,
                              FontCopyCache *copy_cache = nullptr) {
	if (paths.empty())
		return;

	switch (mode) {
		case FontCollectionMode::CheckFontsOnly:
			return;
		case FontCollectionMode::SymlinkToFolder:
			Emit(font_event_sink, FontCollectorEventType::CollectionSymlinkingFontsToFolder);
			break;
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

	uintmax_t total_size = 0;
	bool all_ok = true;
	for (auto path : paths) {
		path.make_preferred();
		total_size += agi::fs::Size(path);

		auto result = mode == FontCollectionMode::CopyToZip
			? CopyFontToArchive(*archive, path)
			: CopyFontToFolderCached(path, destination, mode, copy_cache);

		switch (result) {
			case FileCollectionResult::Copied: {
				FontCollectorEvent event;
				event.type = FontCollectorEventType::CollectionCopied;
				event.path = path;
				Emit(font_event_sink, std::move(event));
				break;
			}
			case FileCollectionResult::AlreadyExists: {
				FontCollectorEvent event;
				event.type = FontCollectorEventType::CollectionAlreadyExists;
				event.path = path;
				Emit(font_event_sink, std::move(event));
				break;
			}
			case FileCollectionResult::Symlinked: {
				FontCollectorEvent event;
				event.type = FontCollectorEventType::CollectionSymlinked;
				event.path = path;
				Emit(font_event_sink, std::move(event));
				break;
			}
			case FileCollectionResult::Failed: {
				FontCollectorEvent event;
				event.type = FontCollectorEventType::CollectionFailedCopy;
				event.path = path;
				Emit(font_event_sink, std::move(event));
				all_ok = false;
				break;
			}
		}
	}

	if (all_ok)
		Emit(font_event_sink, FontCollectorEventType::CollectionDoneAllCopied);
	else
		Emit(font_event_sink, FontCollectorEventType::CollectionDoneSomeNotCopied);

	if (total_size > 32 * 1024 * 1024)
		Emit(font_event_sink, FontCollectorEventType::CollectionOver32MBWarning);

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

FontCollectorSession::FontCollectorSession(FontCollectorBackend backend, FontCollectorEventSink font_event_sink)
: backend(backend)
, init_event_sink(std::move(font_event_sink))
, lister(CreateFontFileLister(backend, init_event_sink))
{
}

FontCollectorSession::~FontCollectorSession() = default;

std::vector<agi::fs::path> FontCollectorSession::GetFontPaths(AssFile const *subs,
                                                              FontCollectorEventSink font_event_sink,
                                                              FontCollectorDetails *details,
                                                              bool enable_libass_compat) {
	FontCollector collector(font_event_sink, *lister);
	if (enable_libass_compat)
		collector.EnableLibassCompat(true);
	return collector.GetFontPaths(subs, details);
}

std::vector<std::vector<agi::fs::path>> FontCollectorSession::GetFontPaths(std::vector<FontCollectionBatchSource> const& sources,
                                                                           bool enable_libass_compat) {
	FontCollector collector(init_event_sink, *lister);
	if (enable_libass_compat)
		collector.EnableLibassCompat(true);

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
                  bool enable_libass_compat,
                  FontCollectorBackend backend) {
	FontCollectorSession session(backend, font_event_sink);
	CollectFonts(session, subs, destination, mode, std::move(font_event_sink), details, std::move(archive_factory), enable_libass_compat);
}

void CollectFonts(FontCollectorSession& session,
                  AssFile const *subs,
                  agi::fs::path const& destination,
                  FontCollectionMode mode,
                  FontCollectorEventSink font_event_sink,
                  FontCollectorDetails *details,
                  FontCollectionArchiveFactory archive_factory,
                  bool enable_libass_compat) {
	auto paths = session.GetFontPaths(subs, font_event_sink, details, enable_libass_compat);
	CollectResolvedFontPaths(std::move(paths), destination, mode, font_event_sink, archive_factory);
}

void CollectFonts(FontCollectorSession& session,
                  std::vector<FontCollectionBatchSource> const& sources,
                  FontCollectionMode mode,
                  FontCollectionArchiveFactory archive_factory,
                  bool enable_libass_compat) {
	auto paths_by_source = session.GetFontPaths(sources, enable_libass_compat);
	FontCopyCache copy_cache;
	auto *copy_cache_ptr = mode == FontCollectionMode::CopyToFolder ||
	                       mode == FontCollectionMode::CopyToScriptFolder ||
	                       mode == FontCollectionMode::SymlinkToFolder
		? &copy_cache
		: nullptr;
	for (size_t i = 0; i < sources.size() && i < paths_by_source.size(); ++i)
		CollectResolvedFontPaths(
			std::move(paths_by_source[i]),
			sources[i].destination,
			mode,
			sources[i].font_event_sink,
			archive_factory,
			copy_cache_ptr);
}
