// Snapshot of the pre-observation Windows catalog builder (from git HEAD at
// extraction). Kept for A/B comparison only; not linked into the product.
//
// Windows FontFamilyCatalog builder. GDI selects the physical face; system
// DirectWrite only supplements that already-selected HDC face with names and
// an entity identity.

#include "legacy/font_family_catalog_win_legacy.h"

#include "gdi_font_resolver.h"

#include <libaegisub/charset_conv_win.h>
#include <libaegisub/log.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

constexpr std::size_t kGdiFaceNameMaxUnits = LF_FACESIZE - 1;

std::string ascii_lower(std::string_view text) {
	std::string result;
	result.reserve(text.size());
	for (unsigned char ch : text)
		result.push_back(static_cast<char>(std::tolower(ch)));
	return result;
}

std::optional<std::wstring> to_utf16(std::string_view value) {
	if (value.empty())
		return std::wstring{};
	try {
		return agi::charset::ConvertW(std::string(value));
	}
	catch (...) {
		return std::nullopt;
	}
}

bool ordinal_icase_equal(std::string_view left, std::string_view right) {
	auto const left_wide = to_utf16(left);
	auto const right_wide = to_utf16(right);
	return left_wide && right_wide &&
		CompareStringOrdinal(
			left_wide->data(), static_cast<int>(left_wide->size()),
			right_wide->data(), static_cast<int>(right_wide->size()), TRUE) == CSTR_EQUAL;
}

bool is_english_locale(std::string const& locale) {
	if (locale.size() < 2)
		return false;
	auto const a = static_cast<char>(std::tolower(static_cast<unsigned char>(locale[0])));
	auto const b = static_cast<char>(std::tolower(static_cast<unsigned char>(locale[1])));
	return a == 'e' && b == 'n' &&
		(locale.size() == 2 || locale[2] == '-' || locale[2] == '_');
}

bool is_en_us(std::string const& locale) {
	auto folded = ascii_lower(locale);
	return folded == "en-us" || folded == "en_us";
}

void add_win32_name(FontFamilyRecord& record, DWriteLocalizedName const& name) {
	if (name.value.empty())
		return;
	auto const duplicate = std::find_if(record.names.begin(), record.names.end(),
		[&](FontFamilyName const& current) {
			return current.kind == FontFamilyNameKind::Win32Family &&
			       ordinal_icase_equal(current.value, name.value) &&
			       current.locale == name.locale;
		});
	if (duplicate != record.names.end())
		return;
	record.names.push_back({name.value, name.locale, FontFamilyNameKind::Win32Family});
}

void add_informational_name(
	FontFamilyRecord& record,
	DWriteLocalizedName const& name,
	FontFamilyNameKind kind,
	FontVariantOutcome const& outcome) {
	if (name.value.empty() || outcome.status != FontVariantStatus::Canonical ||
	    outcome.entity_token == 0 || outcome.role == FontVariantRole::Unknown)
		return;
	auto const duplicate = std::find_if(record.names.begin(), record.names.end(),
		[&](FontFamilyName const& current) {
			return current.kind == kind && current.value == name.value &&
			       current.locale == name.locale &&
			       current.entity_token == outcome.entity_token;
		});
	if (duplicate != record.names.end())
		return;
	record.names.push_back({
		name.value,
		name.locale,
		kind,
		outcome.entity_token,
		outcome.role});
}

bool same_physical_outcome(
	FontVariantOutcome const& left,
	FontVariantOutcome const& right) noexcept {
	// A missing entity makes identity unknowable. In that case duplicate rows
	// are preferable to silently combining two unrelated family topologies.
	return left.entity_token != 0 && right.entity_token != 0 &&
	       left.entity_token == right.entity_token &&
	       left.requested_weight == right.requested_weight &&
	       left.requested_italic == right.requested_italic &&
	       left.realized_weight == right.realized_weight &&
	       left.realized_italic == right.realized_italic &&
	       left.role == right.role && left.status == right.status;
}

bool same_physical_profile(
	FontFamilyVariantProfile const& left,
	FontFamilyVariantProfile const& right) noexcept {
	if (left.backend != right.backend || left.evidence != right.evidence ||
	    left.automatic_pinning_reliable != right.automatic_pinning_reliable)
		return false;
	for (std::size_t index = 0; index < left.outcomes.size(); ++index) {
		if (!same_physical_outcome(left.outcomes[index], right.outcomes[index]))
			return false;
	}
	return true;
}

