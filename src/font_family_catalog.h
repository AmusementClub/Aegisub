#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

enum class FontFamilyNameKind {
	Win32Family,
	TypographicFamily,
	FullName,
	PostScript,
	PlatformAlias
};

struct FontFamilyName {
	std::string value;
	std::string locale;
	FontFamilyNameKind kind = FontFamilyNameKind::Win32Family;
};

using FontFamilyId = std::uint32_t;

struct FontFamilyRecord {
	FontFamilyId id = 0;
	std::string localized_family_name;
	std::string english_win32_family_name;
	std::vector<FontFamilyName> names;
};

enum class FontFamilyMatchKind {
	None,
	Exact,
	CaseInsensitiveExact,
	Ambiguous
};

struct FontFamilyResolution {
	FontFamilyMatchKind match = FontFamilyMatchKind::None;
	std::optional<FontFamilyId> family;
	std::string canonical_name;
};

/// Immutable snapshot of installed font families and their legal aliases.
class FontFamilyCatalog {
public:
	FontFamilyCatalog() = default;
	explicit FontFamilyCatalog(std::vector<FontFamilyRecord> records);

	bool empty() const { return records_.empty(); }
	std::size_t size() const { return records_.size(); }

	std::vector<FontFamilyRecord> const& records() const { return records_; }
	FontFamilyRecord const* Find(FontFamilyId id) const;

	/// Exact or case-insensitive alias resolution. Returns Ambiguous when more
	/// than one family claims the same alias under the chosen match class.
	FontFamilyResolution Resolve(std::string_view name) const;

	/// Preferred ASS write name for a family under the given preference.
	/// Falls back to localized name when English is unavailable.
	std::string PreferredWriteName(FontFamilyRecord const& record, bool prefer_localized) const;
	std::string PreferredWriteName(FontFamilyId id, bool prefer_localized) const;

	/// Map an existing family name to the preferred write name when resolution
	/// is unique and safe. On None/Ambiguous, returns the input unchanged.
	std::string MapToPreferredWriteName(std::string_view name, bool prefer_localized) const;

	/// Build display list names for the font picker (one name per family).
	std::vector<std::string> DisplayNames(bool prefer_localized) const;

	/// Split optional leading '@' vertical prefix. Returns {prefix, bare_name}.
	static std::pair<std::string, std::string> SplitVerticalPrefix(std::string_view name);
	/// Reattach vertical prefix when present.
	static std::string JoinVerticalPrefix(bool vertical, std::string_view bare_name);
	/// UTF-16 code unit length of the name (for GDI LF_FACESIZE - 1 checks).
	static std::size_t Utf16CodeUnitLength(std::string_view utf8);

private:
	void BuildIndex();

	std::vector<FontFamilyRecord> records_;
	// lowercased alias -> family id(s). Multiple ids => ambiguous.
	std::unordered_map<std::string, std::vector<FontFamilyId>> alias_index_;
	// exact original alias spelling -> family id(s) for exact match preference.
	std::unordered_map<std::string, std::vector<FontFamilyId>> exact_index_;
};

/// Build a catalog for the current platform. Empty when unsupported.
FontFamilyCatalog BuildFontFamilyCatalog();
