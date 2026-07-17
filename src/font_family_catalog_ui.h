#pragma once

#include <memory>
#include <string>
#include <string_view>

#include <wx/arrstr.h>

class FontFamilyCatalog;

/// Font family names presented by GUI font selectors under the current
/// localized/English preference.
struct FontFamilyCatalogUiModel {
	bool prefer_localized = true;
	std::shared_ptr<FontFamilyCatalog const> catalog;
	wxArrayString choices;

	/// Name to show in the selector and write back when the dialog is accepted.
	std::string PreferredName(std::string_view stored_name) const;
};

/// Build one consistent model for every Style Editor entry point.
FontFamilyCatalogUiModel BuildFontFamilyCatalogUiModel();