bool maps_to_profile(
	GdiFontResolver& resolver,
	std::string const& candidate,
	FontFamilyVariantProfile const& expected_profile) {
	if (candidate.empty() ||
	    FontFamilyCatalog::Utf16CodeUnitLength(candidate) > kGdiFaceNameMaxUnits)
		return false;
	for (std::size_t index = 0; index < expected_profile.outcomes.size(); ++index) {
		auto const& expected = expected_profile.outcomes[index];
		auto resolved = resolver.Probe(
			candidate, expected.requested_weight, expected.requested_italic,
			DEFAULT_CHARSET, 0);
		if (!resolved.success || !resolved.matches_requested_family ||
		    !same_physical_outcome(resolved.outcome, expected))
			return false;
	}
	return true;
}

std::string pick_english_name(GdiFontResolver& resolver,
	                          std::vector<DWriteLocalizedName> const& names,
	                          FontFamilyVariantProfile const& expected_profile) {
	std::vector<std::string> en_us;
	std::vector<std::string> other_english;
	for (auto const& name : names) {
		if (name.value.empty() || !is_english_locale(name.locale))
			continue;
		auto& candidates = is_en_us(name.locale) ? en_us : other_english;
		if (std::none_of(
				candidates.begin(), candidates.end(), [&](std::string const& current) {
					return ordinal_icase_equal(current, name.value);
				}))
			candidates.push_back(name.value);
	}

	for (auto const& candidate : en_us) {
		if (maps_to_profile(resolver, candidate, expected_profile))
			return candidate;
	}
	for (auto const& candidate : other_english) {
		if (maps_to_profile(resolver, candidate, expected_profile))
			return candidate;
	}
	return {};
}

std::string normalized_locale(std::string_view locale) {
	auto result = ascii_lower(locale);
	std::replace(result.begin(), result.end(), '_', '-');
	return result;
}

std::string primary_language(std::string_view locale) {
	auto normalized = normalized_locale(locale);
	auto const separator = normalized.find('-');
	if (separator != std::string::npos)
		normalized.resize(separator);
	return normalized;
}

std::string user_default_locale() {
	wchar_t locale[LOCALE_NAME_MAX_LENGTH] = {};
	if (GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH) <= 0)
		return {};
	try {
		return agi::charset::ConvertW(locale);
	}
	catch (...) {
		return {};
	}
}

std::string pick_localized_name(
	GdiFontResolver& resolver,
	FontFamilyRecord const& record,
	std::string_view locale) {
	if (locale.empty())
		return record.localized_family_name;
	auto const language = primary_language(locale);

	// Prefer an exact user-locale Win32 name, then another name in the same
	// language. Every candidate must still pass a live GDI entity check; DWrite
	// metadata alone never makes a name a safe ASS alias.
	for (int rank : {2, 1}) {
		for (auto const& name : record.names) {
			if (name.kind != FontFamilyNameKind::Win32Family || name.locale.empty())
				continue;
			auto const candidate_locale = normalized_locale(name.locale);
			bool const matches = rank == 2
				? candidate_locale == locale
				: !language.empty() && primary_language(candidate_locale) == language;
			if (matches && maps_to_profile(
					resolver, name.value, record.variant_profile))
				return name.value;
		}
	}
	return record.localized_family_name;
}

void merge_record_names(FontFamilyRecord& target, FontFamilyRecord&& source) {
	for (auto& candidate : source.names) {
		auto const duplicate = std::find_if(
			target.names.begin(), target.names.end(), [&](FontFamilyName const& current) {
				return current.value == candidate.value &&
				       current.locale == candidate.locale &&
				       current.kind == candidate.kind &&
				       current.entity_token == candidate.entity_token &&
				       current.variant_role == candidate.variant_role;
			});
		if (duplicate == target.names.end())
			target.names.push_back(std::move(candidate));
	}
}

std::vector<DWriteLocalizedName> win32_names(FontFamilyRecord const& record) {
	std::vector<DWriteLocalizedName> result;
	result.reserve(record.names.size());
	for (auto const& name : record.names) {
		if (name.kind == FontFamilyNameKind::Win32Family)
			result.push_back({name.value, name.locale});
	}
	return result;
}

} // namespace

