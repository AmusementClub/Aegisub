// Copyright (c) 2026, MIRIMIRIM

#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct IDWriteFactory;
struct IDWriteGdiInterop;
struct IDWriteFontFace;
struct IDWriteFont;
struct FontMatchCandidate;

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
	IDWriteFontFace *CreateFontFaceFromFont(IDWriteFont *font) const;
	bool BuildFontCatalog(std::vector<FontMatchCandidate>& faces,
	                      std::vector<IDWriteFontFace *>& dwrite_faces,
	                      std::vector<std::string> const& additional_font_files,
	                      bool include_system_fonts,
	                      std::string& error) const;

	/// Resolve system-preferred fallback family for a codepoint via IDWriteTextLayout
	/// (same approach as libass ass_directwrite.c get_fallback).
	std::optional<std::string> ResolveSystemFallbackFamily(uint32_t codepoint) const;

	/// Get localized family aliases for a GDI-selected LOGFONT.
	std::vector<std::string> GetFontFamilyNamesFromLogFont(LOGFONTW const &lf) const;

	/// Get Win32 family names (all locales) for a GDI-selected LOGFONT.
	/// Prefer DWRITE_INFORMATIONAL_STRING_WIN32_FAMILY_NAMES; fall back to
	/// IDWriteFontFamily::GetFamilyNames when informational strings are absent.
	std::vector<DWriteLocalizedName> GetWin32FamilyNamesFromLogFont(LOGFONTW const &lf) const;
	std::vector<DWriteLocalizedName> GetWin32FamilyNamesFromFont(IDWriteFont *font) const;
	std::vector<std::string> GetFullNamesFromFont(IDWriteFont *font) const;
	std::string GetPostScriptNameFromFont(IDWriteFont *font) const;
	
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

	/// Read a font file through its DirectWrite loader when no local path exists.
	bool ReadFontData(IDWriteFontFace *face, std::vector<char>& out_bytes) const;
};
