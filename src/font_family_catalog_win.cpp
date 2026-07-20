// Windows FontFamilyCatalog builder:
// 1. Enumerate GDI families (same selectable set as style editor default path)
// 2. Read DirectWrite Win32 family names with locales
// 3. Pick English Win32 name only when it GDI-resolves to the same font entity
//    (file path + face index) and is unique across the catalog

#include "font_family_catalog.h"
#include "font_family_catalog_win_detail.h"
#include "font_file_lister_dwrite.h"

#include <libaegisub/charset_conv_win.h>
#include <libaegisub/log.h>
#include <libaegisub/scope_exit.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwrite.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

// GDI lfFaceName limit in UTF-16 code units (excluding trailing NUL).
constexpr std::size_t kGdiFaceNameMaxUnits = LF_FACESIZE - 1;

std::string ascii_lower(std::string const& s) {
	std::string out;
	out.reserve(s.size());
	for (unsigned char ch : s)
		out.push_back(static_cast<char>(std::tolower(ch)));
	return out;
}

bool is_english_locale(std::string const& locale) {
	if (locale.size() < 2)
		return false;
	auto a = static_cast<char>(std::tolower(static_cast<unsigned char>(locale[0])));
	auto b = static_cast<char>(std::tolower(static_cast<unsigned char>(locale[1])));
	if (a != 'e' || b != 'n')
		return false;
	if (locale.size() == 2)
		return true;
	return locale[2] == '-' || locale[2] == '_';
}

bool is_en_us(std::string const& locale) {
	return ascii_lower(locale) == "en-us" || ascii_lower(locale) == "en_us";
}

struct EnumState {
	std::vector<std::string> faces;
	std::unordered_set<std::string> seen;
};

int CALLBACK enum_font_families_proc(LOGFONTW const* lplf, TEXTMETRICW const*, DWORD, LPARAM lParam) {
	auto* state = reinterpret_cast<EnumState*>(lParam);
	if (!lplf || !state)
		return 1;

	// Skip vertical '@' duplicates from GDI; catalog handles '@' as a prefix.
	if (lplf->lfFaceName[0] == L'@')
		return 1;
	if (!lplf->lfFaceName[0])
		return 1;

	std::string utf8 = agi::charset::ConvertW(lplf->lfFaceName);
	if (utf8.empty() || !state->seen.insert(utf8).second)
		return 1;
	state->faces.push_back(std::move(utf8));
	return 1;
}

std::vector<std::string> enumerate_gdi_families() {
	EnumState state;
	HDC hdc = CreateCompatibleDC(nullptr);
	if (!hdc)
		return state.faces;
	auto release_dc = agi::make_scope_exit([&] { DeleteDC(hdc); });

	LOGFONTW lf{};
	lf.lfCharSet = DEFAULT_CHARSET;
	lf.lfFaceName[0] = L'\0';
	lf.lfPitchAndFamily = 0;
	EnumFontFamiliesExW(hdc, &lf, enum_font_families_proc, reinterpret_cast<LPARAM>(&state), 0);
	return state.faces;
}

/// Create a GDI font for facename and return the face actually selected.
bool gdi_select_face(std::string const& facename, std::string& out_selected, LOGFONTW& out_lf) {
	out_selected.clear();
	out_lf = {};
	out_lf.lfCharSet = DEFAULT_CHARSET;
	out_lf.lfOutPrecision = OUT_TT_PRECIS;
	out_lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
	out_lf.lfQuality = DEFAULT_QUALITY;
	out_lf.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
	out_lf.lfWeight = FW_NORMAL;

	// Reject names that already exceed the GDI face buffer (bare or with @).
	if (FontFamilyCatalog::Utf16CodeUnitLength(facename) > kGdiFaceNameMaxUnits)
		return false;

	auto wide = agi::charset::ConvertW(facename);
	if (wide.empty() || wide.size() >= LF_FACESIZE)
		return false;
	wcsncpy(out_lf.lfFaceName, wide.c_str(), LF_FACESIZE - 1);
	out_lf.lfFaceName[LF_FACESIZE - 1] = L'\0';

	HDC hdc = CreateCompatibleDC(nullptr);
	if (!hdc)
		return false;
	auto release_dc = agi::make_scope_exit([&] { DeleteDC(hdc); });

	HFONT hfont = CreateFontIndirectW(&out_lf);
	if (!hfont)
		return false;
	auto release_font = agi::make_scope_exit([&] {
		SelectObject(hdc, nullptr);
		DeleteObject(hfont);
	});
	SelectObject(hdc, hfont);

	wchar_t selected[LF_FACESIZE] = {};
	if (!GetTextFaceW(hdc, LF_FACESIZE, selected) || !selected[0])
		return false;

	// Refresh LOGFONT from the actual selected object (metrics may differ).
	GetObjectW(hfont, sizeof(LOGFONTW), &out_lf);
	out_selected = agi::charset::ConvertW(selected);
	return !out_selected.empty();
}

using font_family_catalog_win_detail::FontEntityKey;
using font_family_catalog_win_detail::SameEntity;

