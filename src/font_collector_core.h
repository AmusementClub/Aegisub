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

#include <libaegisub/fs_fwd.h>

#include <functional>
#include <memory>

class AssFile;
struct FontCollectorDetails;

enum class FontCollectionMode {
	CheckFontsOnly = 0,
	CopyToFolder = 1,
	CopyToScriptFolder = 2,
	CopyToZip = 3,
	SymlinkToFolder = 4
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

class FontCollectionArchiveWriter {
public:
	virtual ~FontCollectionArchiveWriter() = default;

	virtual bool IsOk() const = 0;
	virtual bool AddFile(agi::fs::path const& source, agi::fs::path const& name) = 0;
};

using FontCollectionArchiveFactory =
	std::function<std::unique_ptr<FontCollectionArchiveWriter>(agi::fs::path const& destination)>;

FontCollectionDestinationResult PrepareFontCollectionDestination(FontCollectionMode mode, agi::fs::path const& destination);

void CollectFonts(AssFile const *subs,
                  agi::fs::path const& destination,
                  FontCollectionMode mode,
                  FontCollectorEventSink font_event_sink,
                  FontCollectorDetails *details = nullptr,
                  FontCollectionArchiveFactory archive_factory = {},
                  bool enable_libass_compat = false);
