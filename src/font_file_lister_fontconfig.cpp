// Copyright (c) 2012, Thomas Goyne <plorkyeran@aegisub.org>
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

#include "font_collector_unicode.h"

#include <libaegisub/string_utils.h>

#include <fontconfig/fontconfig.h>
#include <cmath>
#include <cwctype>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {
void Emit(FontCollectorEventSink const& sink, FontCollectorEvent event) {
	if (sink)
		sink(event);
}

#ifdef _WIN32
std::string WideToUtf8(std::wstring const& value) {
	if (value.empty())
		return {};

	auto len = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
	if (len <= 0)
		return {};

	std::string text(len, '\0');
	WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), text.data(), len, nullptr, nullptr);
	return text;
}

bool DirectoryExists(std::wstring const& path) {
	auto attributes = GetFileAttributesW(path.c_str());
	return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

bool IsFontFile(std::filesystem::path const& path) {
	auto ext = path.extension().wstring();
	for (auto& ch : ext)
		ch = static_cast<wchar_t>(std::towlower(ch));
	return ext == L".ttf" || ext == L".ttc" || ext == L".otf" || ext == L".otc" || ext == L".woff" || ext == L".woff2";
}

std::vector<std::wstring> GetWindowsFontDirs() {
	std::vector<std::wstring> dirs;

	wchar_t windows_dir[MAX_PATH] = {};
	if (GetWindowsDirectoryW(windows_dir, MAX_PATH)) {
		std::wstring font_dir = windows_dir;
		font_dir += L"\\Fonts";
		if (DirectoryExists(font_dir))
			dirs.push_back(font_dir);
	}

	auto local_app_data_len = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
	if (local_app_data_len > 1) {
		std::wstring local_app_data(local_app_data_len, L'\0');
		if (GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data.data(), local_app_data_len)) {
			while (!local_app_data.empty() && local_app_data.back() == L'\0')
				local_app_data.pop_back();

			std::wstring user_font_dir = local_app_data + L"\\Microsoft\\Windows\\Fonts";
			if (DirectoryExists(user_font_dir))
				dirs.push_back(user_font_dir);
		}
	}

	return dirs;
}

void AddWindowsFontFiles(FcConfig *config) {
	for (auto const& dir : GetWindowsFontDirs()) {
		std::error_code ec;
		auto const font_dir = std::filesystem::path{dir};
		std::filesystem::recursive_directory_iterator it(
			font_dir,
			std::filesystem::directory_options::skip_permission_denied,
			ec);
		std::filesystem::recursive_directory_iterator end;
		for (; !ec && it != end; it.increment(ec)) {
			std::error_code status_ec;
			if (!it->is_regular_file(status_ec) || status_ec || !IsFontFile(it->path()))
				continue;

			auto path = WideToUtf8(it->path().wstring());
			if (!path.empty())
				FcConfigAppFontAddFile(config, reinterpret_cast<FcChar8 const *>(path.c_str()));
		}
	}
}

FcConfig *CreateFontConfig() {
	auto config = FcConfigCreate();
	if (!config)
		return nullptr;

	AddWindowsFontFiles(config);

	return config;
}
#else
FcConfig *CreateFontConfig() {
	return FcInitLoadConfig();
}
#endif

bool pattern_matches(FcPattern *pat, const char *field, std::string const& name) {
	FcChar8 *str;
	for (int i = 0; FcPatternGetString(pat, field, i, &str) == FcResultMatch; ++i) {
		std::string sstr((char *)str);
		agi::util::strings::to_lower_inplace(sstr);
		if (sstr == name)
			return true;
	}
	return false;
}

void find_font(FcFontSet *src, FcFontSet *dst, std::string const& family) {
	if (!src) return;

	for (int i = 0; i < src->nfont; ++i) {
		FcPattern *pat = src->fonts[i];
		int val;
		if (FcPatternGetBool(pat, FC_OUTLINE, 0, &val) != FcResultMatch || val != FcTrue) continue;

		if (pattern_matches(pat, FC_FULLNAME, family) || pattern_matches(pat, FC_FAMILY, family))
			FcFontSetAdd(dst, FcPatternDuplicate(pat));
	}
}

int NormalizeAssWeight(int bold) {
	return (bold == 1 || bold == -1) ? 700 : bold <= 0 ? 400 : bold;
}

int FontconfigWeightFromOpenType(int weight) {
#if FC_VERSION >= 21292
	return static_cast<int>(std::lround(FcWeightFromOpenTypeDouble(weight)));
#else
	return FcWeightFromOpenType(weight);
#endif
}

