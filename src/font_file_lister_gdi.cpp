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
#include "gdi_font_resolver.h"

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
#include <new>
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

	auto const nblocks = len / 4;
	for (uint32_t i = 0; i < nblocks; ++i) {
		uint32_t k = 0;
		std::memcpy(&k, data + i * 4, sizeof(k));
		k *= c1;
		k = _rotl(k, r1);
		k *= c2;

		hash ^= k;
		hash = _rotl(hash, r2) * m + n;
	}

	uint32_t tail = 0;
	auto const *tail_data = reinterpret_cast<unsigned char const *>(data + nblocks * 4);
	switch (len & 3U) {
		case 3: tail ^= static_cast<uint32_t>(tail_data[2]) << 16; [[fallthrough]];
		case 2: tail ^= static_cast<uint32_t>(tail_data[1]) << 8; [[fallthrough]];
		case 1:
			tail ^= tail_data[0];
			tail *= c1;
			tail = _rotl(tail, r1);
			tail *= c2;
			hash ^= tail;
			break;
		default: break;
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

std::wstring SelectedFaceNameFull(HDC dc) {
	wchar_t face[256] = {};
	if (GetTextFaceW(dc, static_cast<int>(sizeof(face) / sizeof(face[0])), face) <= 0)
		return {};
	return face;
}

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
			font_path.make_preferred();
			auto const duplicate = std::any_of(files.begin(), files.end(), [&](agi::fs::path const& existing) {
				auto const& left = existing.native();
				auto const& right = font_path.native();
				return CompareStringOrdinal(
					left.data(), static_cast<int>(left.size()),
					right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
			});
			if (!duplicate)
				files.push_back(std::move(font_path));
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
			auto const read_size = stream->gcount();
			if (read_size <= 0 || read_size > 1024)
				continue;
			auto hash = murmur3(&buffer[0], static_cast<uint32_t>(read_size));
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

bool get_font_data(std::string& buffer, HDC dc) {
	constexpr DWORD max_font_bytes = 64u * 1024u * 1024u;
	buffer.clear();

	// For ttc files we have to ask for the "ttcf" table to get the complete file
	DWORD ttcf = 0x66637474;
	auto size = GetFontData(dc, ttcf, 0, nullptr, 0);
	if (size == GDI_ERROR) {
		ttcf = 0;
		size = GetFontData(dc, 0, 0, nullptr, 0);
	}
	if (size == GDI_ERROR || size == 0 || size > max_font_bytes)
		return false;

	try {
		buffer.resize(size);
	}
	catch (std::bad_alloc const&) {
		return false;
	}
	auto const bytes_read = GetFontData(dc, ttcf, 0, buffer.data(), size);
	if (bytes_read == GDI_ERROR || bytes_read != size) {
		buffer.clear();
		return false;
	}
	return true;
}
}

GdiFontFileLister::~GdiFontFileLister() = default;

GdiFontFileLister::GdiFontFileLister(FontCollectorEventSink &cb)
: resolver(std::make_unique<GdiFontResolver>())
{
	FontCollectorEvent event;
	if (resolver->dwrite_available()) {
		event.type = FontCollectorEventType::FontBackendInfo;
		event.message = resolver->dwrite_description();
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
	if (!resolver->dwrite_available())
		index = index_fonts(cb);
}

FontFileListerMatchKey GdiFontFileLister::GetMatchKey(
	aegisub::ass::AssFontRequest const& request) const {
	FontFileListerMatchKey key;
	key.family = request.family;
	key.weight = request.effective_weight;
	key.italic = request.italic;
	key.charset = request.charset < 0
		? aegisub::ass::DefaultCharset
		: std::min(request.charset, 255);
	key.height = QuantizeAssHeightForGdiProbe(request.height);
	return key;
}

CollectionResult GdiFontFileLister::GetFontPaths(
	std::string const& facename,
	int bold,
	bool italic,
	std::vector<uint32_t> const& characters) {
	auto const weight = bold == 0 ? FW_NORMAL : bold == 1 ? FW_BOLD : bold;
	return GetFontPathsImpl(facename, weight, italic, DEFAULT_CHARSET, 0, characters);
}

CollectionResult GdiFontFileLister::GetFontPaths(
	aegisub::ass::AssFontRequest const& request,
	std::vector<uint32_t> const& characters) {
	auto result = GetFontPathsImpl(
		request.family,
		request.effective_weight,
		request.italic,
		request.charset,
		QuantizeAssHeightForGdiProbe(request.height),
		characters);
	result.backend_requested_weight = request.effective_weight;
	if (result.realized_status) {
		auto const role = result.realized_role.value_or(FontVariantRole::Unknown);
		bool const realized_bold = role == FontVariantRole::Bold ||
		                           role == FontVariantRole::BoldItalic;
		bool const realized_italic = role == FontVariantRole::Italic ||
		                             role == FontVariantRole::BoldItalic;
		result.implicit_variant_fallback =
			*result.realized_status == FontVariantStatus::Canonical &&
			((realized_bold && request.effective_weight == 400 &&
			  !request.has_explicit_bold) ||
			 (realized_italic && !request.italic &&
			  !request.has_explicit_italic));
	}
	return result;
}

CollectionResult GdiFontFileLister::GetFontPathsImpl(
	std::string const& facename,
	int weight,
	bool italic,
	int charset,
	int height,
	std::vector<uint32_t> const& characters) {
	CollectionResult ret;
	ret.requested_weight = weight;
	if (!resolver)
		return ret;

	auto probe = resolver->ProbeWithSelection(
		facename, weight, italic, charset, height,
		[&](GdiFontSelectionView const& selected) {
			if (!selected.dc || !selected.probe)
				return;

			ret.matched_facename = selected.probe->selected_family_name;
			auto selected_face_full = SelectedFaceNameFull(selected.dc);
			if (!selected_face_full.empty())
				ret.matched_facename_full = agi::charset::ConvertW(selected_face_full);
			for (auto const& name : selected.probe->win32_family_names) {
				if (!name.value.empty() &&
				    std::find(ret.matched_names.begin(), ret.matched_names.end(), name.value) == ret.matched_names.end())
					ret.matched_names.push_back(name.value);
			}
			if (!selected.probe->matches_requested_family)
				return;

			// The resolver has already combined OS/2 metadata with the selected
			// HDC face. TEXTMETRIC may report a requested/synthetic weight, so it
			// must not override the intrinsic realized variant here.
			ret.matched_weight = selected.probe->outcome.realized_weight;
			auto const role = selected.probe->outcome.role;
			ret.matched_bold = role == FontVariantRole::Bold ||
			                   role == FontVariantRole::BoldItalic;
			ret.matched_italic = selected.probe->outcome.realized_italic;

			if (selected.dwrite_face && selected.dwrite_bridge) {
				if (!selected.local_file_path.empty()) {
					ret.paths.push_back(agi::fs::PathFromString(
						std::string(selected.local_file_path)));
					ret.path_source = "dwrite";
					ret.face_index = selected.face_index;
				}
				if (ret.face_index < 0)
					ret.face_index = static_cast<int>(selected.dwrite_face->GetIndex());

				if (ret.paths.empty()) {
					std::vector<char> bytes;
					if (selected.dwrite_bridge->ReadFontData(selected.dwrite_face, bytes) && !bytes.empty()) {
						FontMemoryFont memory_font;
						memory_font.facename = !ret.matched_facename_full.empty() ? ret.matched_facename_full :
						                           !ret.matched_facename.empty() ? ret.matched_facename : facename;
						memory_font.data = std::make_shared<std::vector<char> const>(std::move(bytes));
						ret.memory_fonts.push_back(std::move(memory_font));
					}
				}

				auto const faux = DetectFauxStylesDWrite(selected.dwrite_face);
				ret.fake_bold = faux.faux_bold;
				ret.fake_italic = faux.faux_italic;
			}

			// A successful DWrite memory read is already the selected entity. Only
			// ask GDI for the complete font when DWrite supplied neither form.
			if (ret.paths.empty() && ret.memory_fonts.empty() &&
			    get_font_data(buffer, selected.dc)) {
				auto range = index.equal_range(murmur3(
					buffer.data(), static_cast<uint32_t>(std::min<std::size_t>(buffer.size(), 1024u))));
				std::vector<char> file_buffer(buffer.size());
				for (auto it = range.first; it != range.second; ++it) {
					try {
						auto stream = agi::io::Open(it->second, true);
						stream->read(file_buffer.data(), static_cast<std::streamsize>(file_buffer.size()));
						if (stream->gcount() != static_cast<std::streamsize>(file_buffer.size()))
							continue;
						if (std::memcmp(file_buffer.data(), buffer.data(), buffer.size()) == 0) {
							ret.paths.push_back(it->second);
							ret.path_source = "gdi";
							break;
						}
					}
					catch (agi::Exception const&) {
						continue;
					}
				}

				if (ret.paths.empty()) {
					FontMemoryFont memory_font;
					memory_font.facename = !ret.matched_facename_full.empty() ? ret.matched_facename_full :
					                           !ret.matched_facename.empty() ? ret.matched_facename : facename;
					memory_font.data = std::make_shared<std::vector<char> const>(buffer.begin(), buffer.end());
					ret.memory_fonts.push_back(std::move(memory_font));
				}
			}
			if (!ret.paths.empty())
				ret.memory_fonts.clear();

			std::wstring utf16characters;
			utf16characters.reserve(characters.size());
			for (auto chr : characters) {
				font_collector::unicode::Rune rune;
				if (!font_collector::unicode::Rune::TryCreate(chr, rune))
					continue;
				wchar_t encoded[2];
				int chars_written = 0;
				if (rune.TryEncodeToUtf16(encoded, chars_written))
					utf16characters.append(encoded, chars_written);
			}
			if (utf16characters.empty())
				return;

			SCRIPT_CACHE cache = nullptr;
			auto release_cache = agi::make_scope_exit([&] { ScriptFreeCache(&cache); });
			std::vector<WORD> indices(utf16characters.size());
			auto hr = ScriptGetCMap(
				selected.dc, &cache, utf16characters.data(),
				static_cast<int>(utf16characters.size()), 0, indices.data());

			if (hr == E_HANDLE) {
				GetGlyphIndicesW(
					selected.dc, utf16characters.data(),
					static_cast<int>(utf16characters.size()), indices.data(),
					GGI_MARK_NONEXISTING_GLYPHS);
				for (std::size_t index = 0; index < utf16characters.size(); ++index) {
					if (!font_collector::unicode::IsSurrogate(utf16characters[index]) &&
					    indices[index] == SHRT_MAX)
						append_utf16_to_utf8(ret.missing, utf16characters[index]);
				}
			}
			else if (hr == S_FALSE) {
				for (std::size_t index = 0; index < utf16characters.size(); ++index) {
					if (font_collector::unicode::IsHighSurrogate(utf16characters[index])) {
						hr = ScriptGetCMap(
							selected.dc, &cache, &utf16characters[index], 2, 0, &indices[index]);
						if (hr == S_FALSE)
							append_utf16_pair_to_utf8(
								ret.missing, utf16characters[index], utf16characters[index + 1]);
						++index;
					}
					else if (indices[index] == 0) {
						append_utf16_to_utf8(ret.missing, utf16characters[index]);
					}
				}
			}
		});

	ret.realized_status = probe.outcome.status;
	if (probe.outcome.role != FontVariantRole::Unknown)
		ret.realized_role = probe.outcome.role;
	ret.noncanonical_variant = probe.outcome.status != FontVariantStatus::Canonical;

	return ret;
}
