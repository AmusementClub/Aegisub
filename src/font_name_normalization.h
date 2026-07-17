#pragma once

#include "font_family_catalog.h"

#include <cstddef>
#include <span>
#include <string>
#include <vector>

class AssFile;

enum class FontNameNormalizationTarget {
	Localized,
	EnglishWin32
};

enum class FontNameSourceKind {
	Style,
	Override
};

struct FontNameSourceLocation {
	FontNameSourceKind kind = FontNameSourceKind::Style;
	std::string style;
	int line = 0;
	std::size_t override_index = 0;
	bool comment = false;
};

struct FontNameNormalizationChange {
	FontNameSourceLocation source;
	std::string current_name;
	std::string recommended_name;
	FontFamilyMatchKind match_kind = FontFamilyMatchKind::None;
	std::string reason_code;
	bool safe_to_apply = false;
};

struct FontNameNormalizationPlan {
	FontNameNormalizationTarget target = FontNameNormalizationTarget::Localized;
	bool catalog_available = false;
	std::size_t scanned_name_count = 0;
	std::vector<FontNameNormalizationChange> changes;
};

/// Analyze style font names and non-empty explicit \fn tags. This function is
/// read-only: it never modifies the AssFile.
FontNameNormalizationPlan BuildFontNameNormalizationPlan(
	AssFile const& file,
	FontFamilyCatalog const& catalog,
	FontNameNormalizationTarget target,
	std::span<int const> style_source_lines = {});
