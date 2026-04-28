// Copyright (c) 2016, Thomas Goyne <plorkyeran@aegisub.org>
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

extern "C" {
#include "ass_fontselect.h"
}

#undef inline

#include <memory>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "font_file_lister_dwrite.h"

namespace {
DWriteBridge& dwrite_bridge() {
	static DWriteBridge bridge;
	return bridge;
}

class GdiFont {
	HFONT font;
	std::shared_ptr<HDC__> dc;
	IDWriteFontFace *dwrite_face = nullptr;
	bool dwrite_face_attempted = false;

	size_t size = 0;
	std::unique_ptr<char[]> font_data;

	void ensure_dwrite_face() {
		if (dwrite_face_attempted)
			return;
		dwrite_face_attempted = true;
		if (dwrite_bridge().available()) {
			SelectObject(dc.get(), font);
			dwrite_face = dwrite_bridge().CreateFontFaceFromHdc(dc.get());
		}
	}

public:
	GdiFont(HFONT font, std::shared_ptr<HDC__> dc) : font(font), dc(dc) {}
	~GdiFont() {
		if (dwrite_face)
			dwrite_face->Release();
		DeleteObject(font);
	}

	size_t GetData(unsigned char *data, size_t offset, size_t len);
	bool CheckPostscript() { return false; }
		bool CheckGlyph(uint32_t codepoint) {
			if (codepoint == 0)
				return true;
			ensure_dwrite_face();
			if (dwrite_face)
				return dwrite_bridge().HasGlyph(dwrite_face, codepoint);

			// Fallback: GDI GetGlyphIndicesW
			SelectObject(dc.get(), font);
			WORD index = 0xFFFF;
			if (codepoint < 0x10000) {
				auto wch = static_cast<wchar_t>(codepoint);
				GetGlyphIndicesW(dc.get(), &wch, 1, &index, GGI_MARK_NONEXISTING_GLYPHS);
			}
			else {
				wchar_t pair[2] = {
					static_cast<wchar_t>(0xD800 + ((codepoint - 0x10000) >> 10)),
					static_cast<wchar_t>(0xDC00 + ((codepoint - 0x10000) & 0x3FF))
				};
				GetGlyphIndicesW(dc.get(), pair, 2, &index, GGI_MARK_NONEXISTING_GLYPHS);
			}
			return index != 0xFFFF;
		}
	void Destroy() { delete this; }
};

size_t GdiFont::GetData(unsigned char *data, size_t offset, size_t len) {
	if (!font_data) {
		SelectObject(dc.get(), font);
		size = GetFontData(dc.get(), 0, 0, 0, 0);
		if (size == GDI_ERROR)
			return 0;
		font_data.reset(new char[size]);
		GetFontData(dc.get(), 0, 0, font_data.get(), size);
	}

	if (!data)
		return size;
	memcpy(data, font_data.get() + offset, len);
	return len;
}

void match_fonts(ASS_Library *lib, ASS_FontProvider *provider, char *name) {
	std::shared_ptr<HDC__> dc(CreateCompatibleDC(nullptr), [](HDC dc) { DeleteDC(dc); });

	LOGFONTW lf{};
	lf.lfCharSet = DEFAULT_CHARSET;
	MultiByteToWideChar(CP_UTF8, 0, name, -1, lf.lfFaceName, LF_FACESIZE);
	auto cb = [=](LOGFONTW const& lf) {
		ASS_FontProviderMetaData meta{};
		meta.weight = lf.lfWeight;
		meta.slant = lf.lfItalic ? FONT_SLANT_ITALIC : FONT_SLANT_NONE;
		meta.width = FONT_WIDTH_NORMAL;

		meta.families= static_cast<char **>(malloc(sizeof(char *)));
		meta.n_family = 1;

		auto name = static_cast<char *>(malloc(LF_FACESIZE * 4));
		auto len = wcsnlen(lf.lfFaceName, LF_FACESIZE);
		auto written = WideCharToMultiByte(CP_UTF8, 0, lf.lfFaceName, len,
		                                   name, LF_FACESIZE * 4, nullptr, nullptr);
		name[written] = 0;
		meta.families[0] = name;

		auto hfont = CreateFontIndirectW(&lf);
		ass_font_provider_add_font(provider, &meta, nullptr, 0, new GdiFont(hfont, dc));
	};
	using type = decltype(cb);
	EnumFontFamiliesEx(dc.get(), &lf, [](const LOGFONT *lf, const TEXTMETRIC *, DWORD, LPARAM lParam) -> int {
		(*reinterpret_cast<type*>(lParam))(*lf);
		return 1;
	}, (LPARAM)&cb, 0);
}

template <typename T, T> struct wrapper;
template <typename T, typename R, typename... Args, R (T::*method)(Args...)>
struct wrapper<R (T::*)(Args...), method> {
	static R call(void *obj, Args... args) {
		return (static_cast<T*>(obj)->*method)(args...);
	}
};
}

extern "C"
ASS_FontProvider *ass_directwrite_add_provider(ASS_Library *,
                                               ASS_FontSelector *selector,
                                               const char *) {
#define WRAP(method) &wrapper<decltype(&method), &method>::call
	static ASS_FontProviderFuncs callbacks = {
		WRAP(GdiFont::GetData),
		WRAP(GdiFont::CheckPostscript),
		WRAP(GdiFont::CheckGlyph),
		WRAP(GdiFont::Destroy),
		nullptr, // destroy_provider
		&match_fonts,
		nullptr, // get_substitution
		nullptr, // get_fallback
	};
	return ass_font_provider_new(selector, &callbacks, nullptr);
}
