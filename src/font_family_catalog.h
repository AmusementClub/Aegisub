#pragma once

#include <array>
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

/// Physical style role reported by a font selection backend.
enum class FontVariantRole : std::uint8_t {
	Unknown,
	Regular,
	Bold,
	Italic,
	BoldItalic
};

struct FontFamilyName {
	std::string value;
	std::string locale;
	FontFamilyNameKind kind = FontFamilyNameKind::Win32Family;
	std::uint64_t entity_token = 0;
	FontVariantRole variant_role = FontVariantRole::Unknown;
};

using FontFamilyId = std::uint32_t;

/// Confidence/safety classification for a backend selection outcome.
enum class FontVariantStatus : std::uint8_t {
	Unknown,
	Canonical,
	NonCanonical,
	Synthetic
};

/// Matching semantics used to produce a family profile. This identifies the
/// authority being modeled; different backends need not select the same face.
enum class FontVariantBackend : std::uint8_t {
	Unknown,
	VsFilterGdi,
	LibassScoring,
	CoreText,
	Fontconfig
};

/// Evidence class for a backend outcome. Observed means the backend selected
/// and inspected the physical face it would render; Algorithmic means the
/// result is only a score/prediction and is therefore report-only for
/// portability fixes.
enum class FontSelectionEvidence : std::uint8_t {
	None,
	Observed,
	Algorithmic
};

/// Lightweight result for one canonical backend request. entity_token is
/// opaque, process-local identity; it never contains or serializes a path.
struct FontVariantOutcome {
	int requested_weight = 400;
	bool requested_italic = false;
	int realized_weight = 0;
	bool realized_italic = false;
	FontVariantRole role = FontVariantRole::Unknown;
	FontVariantStatus status = FontVariantStatus::Unknown;
	std::uint64_t entity_token = 0;
};

struct FontVariantChoice {
	FontVariantRole role = FontVariantRole::Unknown;
	int weight = 400;
	bool italic = false;
	FontVariantStatus status = FontVariantStatus::Unknown;
	std::uint64_t entity_token = 0;
};

/// Backend outcomes in this order: Regular, Bold, Italic, Bold Italic.
struct FontFamilyVariantProfile {
	std::array<FontVariantOutcome, 4> outcomes{};
	FontVariantBackend backend = FontVariantBackend::Unknown;
	FontSelectionEvidence evidence = FontSelectionEvidence::None;
	/// Algorithmic profiles may expose explicit choices but cannot trigger an
	/// automatic ASS pin without observed physical-selection evidence.
	bool automatic_pinning_reliable = false;

	FontVariantOutcome const& For(bool bold, bool italic) const noexcept;
};

/// Package four canonical backend outcomes in RBIZ order. Selectable
/// choices and implicit pins are deliberately derived by font_variant_policy
/// rather than cached in the snapshot.
FontFamilyVariantProfile BuildFontFamilyVariantProfile(
	std::array<FontVariantOutcome, 4> outcomes,
	FontVariantBackend backend = FontVariantBackend::Unknown,
	FontSelectionEvidence evidence = FontSelectionEvidence::None,
	bool automatic_pinning_reliable = false);

struct FontFamilyRecord {
	FontFamilyId id = 0;
	std::string localized_family_name;
	std::string english_win32_family_name;
	std::vector<FontFamilyName> names;
	FontFamilyVariantProfile variant_profile;
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
	FontVariantRole variant_role = FontVariantRole::Unknown;
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

	/// Resolve Full Name/PostScript metadata for explicit font-dialog input.
	/// These names are intentionally excluded from Resolve() and normal ASS
	/// family alias normalization. Ambiguous family or variant matches fail.
	FontFamilyResolution ResolveInformationalName(std::string_view name) const;

	/// Preferred ASS write name for a family under the given preference.
	/// Falls back to localized name when English is unavailable.
	std::string PreferredWriteName(FontFamilyRecord const& record, bool prefer_localized) const;
	std::string PreferredWriteName(FontFamilyId id, bool prefer_localized) const;

	/// Map an existing family name to the preferred write name when resolution
	/// is unique and safe. On None/Ambiguous, returns the input unchanged.
	std::string MapToPreferredWriteName(std::string_view name, bool prefer_localized) const;

	/// Split optional leading '@' vertical prefix. Returns {prefix, bare_name}.
	static std::pair<std::string, std::string> SplitVerticalPrefix(std::string_view name);
	/// Reattach vertical prefix when present.
	static std::string JoinVerticalPrefix(bool vertical, std::string_view bare_name);
	/// UTF-16 code unit length of the name (for GDI LF_FACESIZE - 1 checks).
	static std::size_t Utf16CodeUnitLength(std::string_view utf8);

private:
#if defined(_WIN32)
	struct CaseInsensitiveAliasEntry {
		std::wstring name;
		std::vector<FontFamilyId> families;
	};
#endif

	void BuildIndex();

	std::vector<FontFamilyRecord> records_;
	std::unordered_map<FontFamilyId, std::size_t> id_index_;
	// Informational Full/PostScript name -> candidate physical family/variant.
	// Ambiguity is retained explicitly so callers can distinguish it from a
	// missing name without rescanning every record.
	std::unordered_map<std::string, FontFamilyResolution> informational_index_;
	// Case-insensitive alias -> family id(s). Windows stores UTF-16 names in
	// CompareStringOrdinal order; other platforms retain the legacy key.
#if defined(_WIN32)
	std::vector<CaseInsensitiveAliasEntry> alias_index_;
#else
	std::unordered_map<std::string, std::vector<FontFamilyId>> alias_index_;
#endif
	// exact original alias spelling -> family id(s) for exact match preference.
	std::unordered_map<std::string, std::vector<FontFamilyId>> exact_index_;
};

/// Build a catalog for the current platform. Empty when unsupported.
FontFamilyCatalog BuildFontFamilyCatalog();
