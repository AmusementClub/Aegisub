#include "font_family_catalog.h"

#include <algorithm>
#include <cctype>

namespace {

std::string ascii_lower(std::string_view s) {
	std::string out;
	out.reserve(s.size());
	for (unsigned char ch : s)
		out.push_back(static_cast<char>(std::tolower(ch)));
	return out;
}

std::string fold_key(std::string_view s) {
	// Case-fold key for alias lookup. ASCII lower is enough for Win32 English
	// names; CJK aliases are compared via exact index or platform builder
	// registration of both original and folded forms.
	return ascii_lower(s);
}

} // namespace

FontFamilyCatalog::FontFamilyCatalog(std::vector<FontFamilyRecord> records)
: records_(std::move(records))
{
	BuildIndex();
}

void FontFamilyCatalog::BuildIndex() {
	alias_index_.clear();
	exact_index_.clear();
	for (auto const& rec : records_) {
		auto add_alias = [&](std::string const& value) {
			if (value.empty())
				return;
			exact_index_[value].push_back(rec.id);
			alias_index_[fold_key(value)].push_back(rec.id);
		};

		add_alias(rec.localized_family_name);
		add_alias(rec.english_win32_family_name);
		for (auto const& n : rec.names)
			add_alias(n.value);

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
	for (auto& kv : alias_index_)
		unique_ids(kv.second);
}

FontFamilyRecord const* FontFamilyCatalog::Find(FontFamilyId id) const {
	for (auto const& rec : records_) {
		if (rec.id == id)
			return &rec;
	}
	return nullptr;
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
	if (auto it = alias_index_.find(fold_key(full_str)); it != alias_index_.end()) {
		if (pick(it->second, FontFamilyMatchKind::CaseInsensitiveExact))
			return result;
	}
	if (vertical) {
		if (auto it = alias_index_.find(fold_key(bare_str)); it != alias_index_.end()) {
			if (pick(it->second, FontFamilyMatchKind::CaseInsensitiveExact))
				return result;
		}
	}

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

std::vector<std::string> FontFamilyCatalog::DisplayNames(bool prefer_localized) const {
	std::vector<std::string> names;
	names.reserve(records_.size());
	for (auto const& rec : records_)
		names.push_back(PreferredWriteName(rec, prefer_localized));

	std::sort(names.begin(), names.end());
	names.erase(std::unique(names.begin(), names.end()), names.end());
	return names;
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
	// Non-Windows catalog builders land in later phases.
	return FontFamilyCatalog{};
}
#endif
