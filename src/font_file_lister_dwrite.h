// Copyright (c) 2026, MIRIMIRIM

#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

struct IDWriteFactory;
struct IDWriteGdiInterop;
struct IDWriteFontFace;

/// One localized string entry from DirectWrite (value + locale tag).
struct DWriteLocalizedName {
	std::string value;
	std::string locale;
};

/// @class DWriteBridge
/// @brief Bridges GDI font selection to DirectWrite for file path and metadata.
class DWriteBridge {
	IDWriteFactory *factory = nullptr;
	IDWriteGdiInterop *gdi_interop = nullptr;
	HMODULE dll_handle = nullptr;
	bool available_ = false;
	bool is_dwritecore_ = false;
	std::string dll_description_;

public:
	DWriteBridge();
	~DWriteBridge();

	DWriteBridge(DWriteBridge const&) = delete;
	DWriteBridge& operator=(DWriteBridge const&) = delete;

	bool available() const { return available_; }
	bool is_dwritecore() const { return is_dwritecore_; }
	std::string const& dll_description() const { return dll_description_; }

	/// Create a face via GDI interop (system DWrite path).
	IDWriteFontFace *CreateFontFaceFromHdc(HDC hdc) const;
	
	/// Create a face from LOGFONT via DWrite font system.
	IDWriteFontFace *CreateFontFaceFromLogFont(LOGFONTW const &lf) const;

	/// Get localized family aliases for a GDI-selected LOGFONT.
	std::vector<std::string> GetFontFamilyNamesFromLogFont(LOGFONTW const &lf) const;

	/// Get Win32 family names (all locales) for a GDI-selected LOGFONT.
	/// Prefer DWRITE_INFORMATIONAL_STRING_WIN32_FAMILY_NAMES; fall back to
	/// IDWriteFontFamily::GetFamilyNames when informational strings are absent.
	std::vector<DWriteLocalizedName> GetWin32FamilyNamesFromLogFont(LOGFONTW const &lf) const;
	
	/// Get the font file path from an IDWriteFontFace.
	/// @param face DWrite font face
	/// @param[out] out_path Receives the file path (UTF-8)
	/// @param[out] out_face_index Receives the face index in collection, or 0
	/// @return true on success
	bool GetFontFilePath(IDWriteFontFace *face, std::string &out_path, int &out_face_index) const;

	/// Check glyph coverage via DWrite.
	/// @param face DWrite font face
	/// @param codepoint Unicode scalar value
	/// @return true if the font contains the glyph
	bool HasGlyph(IDWriteFontFace *face, uint32_t codepoint) const;
	
	/// Read raw font data via DWrite font file stream.
	/// @param face DWrite font face
	/// @param[out] out_bytes Receives raw font bytes
	/// @return true on success
	bool ReadFontData(IDWriteFontFace *face, std::vector<char> &out_bytes) const;
};

/// Determine font format from magic bytes.
/// Note: 'ttcf' covers both TTC (TrueType) and OTC (CFF) collections.
enum class FontFormat { Unknown, TrueType, OpenType, Collection, Woff };
FontFormat DetermineFontFormat(std::span<const char, 4> data);

/// Map font format to file extension (without dot).
const char *FontFormatExtension(FontFormat format);
