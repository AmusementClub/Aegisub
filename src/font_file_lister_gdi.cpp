// Copyright (c) 2016, Thomas Goyne <plorkyeran@aegisub.org>
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

#include "font_file_lister.h"
#include "font_file_lister_dwrite.h"

#include "font_collector_unicode.h"

#include <libaegisub/charset_conv_win.h>
#include <libaegisub/fs.h>
#include <libaegisub/io.h>
#include <libaegisub/scope_exit.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <dwrite.h>
#include <memory>
#include <ShlObj.h>
#include <Usp10.h>
#include <vector>

namespace {
void Emit(FontCollectorEventSink const& sink, FontCollectorEvent event) {
	if (sink)
		sink(event);
}

void append_utf16_to_utf8(std::string& out, wchar_t ch) {
	char buf[4];
	auto len = WideCharToMultiByte(CP_UTF8, 0, &ch, 1, buf, sizeof(buf), nullptr, nullptr);
	if (len > 0) out.append(buf, len);
}

void append_utf16_pair_to_utf8(std::string& out, wchar_t lead, wchar_t trail) {
	wchar_t pair[2] = {lead, trail};
	char buf[4];
	auto len = WideCharToMultiByte(CP_UTF8, 0, pair, 2, buf, sizeof(buf), nullptr, nullptr);
	if (len > 0) out.append(buf, len);
}

uint32_t murmur3(const char *data, uint32_t len) {
	static const uint32_t c1 = 0xcc9e2d51;
	static const uint32_t c2 = 0x1b873593;
	static const uint32_t r1 = 15;
	static const uint32_t r2 = 13;
	static const uint32_t m = 5;
	static const uint32_t n = 0xe6546b64;

	uint32_t hash = 0;

	const int nblocks = len / 4;
	auto blocks = reinterpret_cast<const uint32_t *>(data);
	for (uint32_t i = 0; i * 4 < len; ++i) {
		uint32_t k = blocks[i];
		k *= c1;
		k = _rotl(k, r1);
		k *= c2;

		hash ^= k;
		hash = _rotl(hash, r2) * m + n;
	}

	hash ^= len;
	hash ^= hash >> 16;
	hash *= 0x85ebca6b;
	hash ^= hash >> 13;
	hash *= 0xc2b2ae35;
	hash ^= hash >> 16;

	return hash;
}

struct FauxResult { bool faux_bold = false; bool faux_italic = false; };

// FauxResult DetectFauxStyles(HDC dc, LOGFONTW& lf, TEXTMETRICW const& metrics,
//                             int requested_bold, bool requested_italic) {
// 	struct FamilyBits { bool bold = false; bool italic = false; };
// 	FamilyBits bits;
// 	EnumFontFamiliesExW(dc, &lf,
// 		[](LOGFONTW const *member, TEXTMETRICW const *, DWORD, LPARAM lParam) -> int {
// 			auto *b = reinterpret_cast<FamilyBits *>(lParam);
// 			if (static_cast<int>(member->lfWeight) >= 600) b->bold = true;
// 			if (member->lfItalic) b->italic = true;
// 			return 1;
// 		}, reinterpret_cast<LPARAM>(&bits), 0);

// 	bool const synthesizing = (metrics.tmOverhang != 0);

// 	FauxResult result;
// 	result.faux_italic = requested_italic && !bits.italic && synthesizing;
// 	result.faux_bold = (requested_bold != 0) && !bits.bold && synthesizing;
// 	return result;
// }

FauxResult DetectFauxStylesDWrite(IDWriteFontFace *face) {
	FauxResult result;
	if (!face) return result;
	auto const sims = face->GetSimulations();
	result.faux_bold = (sims & DWRITE_FONT_SIMULATIONS_BOLD) != 0;
	result.faux_italic = (sims & DWRITE_FONT_SIMULATIONS_OBLIQUE) != 0;
	return result;
}

std::vector<agi::fs::path> get_installed_fonts() {
	static const auto fonts_key_name = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Fonts";

	std::vector<agi::fs::path> files;

	for (HKEY hKey : { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE }) {
		HKEY key;
		auto ret = RegOpenKeyExW(hKey, fonts_key_name, 0, KEY_QUERY_VALUE, &key);
		if (ret != ERROR_SUCCESS) continue;
		auto close_key = agi::make_scope_exit([=] { RegCloseKey(key); });

		wchar_t fdir[MAX_PATH];
		SHGetFolderPathW(NULL, CSIDL_FONTS, NULL, 0, fdir);
		agi::fs::path font_dir(fdir);

		for (DWORD i = 0;; ++i) {
			WCHAR font_name[SHRT_MAX];
			std::vector<WCHAR> font_filename(MAX_PATH);
			DWORD name_len = static_cast<DWORD>(sizeof(font_name) / sizeof(font_name[0]));
			DWORD data_len = static_cast<DWORD>(font_filename.size() * sizeof(WCHAR));

			ret = RegEnumValueW(key, i, font_name, &name_len, NULL, NULL, reinterpret_cast<BYTE*>(font_filename.data()), &data_len);
			if (ret == ERROR_MORE_DATA) {
				name_len = static_cast<DWORD>(sizeof(font_name) / sizeof(font_name[0]));
				font_filename.resize((data_len + sizeof(WCHAR) - 1) / sizeof(WCHAR));
				ret = RegEnumValueW(key, i, font_name, &name_len, NULL, NULL, reinterpret_cast<BYTE*>(font_filename.data()), &data_len);
			}
			if (ret == ERROR_NO_MORE_ITEMS) break;
			if (ret != ERROR_SUCCESS) continue;

			agi::fs::path font_path(font_filename.data());
			if (!agi::fs::FileExists(font_path) && agi::fs::FileExists(font_dir / font_path))
				font_path = font_dir / font_path;
			files.push_back(font_path);
		}
	}

	return files;
}

using font_index = std::unordered_multimap<uint32_t, agi::fs::path>;

font_index index_fonts(FontCollectorEventSink &cb) {
	font_index hash_to_path;
	auto fonts = get_installed_fonts();
	std::unique_ptr<char[]> buffer(new char[1024]);
	for (auto const& path : fonts) {
		try {
			auto stream = agi::io::Open(path, true);
			stream->read(&buffer[0], 1024);
			auto hash = murmur3(&buffer[0], stream->tellg());
			hash_to_path.emplace(hash, path);
		}
		catch (agi::Exception const& e) {
			FontCollectorEvent event;
			event.type = FontCollectorEventType::FontCacheError;
			event.message = e.GetMessage();
			Emit(cb, std::move(event));
		}
	}
	return hash_to_path;
}

void get_font_data(std::string& buffer, HDC dc) {
	buffer.clear();

	// For ttc files we have to ask for the "ttcf" table to get the complete file
	DWORD ttcf = 0x66637474;
	auto size = GetFontData(dc, ttcf, 0, nullptr, 0);
	if (size == GDI_ERROR) {
		ttcf = 0;
		size = GetFontData(dc, 0, 0, nullptr, 0);
	}
	if (size == GDI_ERROR || size == 0)
		return;

	buffer.resize(size);
	GetFontData(dc, ttcf, 0, &buffer[0], size);
}
}

