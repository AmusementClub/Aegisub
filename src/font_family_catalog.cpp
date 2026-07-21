#include "font_family_catalog.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <optional>
#include <unordered_set>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

#if defined(_WIN32)

std::optional<std::wstring> to_utf16(std::string_view value) {
	if (value.empty())
		return std::wstring{};
	if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
		return std::nullopt;
	auto const input_size = static_cast<int>(value.size());
	auto const output_size = MultiByteToWideChar(
		CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), input_size, nullptr, 0);
	if (output_size <= 0)
		return std::nullopt;
	std::wstring output(static_cast<std::size_t>(output_size), L'\0');
	if (MultiByteToWideChar(
			CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), input_size,
			output.data(), output_size) != output_size)
		return std::nullopt;
	return output;
}

int ordinal_icase_compare(std::wstring_view left, std::wstring_view right) noexcept {
	auto const result = CompareStringOrdinal(
		left.data(), static_cast<int>(left.size()),
		right.data(), static_cast<int>(right.size()), TRUE);
	if (result == CSTR_LESS_THAN)
		return -1;
	if (result == CSTR_GREATER_THAN)
		return 1;
	if (result == CSTR_EQUAL)
		return 0;
	// Inputs originate from GDI/DWrite family names and are tiny valid UTF-16
	// strings. Preserve a strict order defensively if the OS call ever fails.
	return left.compare(right);
}

#else

std::string ascii_lower(std::string_view s) {
	std::string out;
	out.reserve(s.size());
	for (unsigned char ch : s)
		out.push_back(static_cast<char>(std::tolower(ch)));
	return out;
}

std::string fold_key(std::string_view s) {
	return ascii_lower(s);
}

#endif

} // namespace

FontVariantOutcome const& FontFamilyVariantProfile::For(bool bold, bool italic) const noexcept {
	return outcomes[(italic ? 2u : 0u) + (bold ? 1u : 0u)];
}

FontFamilyVariantProfile BuildFontFamilyVariantProfile(
	std::array<FontVariantOutcome, 4> outcomes,
	FontVariantBackend backend,
	FontSelectionEvidence evidence,
	bool automatic_pinning_reliable) {
	FontFamilyVariantProfile profile;
	profile.outcomes = std::move(outcomes);
	profile.backend = backend;
	profile.evidence = evidence;
	profile.automatic_pinning_reliable = automatic_pinning_reliable;
	return profile;
}

FontFamilyCatalog::FontFamilyCatalog(std::vector<FontFamilyRecord> records)
: records_(std::move(records))
{
	BuildIndex();
}