bool resolve_entity(DWriteBridge const& dwrite, std::string const& facename, FontEntityKey& out) {
	out = {};
	if (!dwrite.available())
		return false;

	std::string selected;
	LOGFONTW lf{};
	if (!gdi_select_face(facename, selected, lf))
		return false;

	IDWriteFontFace *face = dwrite.CreateFontFaceFromLogFont(lf);
	if (!face)
		return false;
	auto release_face = agi::make_scope_exit([&] { face->Release(); });

	std::string path;
	int index = -1;
	if (!dwrite.GetFontFilePath(face, path, index) || path.empty())
		return false;

	out.path_lower = ascii_lower(path);
	out.face_index = index >= 0 ? index : static_cast<int>(face->GetIndex());
	out.valid = true;
	return true;
}

bool resolve_entity_via_hdc(DWriteBridge const& dwrite,
                            LOGFONTW const& lf,
                            FontEntityKey& out,
                            std::vector<DWriteLocalizedName>* out_names = nullptr) {
	out = {};
	std::string path;
	int index = -1;
	std::vector<DWriteLocalizedName> names;
	if (!dwrite.ResolveEntityAndNamesViaHdc(lf, path, index, names) || path.empty())
		return false;

	out.path_lower = ascii_lower(path);
	out.face_index = index;
	out.valid = index >= 0;
	if (!out.valid)
		return false;

	if (out_names)
		*out_names = std::move(names);
	return true;
}

enum class SeedResolutionPath {
	LogFont,
	Hdc
};

/// True if GDI+DWrite resolution of `candidate` lands on the same font entity
/// as `localized_entity` (already resolved for the seed family).
bool candidate_maps_to_entity(DWriteBridge const& dwrite,
                              std::string const& candidate,
                              FontEntityKey const& localized_entity) {
	if (candidate.empty() || !localized_entity.valid)
		return false;
	// Bare English name must fit GDI; vertical '@' form is re-checked at map time.
	if (FontFamilyCatalog::Utf16CodeUnitLength(candidate) > kGdiFaceNameMaxUnits)
		return false;

	FontEntityKey candidate_entity;
	if (!resolve_entity(dwrite, candidate, candidate_entity))
		return false;
	return SameEntity(candidate_entity, localized_entity);
}

std::string pick_english_name(DWriteBridge const& dwrite,
                              std::vector<DWriteLocalizedName> const& names,
                              FontEntityKey const& localized_entity) {
	// Priority: en-US, other en-*, then empty.
	std::string en_us;
	std::string en_other;
	for (auto const& n : names) {
		if (n.value.empty())
			continue;
		if (!is_english_locale(n.locale))
			continue;
		if (is_en_us(n.locale)) {
			if (en_us.empty())
				en_us = n.value;
		} else if (en_other.empty()) {
			en_other = n.value;
		}
	}

	auto try_name = [&](std::string const& candidate) -> std::string {
		if (candidate.empty())
			return {};
		if (!candidate_maps_to_entity(dwrite, candidate, localized_entity))
			return {};
		return candidate;
	};

	if (auto s = try_name(en_us); !s.empty())
		return s;
	if (auto s = try_name(en_other); !s.empty())
		return s;
	return {};
}

} // namespace