GdiFontFileLister::~GdiFontFileLister() = default;

GdiFontFileLister::GdiFontFileLister(FontCollectorEventSink &cb)
: dwrite_bridge(std::make_unique<DWriteBridge>())
, dc(CreateCompatibleDC(nullptr), [](HDC dc) { DeleteDC(dc); })
{
	FontCollectorEvent event;
	if (dwrite_bridge->available()) {
		event.type = FontCollectorEventType::FontBackendInfo;
		event.message = dwrite_bridge->dll_description();
		Emit(cb, std::move(event));
	}
	else {
		event.type = FontCollectorEventType::FontBackendInfo;
		event.message = "GDI (DWrite unavailable)";
		Emit(cb, std::move(event));
	}

	event = FontCollectorEvent();
	event.type = FontCollectorEventType::UpdatingFontCache;
	Emit(cb, std::move(event));
	if (!dwrite_bridge->available())
		index = index_fonts(cb);
}

CollectionResult GdiFontFileLister::GetFontPaths(std::string const& facename, int bold, bool italic, std::vector<uint32_t> const& characters) {
	CollectionResult ret;

	LOGFONTW lf{};
	lf.lfCharSet = DEFAULT_CHARSET;
	wcsncpy(lf.lfFaceName, agi::charset::ConvertW(facename).c_str(), LF_FACESIZE);
	lf.lfItalic = italic ? -1 : 0;
	lf.lfWeight = bold == 0 ? 400 :
	              bold == 1 ? 700 :
	                          bold;
	lf.lfCharSet = DEFAULT_CHARSET;
	lf.lfOutPrecision = OUT_TT_PRECIS;
	lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
	lf.lfQuality = ANTIALIASED_QUALITY;
	lf.lfPitchAndFamily = DEFAULT_PITCH|FF_DONTCARE;
	ret.requested_weight = lf.lfWeight;

	auto hfont = CreateFontIndirectW(&lf);
	if (!hfont) return ret;
	SelectObject(dc, hfont);
	auto release_font = agi::make_scope_exit([=] {
		SelectObject(dc, nullptr);
		DeleteObject(hfont);
	});

	// Get the actual face name GDI selected.
	wchar_t selected_face[LF_FACESIZE] = {};
	if (GetTextFaceW(dc, LF_FACESIZE, selected_face) > 0)
		ret.matched_facename = agi::charset::ConvertW(selected_face);
	TEXTMETRICW metrics = {};
	if (GetTextMetricsW(dc, &metrics)) {
		ret.matched_weight = metrics.tmWeight;
		ret.matched_bold = metrics.tmWeight > 550;
		ret.matched_italic = metrics.tmItalic != 0;
	}

	// --- DWrite bridge: try to get path, simulations and raw data ---
	if (dwrite_bridge->available()) {
		IDWriteFontFace *dw_face;
		if (dwrite_bridge->is_dwritecore()) {
			LOGFONT lf_actual{};
			GetObject(hfont, sizeof(LOGFONT), &lf_actual);
			dw_face = dwrite_bridge->CreateFontFaceFromLogFont(lf_actual);
		} else {
			dw_face = dwrite_bridge->CreateFontFaceFromHdc(dc);
		}
		if (dw_face) {
			std::string dw_path;
			int dw_index = -1;
			if (dwrite_bridge->GetFontFilePath(dw_face, dw_path, dw_index))
				ret.path_source = dwrite_bridge->is_dwritecore() ? "dwritecore" : "dwrite";

			if (!dw_path.empty()) {
				ret.paths.push_back(agi::fs::PathFromString(dw_path));
				ret.face_index = dw_index;
			}

			if (ret.face_index < 0)
				ret.face_index = static_cast<int>(dw_face->GetIndex());

			dwrite_bridge->ReadFontData(dw_face, ret.raw_data.bytes);

			auto dw_faux = DetectFauxStylesDWrite(dw_face);
			if (dw_faux.faux_bold) ret.fake_bold = true;
			if (dw_faux.faux_italic) ret.fake_italic = true;

			dw_face->Release();
		}
	}
	// else {
	// 	auto faux = DetectFauxStyles(dc, lf, metrics, bold, italic);
	// 	ret.fake_bold = faux.faux_bold;
	// 	ret.fake_italic = faux.faux_italic;
	// }

	// --- GDI fallback: path via registry + hash, raw_data via GetFontData ---
	if (ret.paths.empty() || ret.raw_data.bytes.empty()) {
		// Ensure we have the raw GDI font data.
		if (ret.raw_data.bytes.empty())
			get_font_data(buffer, dc);

		// Try to find the file path from the registry index.
		if (ret.paths.empty() && !buffer.empty()) {
			auto range = index.equal_range(murmur3(buffer.c_str(), std::min<size_t>(buffer.size(), 1024U)));
			std::unique_ptr<char[]> file_buffer(new char[buffer.size()]);
			for (auto it = range.first; it != range.second; ++it) {
				auto stream = agi::io::Open(it->second, true);
				stream->read(&file_buffer[0], buffer.size());
				if ((size_t)stream->tellg() != buffer.size())
					continue;
				if (memcmp(&file_buffer[0], &buffer[0], buffer.size()) == 0) {
					ret.paths.push_back(it->second);
					ret.path_source = "gdi";
					break;
				}
			}
		}

		// Fill raw_data from GDI if DWrite didn't provide it.
		if (ret.raw_data.bytes.empty() && !buffer.empty()) {
			ret.raw_data.bytes.assign(buffer.begin(), buffer.end());
		}
	}
	if (ret.raw_data.bytes.size() >= 4 && ret.raw_data.bytes.data())
		ret.is_collection = (DetermineFontFormat(std::span<const char, 4>(ret.raw_data.bytes.data(), 4)) == FontFormat::Collection);

	// Convert the characters to a utf-16 string
	std::wstring utf16characters;
	utf16characters.reserve(characters.size());
	for (auto chr : characters) {
		font_collector::unicode::Rune rune;
		if (!font_collector::unicode::Rune::TryCreate(chr, rune))
			continue;
		wchar_t buffer[2];
		int chars_written = 0;
		if (rune.TryEncodeToUtf16(buffer, chars_written))
			utf16characters.append(buffer, chars_written);
	}

	SCRIPT_CACHE cache = nullptr;
	std::unique_ptr<WORD[]> indices(new WORD[utf16characters.size()]);

	// First try to check glyph coverage with Uniscribe, since it
	// handles non-BMP unicode characters
	auto hr = ScriptGetCMap(dc, &cache, utf16characters.data(),
		utf16characters.size(), 0, indices.get());

	// Uniscribe doesn't like some types of fonts, so fall back to GDI
	if (hr == E_HANDLE) {
		GetGlyphIndicesW(dc, utf16characters.data(), utf16characters.size(),
			indices.get(), GGI_MARK_NONEXISTING_GLYPHS);
		for (size_t i = 0; i < utf16characters.size(); ++i) {
			if (font_collector::unicode::IsSurrogate(utf16characters[i]))
				continue;
			if (indices[i] == SHRT_MAX)
				append_utf16_to_utf8(ret.missing, utf16characters[i]);
		}
	}
	else if (hr == S_FALSE) {
		for (size_t i = 0; i < utf16characters.size(); ++i) {
			// Uniscribe doesn't report glyph indexes for non-BMP characters,
			// so we have to call ScriptGetCMap on each individual pair to
			// determine if it's the missing one
			if (font_collector::unicode::IsHighSurrogate(utf16characters[i])) {
				hr = ScriptGetCMap(dc, &cache, &utf16characters[i], 2, 0, &indices[i]);
				if (hr == S_FALSE) {
					append_utf16_pair_to_utf8(ret.missing, utf16characters[i], utf16characters[i + 1]);
				}
				++i;
			}
			else if (indices[i] == 0) {
				append_utf16_to_utf8(ret.missing, utf16characters[i]);
			}
		}
	}
	ScriptFreeCache(&cache);

	return ret;
}