void FontFamilyCatalog::BuildIndex() {
	alias_index_.clear();
	exact_index_.clear();
	informational_index_.clear();
	id_index_.clear();
	id_index_.reserve(records_.size());
	for (std::size_t index = 0; index < records_.size(); ++index) {
		auto const& rec = records_[index];
		id_index_.emplace(rec.id, index);
		bool const localized_is_safe_alias = !std::any_of(
			rec.names.begin(), rec.names.end(), [&](FontFamilyName const& name) {
				return name.kind == FontFamilyNameKind::PlatformAlias &&
				       name.value == rec.localized_family_name;
			});
		auto add_alias = [&](std::string const& value) {
			if (value.empty())
				return;
			exact_index_[value].push_back(rec.id);
#if defined(_WIN32)
			if (auto wide = to_utf16(value))
				alias_index_.push_back({std::move(*wide), {rec.id}});
#else
			alias_index_[fold_key(value)].push_back(rec.id);
#endif
		};

		if (localized_is_safe_alias)
			add_alias(rec.localized_family_name);
		add_alias(rec.english_win32_family_name);
		// Only Win32 family names are safe ASS family aliases. Other name
		// records are informational metadata for diagnostics and UI display;
		// indexing them here would make Full/PostScript/typographic names
		// eligible for automatic normalization.
		for (auto const& n : rec.names) {
			if (n.kind == FontFamilyNameKind::Win32Family)
				add_alias(n.value);
		}

		// Also index bare names without '@' so vertical aliases resolve.
		auto bare_local = SplitVerticalPrefix(rec.localized_family_name).second;
		if (bare_local != rec.localized_family_name)
			add_alias(bare_local);
		auto bare_en = SplitVerticalPrefix(rec.english_win32_family_name).second;
		if (!bare_en.empty() && bare_en != rec.english_win32_family_name)
			add_alias(bare_en);
	}

	// Deduplicate id lists while preserving order.
	auto unique_ids = [](std::vector<FontFamilyId>& ids) {
		std::vector<FontFamilyId> out;
		out.reserve(ids.size());
		for (auto id : ids) {
			if (std::find(out.begin(), out.end(), id) == out.end())
				out.push_back(id);
		}
		ids.swap(out);
	};
	for (auto& kv : exact_index_)
		unique_ids(kv.second);

	std::unordered_map<std::string, FontFamilyResolution> informational_candidates;
	std::unordered_map<std::string, std::uint64_t> informational_entities;
	std::unordered_set<std::string> ambiguous_informational;
	for (auto const& record : records_) {
		for (auto const& metadata : record.names) {
			if (metadata.kind != FontFamilyNameKind::FullName &&
			    metadata.kind != FontFamilyNameKind::PostScript)
				continue;
			std::string key;
#if defined(_WIN32)
			if (auto wide = to_utf16(metadata.value)) {
				auto const required = LCMapStringEx(
					LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE,
					wide->data(), static_cast<int>(wide->size()), nullptr, 0,
					nullptr, nullptr, 0);
				if (required == static_cast<int>(wide->size())) {
					std::wstring folded(wide->size(), L'\0');
					if (LCMapStringEx(
							LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE,
							wide->data(), static_cast<int>(wide->size()),
							folded.data(), required, nullptr, nullptr, 0) == required)
						key.assign(reinterpret_cast<char const*>(folded.data()),
						           folded.size() * sizeof(wchar_t));
				}
			}
#else
			key = fold_key(metadata.value);
#endif
			if (key.empty())
				continue;
			FontFamilyResolution candidate;
			candidate.match = FontFamilyMatchKind::Exact;
			candidate.family = record.id;
			candidate.variant_role = metadata.variant_role;
			auto [it, inserted] = informational_candidates.emplace(key, candidate);
			if (inserted)
				informational_entities.emplace(key, metadata.entity_token);
			else if (
			    (it->second.family != candidate.family ||
			     it->second.variant_role != candidate.variant_role ||
			     informational_entities[key] != metadata.entity_token ||
			     metadata.entity_token == 0))
				ambiguous_informational.insert(key);
		}
	}
	for (auto& [key, candidate] : informational_candidates) {
		if (ambiguous_informational.contains(key)) {
			candidate.match = FontFamilyMatchKind::Ambiguous;
			candidate.family.reset();
			candidate.variant_role = FontVariantRole::Unknown;
		}
		informational_index_.emplace(std::move(key), std::move(candidate));
	}
#if defined(_WIN32)
	std::sort(alias_index_.begin(), alias_index_.end(), [](auto const& left, auto const& right) {
		return ordinal_icase_compare(left.name, right.name) < 0;
	});
	std::vector<CaseInsensitiveAliasEntry> merged_aliases;
	merged_aliases.reserve(alias_index_.size());
	for (auto& entry : alias_index_) {
		if (!merged_aliases.empty() &&
		    ordinal_icase_compare(merged_aliases.back().name, entry.name) == 0) {
			merged_aliases.back().families.insert(
				merged_aliases.back().families.end(),
				entry.families.begin(), entry.families.end());
		}
		else {
			merged_aliases.push_back(std::move(entry));
		}
	}
	alias_index_ = std::move(merged_aliases);
	for (auto& entry : alias_index_)
		unique_ids(entry.families);
#else
	for (auto& kv : alias_index_)
		unique_ids(kv.second);
#endif
}

FontFamilyRecord const* FontFamilyCatalog::Find(FontFamilyId id) const {
	auto const it = id_index_.find(id);
	return it == id_index_.end() ? nullptr : &records_[it->second];
}