FontFamilyCatalog BuildFontFamilyCatalog() {
	auto seeds = enumerate_gdi_families();
	if (seeds.empty()) {
		LOG_W("font/family_catalog") << "GDI family enumeration returned no fonts";
		return FontFamilyCatalog{};
	}

	DWriteBridge dwrite;
	std::vector<FontFamilyRecord> records;
	records.reserve(seeds.size());
	FontFamilyId next_id = 1;

	// Track english names claimed by a family so we can mark collisions later.
	std::unordered_map<std::string, std::vector<FontFamilyId>> english_owners;

	// Remember how each seed was resolved so the final safety pass can repeat
	// the same live lookup instead of trusting a stale entity snapshot.
	std::unordered_map<std::string, SeedResolutionPath> seed_resolution_paths;

	for (auto const& seed_face : seeds) {
		FontFamilyRecord rec;
		rec.id = next_id++;
		rec.localized_family_name = seed_face;

		// Always register the GDI face as a Win32 family alias.
		{
			FontFamilyName n;
			n.value = seed_face;
			n.locale = {};
			n.kind = FontFamilyNameKind::Win32Family;
			rec.names.push_back(std::move(n));
		}

		std::string selected;
		LOGFONTW lf{};
		if (!gdi_select_face(seed_face, selected, lf) || !dwrite.available()) {
			// Without DWrite we cannot entity-validate English aliases safely.
			records.push_back(std::move(rec));
			continue;
		}

		auto win32_names = dwrite.GetWin32FamilyNamesFromLogFont(lf);
		for (auto const& wn : win32_names) {
			if (wn.value.empty())
				continue;
			// Avoid duplicating the seed face string with empty locale.
			bool exists = false;
			for (auto const& existing : rec.names) {
				if (existing.value == wn.value && existing.locale == wn.locale) {
					exists = true;
					break;
				}
			}
			if (exists)
				continue;
			FontFamilyName n;
			n.value = wn.value;
			n.locale = wn.locale;
			n.kind = FontFamilyNameKind::Win32Family;
			rec.names.push_back(std::move(n));
		}

		FontEntityKey seed_entity;
		bool have_seed = resolve_entity(dwrite, seed_face, seed_entity);
		auto seed_resolution_path = SeedResolutionPath::LogFont;

		// Fallback when DWrite rejects the LOGFONT (typical for fonts whose
		// zh-CN name table lacks name ID 2 — Subfamily — so DWrite cannot build
		// a RBIZ family entry and returns DWRITE_E_NOFONT). Recover the seed
		// entity and English Win32 family names via the HDC path, which reads
		// the currently-selected HFONT directly, bypassing the locale lookup.
		//
		// NOTE: DWriteCore returns E_NOTIMPL from CreateFontFaceFromHdc, so
		// this fallback is skipped there. Fonts with incomplete zh-CN name
		// tables will fall back to the localized family name on DWriteCore.
		// Fixing that requires reading the name table directly (bypassing
		// DWrite entirely) — out of scope for this change.
		if (!have_seed && !dwrite.is_dwritecore()) {
			std::vector<DWriteLocalizedName> hd_names;
			if (resolve_entity_via_hdc(dwrite, lf, seed_entity, &hd_names)) {
				have_seed = true;
				seed_resolution_path = SeedResolutionPath::Hdc;

				// Merge recovered Win32 family names into rec.names and the
				// local win32_names list (deduplicated) so pick_english_name
				// can see them.
				for (auto const& wn : hd_names) {
					if (wn.value.empty()) continue;
					bool exists = false;
					for (auto const& existing : rec.names) {
						if (existing.value == wn.value && existing.locale == wn.locale) {
							exists = true;
							break;
						}
					}
					if (!exists) {
						FontFamilyName n;
						n.value = wn.value;
						n.locale = wn.locale;
						n.kind = FontFamilyNameKind::Win32Family;
						rec.names.push_back(std::move(n));
						win32_names.push_back(wn);
					}
				}
			}
		}

		if (have_seed)
			seed_resolution_paths.emplace(seed_face, seed_resolution_path);

		if (have_seed) {
			rec.english_win32_family_name = pick_english_name(dwrite, win32_names, seed_entity);
			if (!rec.english_win32_family_name.empty())
				english_owners[ascii_lower(rec.english_win32_family_name)].push_back(rec.id);
		}

		// If no English candidate, leave english_win32_family_name empty so
		// PreferredWriteName falls back to localized.
		records.push_back(std::move(rec));
	}

	// Drop English names that map to multiple families (unresolvable ambiguity).
	// String-level ownership is not enough by itself for entity safety, but it
	// still catches two families publishing the same portable write name.
	std::unordered_set<FontFamilyId> clear_english;
	for (auto const& kv : english_owners) {
		if (kv.second.size() > 1) {
			for (auto id : kv.second)
				clear_english.insert(id);
		}
	}
	if (!clear_english.empty()) {
		for (auto& rec : records) {
			if (clear_english.count(rec.id))
				rec.english_win32_family_name.clear();
		}
		LOG_D("font/family_catalog") << "Cleared ambiguous English Win32 family names for "
		                             << clear_english.size() << " families";
	}

	// Final safety pass: re-resolve both names so a font install, removal, or
	// substitution during catalog construction cannot publish a stale alias.
	// Seeds that required the HDC fallback must use that path again because
	// CreateFontFromLOGFONT is known to reject them.
	if (dwrite.available()) {
		for (auto& rec : records) {
			if (rec.english_win32_family_name.empty())
				continue;

			FontEntityKey seed_entity;
			bool seed_ok = false;
			auto resolution_path = seed_resolution_paths.find(rec.localized_family_name);
			if (resolution_path != seed_resolution_paths.end()) {
				if (resolution_path->second == SeedResolutionPath::LogFont) {
					seed_ok = resolve_entity(dwrite, rec.localized_family_name, seed_entity);
				}
				else {
					std::string selected;
					LOGFONTW lf{};
					seed_ok = gdi_select_face(rec.localized_family_name, selected, lf)
					       && resolve_entity_via_hdc(dwrite, lf, seed_entity);
				}
			}

			// The English candidate is an ASCII family name; CreateFontFromLOGFONT
			// typically accepts it without needing the HDC fallback.
			FontEntityKey eng_entity;
			bool eng_ok = resolve_entity(dwrite, rec.english_win32_family_name, eng_entity);

			if (!seed_ok || !eng_ok || !SameEntity(seed_entity, eng_entity))
				rec.english_win32_family_name.clear();
		}
	}

	LOG_I("font/family_catalog") << "Built Windows font family catalog with " << records.size()
	                             << " families (DWrite "
	                             << (dwrite.available() ? "available" : "unavailable") << ")";
	return FontFamilyCatalog(std::move(records));
}
