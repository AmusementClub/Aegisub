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

#pragma once

#include <libaegisub/fs_fwd.h>

#include <functional>
#include <string>
#include <vector>

enum class FontCollectorEventType {
	UpdatingFontCache,
	FontCacheError,
	ParsingFile,
	StyleMissing,
	SearchingForFontFiles,
	FontMissing,
	FontFound,
	FakeBold,
	FakeItalic,
	MissingGlyphs,
	Usage,
	SearchComplete,
	AllFontsFound,
	FontsMissing,
	FontsMissingGlyphs,
	CollectionSymlinkingFontsToFolder,
	CollectionCopyingFontsToFolder,
	CollectionCopyingFontsToArchive,
	CollectionFailedCreateDirectory,
	CollectionFailedOpen,
	CollectionCopied,
	CollectionAlreadyExists,
	CollectionSymlinked,
	CollectionFailedCopy,
	CollectionDoneAllCopied,
	CollectionDoneSomeNotCopied,
	CollectionOver32MBWarning,
	CollectionNewline
};

struct FontCollectorEvent {
	FontCollectorEventType type = FontCollectorEventType::ParsingFile;
	std::string face;
	std::string message;
	std::string style;
	std::vector<std::string> styles;
	std::vector<int> lines;
	agi::fs::path path;
	int count = 0;
};

using FontCollectorEventSink = std::function<void(FontCollectorEvent const&)>;