FontFamilyResolution FontFamilyCatalog::Resolve(std::string_view name) const {
	FontFamilyResolution result;
	if (name.empty())
		return result;

	auto [vertical_prefix, bare] = SplitVerticalPrefix(name);
	bool const vertical = !vertical_prefix.empty();
	std::string const bare_str(bare);
	std::string const full_str(name);

	auto finalize = [&](FontFamilyId id, FontFamilyMatchKind kind) {
		auto const* rec = Find(id);
		if (!rec)
			return;
		result.match = kind;
		result.family = id;
		std::string canonical = rec->localized_family_name;
		if (!rec->english_win32_family_name.empty())
			canonical = rec->english_win32_family_name;
		// Preserve vertical prefix on the canonical form of the same family.
		auto bare_can = SplitVerticalPrefix(canonical).second;
		result.canonical_name = JoinVerticalPrefix(vertical, bare_can);
	};

	auto pick = [&](std::vector<FontFamilyId> const& ids, FontFamilyMatchKind kind) -> bool {
		if (ids.empty())
			return false;
		if (ids.size() > 1) {
			result.match = FontFamilyMatchKind::Ambiguous;
			return true;
		}
		finalize(ids.front(), kind);
		return true;
	};

	// Prefer exact spelling matches (full name first, then bare without @).
	if (auto it = exact_index_.find(full_str); it != exact_index_.end()) {
		if (pick(it->second, FontFamilyMatchKind::Exact))
			return result;
	}
	if (vertical) {
		if (auto it = exact_index_.find(bare_str); it != exact_index_.end()) {
			if (pick(it->second, FontFamilyMatchKind::Exact))
				return result;
		}
	}

	// Case-insensitive fallback.
	auto pick_case_insensitive = [&](std::string const& value) {
#if defined(_WIN32)
		auto const wide = to_utf16(value);
		if (!wide)
			return false;
		auto const it = std::lower_bound(
			alias_index_.begin(), alias_index_.end(), *wide,
			[](CaseInsensitiveAliasEntry const& entry, std::wstring const& key) {
				return ordinal_icase_compare(entry.name, key) < 0;
			});
		return it != alias_index_.end() && ordinal_icase_compare(it->name, *wide) == 0
			? pick(it->families, FontFamilyMatchKind::CaseInsensitiveExact)
			: false;
#else
		auto const it = alias_index_.find(fold_key(value));
		return it != alias_index_.end()
			? pick(it->second, FontFamilyMatchKind::CaseInsensitiveExact)
			: false;
#endif
	};
	if (pick_case_insensitive(full_str))
		return result;
	if (vertical) {
		if (pick_case_insensitive(bare_str))
			return result;
	}

	return result;
}

FontFamilyResolution FontFamilyCatalog::ResolveInformationalName(std::string_view name) const {
	FontFamilyResolution result;
	if (name.empty())
		return result;
	auto const [vertical, bare_name] = SplitVerticalPrefix(name);
	std::string key;
#if defined(_WIN32)
	if (auto wide = to_utf16(bare_name)) {
		auto const required = LCMapStringEx(
			LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE,
			wide->data(), static_cast<int>(wide->size()), nullptr, 0,
			nullptr, nullptr, 0);
		if (required == static_cast<int>(wide->size())) {
			std::wstring folded(wide->size(), L'\0');
			if (LCMapStringEx(
					LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE,
					wide->data(), static_cast<int>(wide->size()),
					folded.data(), required, nullptr, nullptr, 0) == required)
				key.assign(reinterpret_cast<char const*>(folded.data()),
				           folded.size() * sizeof(wchar_t));
		}
	}
#else
	key = fold_key(bare_name);
#endif
	if (key.empty())
		return result;
	auto const it = informational_index_.find(key);
	if (it == informational_index_.end())
		return result;
	result = it->second;
	result.canonical_name = JoinVerticalPrefix(!vertical.empty(), bare_name);
	return result;
}

std::string FontFamilyCatalog::PreferredWriteName(FontFamilyRecord const& record, bool prefer_localized) const {
	auto [vprefix, bare_local] = SplitVerticalPrefix(record.localized_family_name);
	bool const vertical = !vprefix.empty();

	if (prefer_localized || record.english_win32_family_name.empty())
		return record.localized_family_name;

	auto bare_en = SplitVerticalPrefix(record.english_win32_family_name).second;
	if (bare_en.empty())
		return record.localized_family_name;
	return JoinVerticalPrefix(vertical, bare_en);
}

