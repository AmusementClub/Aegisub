#pragma once

#include "font_family_catalog.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

/// One item in a platform-neutral font family selector. A zero family id
/// identifies an enumerator fallback which is not backed by a catalog record.
struct FontFamilyChoice {
	std::string label;
	FontFamilyId family_id = 0;

	bool operator==(FontFamilyChoice const&) const = default;
};

/// Font family names and catalog state shared by GUI font selectors. This
/// model deliberately has no dependency on wxWidgets or a platform resolver.
struct FontFamilySelectionModel {
	bool prefer_localized = true;
	std::shared_ptr<FontFamilyCatalog const> catalog;
	std::vector<FontFamilyChoice> choices;

	/// Name to show in the selector and write back when the dialog is accepted.
	std::string PreferredName(std::string_view stored_name) const;

	/// Resolve a family through the same alias model used by every font UI.
	FontFamilyRecord const* ResolveRecord(std::string_view name) const;

	/// Resolve an item selected from choices without going back through its
	/// potentially ambiguous display alias.
	FontFamilyRecord const* ResolveChoice(FontFamilyId family_id) const;
};

/// Build catalog-backed choices, or retain the supplied ordered fallback list
/// when no catalog snapshot is available.
FontFamilySelectionModel BuildFontFamilySelectionModel(
	std::shared_ptr<FontFamilyCatalog const> catalog,
	bool prefer_localized,
	std::vector<std::string> fallback_names = {});
