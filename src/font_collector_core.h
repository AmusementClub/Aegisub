// Copyright (c) 2026, MIRIMIRIM
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

#pragma once

#include "font_collector_events.h"
#include "font_collector_backend.h"

#include <libaegisub/fs_fwd.h>

#include <functional>
#include <memory>
#include <span>
#include <vector>

class AssFile;
class IFontFileLister;
struct FontCollectorDetails;

enum class FontCollectionMode {
	CheckFontsOnly = 0,
	CopyToFolder = 1,
	CopyToScriptFolder = 2,
	CopyToZip = 3
};

enum class FontCollectionDestinationError {
	None,
	CouldNotCreateDestinationFolder,
	InvalidArchivePath
};

struct FontCollectionDestinationResult {
	bool invalid_destination = false;
	FontCollectionDestinationError error = FontCollectionDestinationError::None;
};

struct FontCollectionBatchSource {
	AssFile const *subs = nullptr;
	agi::fs::path destination;
	FontCollectorEventSink font_event_sink;
	FontCollectorDetails *details = nullptr;
};

class FontCollectionArchiveWriter {
public:
	virtual ~FontCollectionArchiveWriter() = default;

	virtual bool IsOk() const = 0;
	virtual bool AddFile(agi::fs::path const& source, agi::fs::path const& name) = 0;
	virtual bool AddMemory(agi::fs::path const& name, std::span<char const> data) = 0;
};

using FontCollectionArchiveFactory =
	std::function<std::unique_ptr<FontCollectionArchiveWriter>(agi::fs::path const& destination)>;

FontCollectionDestinationResult PrepareFontCollectionDestination(FontCollectionMode mode, agi::fs::path const& destination);

class FontCollectorSession {
	FontCollectorMatcher matcher;
	FontCollectorEventSink init_event_sink;
	std::unique_ptr<IFontFileLister> lister;

public:
	FontCollectorSession(FontCollectorEventSink font_event_sink,
	                     FontCollectorMatcher matcher = FontCollectorMatcher::Platform,
	                     FontProviderOptions provider_options = {});
	~FontCollectorSession();

	FontCollectorSession(FontCollectorSession const&) = delete;
	FontCollectorSession& operator=(FontCollectorSession const&) = delete;

	std::vector<agi::fs::path> GetFontPaths(AssFile const *subs,
	                                        FontCollectorEventSink font_event_sink,
	                                        FontCollectorDetails *details = nullptr);
	std::vector<std::vector<agi::fs::path>> GetFontPaths(std::vector<FontCollectionBatchSource> const& sources);

	FontCollectorMatcher GetMatcher() const { return matcher; }
};

void CollectFonts(AssFile const *subs,
                  agi::fs::path const& destination,
                  FontCollectionMode mode,
                  FontCollectorEventSink font_event_sink,
                  FontCollectorDetails *details = nullptr,
                  FontCollectionArchiveFactory archive_factory = {},
                  FontCollectorMatcher matcher = FontCollectorMatcher::Platform);

void CollectFonts(FontCollectorSession& session,
                  AssFile const *subs,
                  agi::fs::path const& destination,
                  FontCollectionMode mode,
                  FontCollectorEventSink font_event_sink,
                  FontCollectorDetails *details = nullptr,
                  FontCollectionArchiveFactory archive_factory = {});

void CollectFonts(FontCollectorSession& session,
                  std::vector<FontCollectionBatchSource> const& sources,
                  FontCollectionMode mode,
                  FontCollectionArchiveFactory archive_factory = {});