std::string FontFamilyCatalog::PreferredWriteName(FontFamilyId id, bool prefer_localized) const {
	auto const* rec = Find(id);
	if (!rec)
		return {};
	return PreferredWriteName(*rec, prefer_localized);
}

std::string FontFamilyCatalog::MapToPreferredWriteName(std::string_view name, bool prefer_localized) const {
	auto resolved = Resolve(name);
	if (resolved.match != FontFamilyMatchKind::Exact &&
	    resolved.match != FontFamilyMatchKind::CaseInsensitiveExact)
		return std::string(name);
	if (!resolved.family)
		return std::string(name);

	// Catalog records store bare family names (no '@'); preserve the caller's
	// vertical prefix so "\@微软雅黑" maps to "\@Microsoft YaHei".
	bool const vertical = !SplitVerticalPrefix(name).first.empty();
	auto preferred = PreferredWriteName(*resolved.family, prefer_localized);
	auto bare = SplitVerticalPrefix(preferred).second;
	auto mapped = JoinVerticalPrefix(vertical, bare);

	// GDI/VSFilter face names are limited to 31 UTF-16 code units including a
	// leading '@'. If the remapped form would overflow, keep the original.
	// LF_FACESIZE is 32 on Windows; use the same numeric limit portably.
	constexpr std::size_t kGdiFaceNameMaxUnits = 31;
	if (Utf16CodeUnitLength(mapped) > kGdiFaceNameMaxUnits)
		return std::string(name);
	return mapped;
}

std::pair<std::string, std::string> FontFamilyCatalog::SplitVerticalPrefix(std::string_view name) {
	if (!name.empty() && name.front() == '@')
		return {std::string("@"), std::string(name.substr(1))};
	return {std::string(), std::string(name)};
}

std::string FontFamilyCatalog::JoinVerticalPrefix(bool vertical, std::string_view bare_name) {
	if (!vertical)
		return std::string(bare_name);
	std::string out;
	out.reserve(bare_name.size() + 1);
	out.push_back('@');
	out.append(bare_name);
	return out;
}

std::size_t FontFamilyCatalog::Utf16CodeUnitLength(std::string_view utf8) {
	// Count UTF-16 code units without allocating (surrogate pairs count as 2).
	std::size_t units = 0;
	for (std::size_t i = 0; i < utf8.size();) {
		unsigned char c = static_cast<unsigned char>(utf8[i]);
		std::uint32_t cp = 0;
		std::size_t len = 0;
		if (c < 0x80) {
			cp = c;
			len = 1;
		} else if ((c >> 5) == 0x6 && i + 1 < utf8.size()) {
			cp = (c & 0x1F) << 6 | (static_cast<unsigned char>(utf8[i + 1]) & 0x3F);
			len = 2;
		} else if ((c >> 4) == 0xE && i + 2 < utf8.size()) {
			cp = (c & 0x0F) << 12
			   | (static_cast<unsigned char>(utf8[i + 1]) & 0x3F) << 6
			   | (static_cast<unsigned char>(utf8[i + 2]) & 0x3F);
			len = 3;
		} else if ((c >> 3) == 0x1E && i + 3 < utf8.size()) {
			cp = (c & 0x07) << 18
			   | (static_cast<unsigned char>(utf8[i + 1]) & 0x3F) << 12
			   | (static_cast<unsigned char>(utf8[i + 2]) & 0x3F) << 6
			   | (static_cast<unsigned char>(utf8[i + 3]) & 0x3F);
			len = 4;
		} else {
			// Invalid sequence: treat as one unit and advance one byte.
			++units;
			++i;
			continue;
		}
		i += len;
		units += (cp > 0xFFFF) ? 2 : 1;
	}
	return units;
}

#if !defined(_WIN32)
FontFamilyCatalog BuildFontFamilyCatalog() {
	// Non-Windows keeps the existing platform/libass behavior until a native
	// variant provider can supply outcomes with equivalent confidence.
	return FontFamilyCatalog{};
}
#endif
