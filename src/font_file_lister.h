// Copyright (c) 2012, Thomas Goyne <plorkyeran@aegisub.org>
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

#include "font_collector_events.h"

#include <libaegisub/fs_fwd.h>
#include <libaegisub/scoped_ptr.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>
#include <unordered_map>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

class AssDialogue;
class AssFile;

struct FontRawData {
	/// Raw font file bytes (from GetFontData or IDWriteFontFileStream).
	std::vector<char> bytes;
};

struct CollectionResult {
	/// Font face selected by the platform matcher.
	std::string matched_facename;
	int face_index = -1;
	/// Font weight selected by the platform matcher.
	int matched_weight = 0;
	/// Whether the selected platform font is bold.
	bool matched_bold = false;
	/// Whether the selected platform font is italic.
	bool matched_italic = false;
	/// Whether the font is in a TrueType/OpenType collection.
	bool is_collection = false;
	/// Characters which could not be found in any font files
	std::string missing;
	/// Paths to the file(s) containing the requested font
	std::vector<agi::fs::path> paths;
	/// How the font file path was resolved: "dwritecore", "dwrite", "gdi", or empty.
	std::string path_source;
	/// Raw font data, usable when the file path is unavailable.
	FontRawData raw_data;
	bool fake_bold = false;
	bool fake_italic = false;
	/// The lfWeight value passed to CreateFontIndirectW.
	int requested_weight = 0;
};

struct FontCollectorMatchedFont {
	std::string facename;
	int face_index = -1;
	int weight = 0;
	bool bold = false;
	bool italic = false;
	bool is_collection = false;
	std::vector<agi::fs::path> paths;
	std::string path_source;
	FontRawData raw_data;
	bool fake_bold = false;
	bool fake_italic = false;
	/// libass-style synthetic detection from platform-neutral common layer (opt-in)
	bool libass_fake_bold = false;
	bool libass_fake_italic = false;
	int libass_score = 0;
	std::string missing_chars;
	int requested_weight = 0;
};

struct FontCollectorAssFontUsage {
	std::string ass_facename;
	int ass_bold = 0;
	bool ass_italic = false;
	std::vector<uint32_t> chars;
	std::vector<std::string> styles;
	std::vector<int> override_lines;
	FontCollectorMatchedFont matched;
};

struct FontCollectorDetails {
	std::vector<FontCollectorAssFontUsage> fonts;
};

class DWriteBridge;

#ifdef _WIN32
class GdiFontFileLister {
	std::unique_ptr<DWriteBridge> dwrite_bridge;
	std::unordered_multimap<uint32_t, agi::fs::path> index;
	agi::scoped_holder<HDC> dc;
	std::string buffer;

	bool ProcessLogFont(LOGFONTW const& expected, LOGFONTW const& actual, std::vector<uint32_t> const& characters);

public:
	/// Constructor
	/// @param cb Callback for status logging
	GdiFontFileLister(FontCollectorEventSink &cb);
	~GdiFontFileLister();

	/// @brief Get the path to the font with the given styles
	/// @param facename Name of font face
	/// @param bold ASS font weight
	/// @param italic Italic?
	/// @param characters Characters in this style
	/// @return Path to the matching font file(s), or empty if not found
	CollectionResult GetFontPaths(std::string const& facename, int bold, bool italic, std::vector<uint32_t> const& characters);
};

using FontFileLister = GdiFontFileLister;

#elif defined(__APPLE__)

struct CoreTextFontFileLister {
	CoreTextFontFileLister(FontCollectorEventSink &) {}

	/// @brief Get the path to the font with the given styles
	/// @param facename Name of font face
	/// @param bold ASS font weight
	/// @param italic Italic?
	/// @param characters Characters in this style
	/// @return Path to the matching font file(s), or empty if not found
	CollectionResult GetFontPaths(std::string const& facename, int bold, bool italic, std::vector<uint32_t> const& characters);
};

using FontFileLister = CoreTextFontFileLister;

#else

typedef struct _FcConfig FcConfig;
typedef struct _FcFontSet FcFontSet;

/// @class FontConfigFontFileLister
/// @brief fontconfig powered font lister
class FontConfigFontFileLister {
	agi::scoped_holder<FcConfig*> config;

	/// @brief Case-insensitive match ASS/SSA font family against full name. (also known as "name for humans")
	/// @param family font fullname
	/// @param bold weight attribute
	/// @param italic italic attribute
	/// @return font set
	FcFontSet *MatchFullname(const char *family, int weight, int slant);
public:
	/// Constructor
	/// @param cb Callback for status logging
	FontConfigFontFileLister(FontCollectorEventSink &cb);

	/// @brief Get the path to the font with the given styles
	/// @param facename Name of font face
	/// @param bold ASS font weight
	/// @param italic Italic?
	/// @param characters Characters in this style
	/// @return Path to the matching font file(s), or empty if not found
	CollectionResult GetFontPaths(std::string const& facename, int bold, bool italic, std::vector<uint32_t> const& characters);
};

using FontFileLister = FontConfigFontFileLister;
#endif

/// @class FontCollector
/// @brief Class which collects the paths to all fonts used in a script
class FontCollector {
	/// All data needed to find the font file used to render text
	struct StyleInfo {
		std::string facename;
		int bold;
		bool italic;
		bool operator<(StyleInfo const& rgt) const;
	};

	/// Data about where each style is used
	struct UsageData {
		std::vector<uint32_t> chars;     ///< Characters used in this style which glyphs will be needed for
		std::vector<int> lines;          ///< Lines on which this style is used via overrides
		std::vector<std::string> styles; ///< ASS styles which use this style
	};

	/// Message callback provider by caller
	FontCollectorEventSink event_sink;

	FontFileLister lister;

	/// The set of all glyphs used in the file
	std::map<StyleInfo, UsageData> used_styles;
	/// Style name -> ASS style definition
	std::map<std::string, StyleInfo> styles;
	/// Paths to found required font files
	std::vector<agi::fs::path> results;
	/// Number of fonts which could not be found
	int missing = 0;
	/// Number of fonts which were found, but did not contain all used glyphs
	int missing_glyphs = 0;

	/// When true, compute libass_fake_bold/italic/score from platform metadata
	bool enable_libass_compat_ = false;

	/// Gather all of the unique styles with text on a line
	void ProcessDialogueLine(const AssDialogue *line, int index, int wrap_style);

	/// Get the font for a single style
	void ProcessChunk(std::pair<StyleInfo, UsageData> const& style, FontCollectorDetails *details);

	/// Print the lines and styles on which a missing font is used
	void PrintUsage(UsageData const& data);

public:
	/// Constructor
	/// @param status_callback Function to pass status updates to
	/// @param lister The actual font file lister
	FontCollector(FontCollectorEventSink event_sink);

	/// Enable libass-style synthetic detection for cross-reference (opt-in).
	/// When enabled, libass_fake_bold, libass_fake_italic, and libass_score
	/// are computed from the platform lister's matched_weight/bold/italic
	/// using the platform-neutral common layer.
	/// Disabled by default; set to true for diagnostic/CLI use.
	void EnableLibassCompat(bool enable) { enable_libass_compat_ = enable; }

	/// @brief Get a list of the locations of all font files used in the file
	/// @param file Lines in the subtitle file to check
	/// @param status Callback function for messages
	/// @return List of paths to fonts
	std::vector<agi::fs::path> GetFontPaths(const AssFile *file, FontCollectorDetails *details = nullptr);
};