FontFamilyCatalog BuildFontFamilyCatalogLegacy() {
	auto const build_started = std::chrono::steady_clock::now();
	GdiFontResolver resolver;
	auto seeds = resolver.EnumerateFamilies();
	if (seeds.empty()) {
		LOG_W("font/family_catalog/ab") << "legacy: GDI family enumeration returned no fonts";
		return {};
	}

	std::vector<FontFamilyRecord> records;
	records.reserve(seeds.size());
	auto const locale = normalized_locale(user_default_locale());
	// Usually an ordinary entity has one record. The vector handles the rare
	// case where two logical families share a Regular face but route another
	// canonical request to different physical faces.
	std::unordered_map<std::uint64_t, std::vector<std::size_t>> profile_owners;
	FontFamilyId next_id = 1;

	for (auto const& seed : seeds) {
		FontFamilyRecord record;
		record.id = next_id++;
		record.localized_family_name = seed;
		record.names.push_back({seed, {}, FontFamilyNameKind::Win32Family});
		std::array<FontVariantOutcome, 4> outcomes;
		GdiFontProbeResult seed_probe;
		bool profile_matches_requested_family = true;
		for (std::size_t index = 0; index < outcomes.size(); ++index) {
			bool const italic = index >= 2;
			bool const bold = (index & 1u) != 0;
			auto probe = resolver.ProbeWithSelection(
				seed, bold ? FW_BOLD : FW_NORMAL, italic, DEFAULT_CHARSET, 0,
				[&](GdiFontSelectionView const& selected) {
					if (!selected.probe || !selected.dwrite_face || !selected.dwrite_bridge)
						return;
					for (auto const& name : selected.dwrite_bridge->GetFullNamesFromFace(
						     selected.dwrite_face))
						add_informational_name(
							record, name, FontFamilyNameKind::FullName,
							selected.probe->outcome);
					for (auto const& name : selected.dwrite_bridge->GetPostScriptNamesFromFace(
						     selected.dwrite_face))
						add_informational_name(
							record, name, FontFamilyNameKind::PostScript,
							selected.probe->outcome);
				});
			outcomes[index] = probe.outcome;
			profile_matches_requested_family =
				profile_matches_requested_family && probe.success &&
				probe.matches_requested_family && probe.outcome.entity_token != 0;
			if (index == 0)
				seed_probe = std::move(probe);
		}
		record.variant_profile = BuildFontFamilyVariantProfile(
			std::move(outcomes), FontVariantBackend::VsFilterGdi,
			FontSelectionEvidence::Observed,
			profile_matches_requested_family);

		// Names are read from the IDWriteFontFace created from the currently
		// selected HDC. No LOGFONT re-resolution is allowed in this path.
		if (seed_probe.success && seed_probe.outcome.entity_token != 0) {
			for (auto const& name : seed_probe.win32_family_names)
				add_win32_name(record, name);
		}

		bool merged = false;
		if (profile_matches_requested_family) {
			auto& candidates = profile_owners[seed_probe.outcome.entity_token];
			for (auto const candidate_index : candidates) {
				if (!same_physical_profile(
						records[candidate_index].variant_profile,
						record.variant_profile))
					continue;
				merge_record_names(records[candidate_index], std::move(record));
				merged = true;
				break;
			}
			if (!merged)
				candidates.push_back(records.size());
		}
		if (merged)
			continue;
		records.push_back(std::move(record));
	}

	// Select presentation/write aliases only after physical aliases have been
	// consolidated. This guarantees one catalog row per proven GDI family while
	// retaining every independently validated Win32 spelling on that row.
	std::vector<std::pair<std::string, std::vector<FontFamilyId>>> english_owners;
	for (auto& record : records) {
		if (record.variant_profile.For(false, false).entity_token == 0)
			continue;
		record.localized_family_name = pick_localized_name(resolver, record, locale);
		record.english_win32_family_name = pick_english_name(
			resolver, win32_names(record), record.variant_profile);
		if (record.english_win32_family_name.empty())
			continue;
		auto owner = std::find_if(
			english_owners.begin(), english_owners.end(), [&](auto const& entry) {
				return ordinal_icase_equal(entry.first, record.english_win32_family_name);
			});
		if (owner == english_owners.end())
			english_owners.push_back({record.english_win32_family_name, {record.id}});
		else
			owner->second.push_back(record.id);
	}

	// The same English spelling claimed by more than one GDI family is not a
	// portable ASS alias even if both claims were individually resolvable.
	std::unordered_set<FontFamilyId> ambiguous_english;
	for (auto const& [name, owners] : english_owners) {
		(void)name;
		if (owners.size() > 1)
			ambiguous_english.insert(owners.begin(), owners.end());
	}
	for (auto& record : records) {
		if (ambiguous_english.contains(record.id))
			record.english_win32_family_name.clear();
	}

	auto const elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - build_started);
	auto const stats = resolver.stats();
	LOG_I("font/family_catalog/ab") << "legacy: Built Windows font family catalog with "
	                             << records.size() << " families in "
	                             << elapsed.count() << " ms ("
	                             << stats.physical_probe_count << " physical probes, "
	                             << stats.memo_hit_count << " memo hits, "
	                             << stats.fingerprint_read_count << " fingerprint reads)";
	return FontFamilyCatalog(std::move(records));
}