int OpenTypeWeightFromFontconfig(FcPattern *pattern) {
	double fc_weight = 0;
	if (FcPatternGetDouble(pattern, FC_WEIGHT, 0, &fc_weight) == FcResultMatch) {
#if FC_VERSION >= 21292
		return static_cast<int>(std::lround(FcWeightToOpenTypeDouble(fc_weight)));
#else
		return FcWeightToOpenType(static_cast<int>(std::lround(fc_weight)));
#endif
	}

	int fc_weight_int = 0;
	if (FcPatternGetInteger(pattern, FC_WEIGHT, 0, &fc_weight_int) == FcResultMatch) {
#if FC_VERSION >= 21292
		return static_cast<int>(std::lround(FcWeightToOpenTypeDouble(fc_weight_int)));
#else
		return FcWeightToOpenType(fc_weight_int);
#endif
	}

	return 400;
}

}

FontConfigFontFileLister::FontConfigFontFileLister(FontCollectorEventSink &cb)
: config(CreateFontConfig(), FcConfigDestroy)
{
	FontCollectorEvent event;
	event.type = FontCollectorEventType::FontBackendInfo;
	event.message = "fontconfig";
	Emit(cb, std::move(event));

	event = FontCollectorEvent();
	event.type = FontCollectorEventType::UpdatingFontCache;
	Emit(cb, std::move(event));
	if (config)
		FcConfigBuildFonts(config);
}

CollectionResult FontConfigFontFileLister::GetFontPaths(std::string const& facename, int bold, bool italic, std::vector<uint32_t> const& characters) {
	CollectionResult ret;
	if (!config)
		return ret;

	std::string family = facename[0] == '@' ? facename.substr(1) : facename;
	agi::util::strings::to_lower_inplace(family);

	int requested_weight = NormalizeAssWeight(bold);
	int weight = FontconfigWeightFromOpenType(requested_weight);
	int slant  = italic ? 110 : 0;
	ret.requested_weight = requested_weight;

	// Create a fontconfig pattern to match the desired weight/slant
	agi::scoped_holder<FcPattern*> pat(FcPatternCreate(), FcPatternDestroy);
	if (!pat) return ret;

	FcPatternAddBool(pat, FC_OUTLINE, true);
	FcPatternAddInteger(pat, FC_SLANT, slant);
	FcPatternAddInteger(pat, FC_WEIGHT, weight);

	FcConfigSetDefaultSubstitute(config, pat);
	if (!FcConfigSubstitute(config, pat, FcMatchPattern)) return ret;

	// Create a font set with only correctly named fonts
	// This is needed because the patterns returned by font matching only
	// include the first family and fullname, so we can't always verify that
	// we got the actual font we were asking for after the fact
	agi::scoped_holder<FcFontSet*> fset(FcFontSetCreate(), FcFontSetDestroy);
	find_font(FcConfigGetFonts(config, FcSetApplication), fset, family);
	find_font(FcConfigGetFonts(config, FcSetSystem), fset, family);

	// Get the best match from fontconfig
	FcResult result;
	FcFontSet *sets[] = { (FcFontSet*)fset };

	agi::scoped_holder<FcFontSet*> matches(FcFontSetSort(config, sets, 1, pat, false, nullptr, &result), FcFontSetDestroy);
	if (matches->nfont == 0)
		return ret;

	auto match = matches->fonts[0];

	FcChar8 *matched_family;
	if (FcPatternGetString(match, FC_FAMILY, 0, &matched_family) == FcResultMatch)
		ret.matched_facename = reinterpret_cast<char const *>(matched_family);
	for (int i = 0; FcPatternGetString(match, FC_FAMILY, i, &matched_family) == FcResultMatch; ++i)
		ret.matched_names.emplace_back(reinterpret_cast<char const *>(matched_family));
	FcPatternGetInteger(match, FC_INDEX, 0, &ret.face_index);
	ret.matched_weight = OpenTypeWeightFromFontconfig(match);
	ret.matched_bold = ret.matched_weight > 550;
	int matched_slant = 0;
	if (FcPatternGetInteger(match, FC_SLANT, 0, &matched_slant) == FcResultMatch)
		ret.matched_italic = matched_slant != FC_SLANT_ROMAN;

	FcChar8 *file;
	if(FcPatternGetString(match, FC_FILE, 0, &file) != FcResultMatch)
		return ret;

	FcCharSet *charset;
	if (FcPatternGetCharSet(match, FC_CHARSET, 0, &charset) == FcResultMatch) {
		for (auto chr : characters) {
			font_collector::unicode::Rune rune;
			if (!font_collector::unicode::Rune::TryCreate(chr, rune))
				continue;
			if (!FcCharSetHasChar(charset, rune.Value()))
				font_collector::unicode::AppendRuneToUtf8(ret.missing, rune);
		}
	}

	ret.fake_bold = requested_weight > ret.matched_weight + 150 && !ret.matched_bold;

	int actual_slant = slant;
	if (FcPatternGetInteger(match, FC_SLANT, 0, &actual_slant) == FcResultMatch)
		ret.fake_italic = italic && !actual_slant;

	ret.paths.emplace_back((const char *)file);
	ret.path_source = "fontconfig";
	return ret;
}
