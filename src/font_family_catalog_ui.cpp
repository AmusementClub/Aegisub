#include "font_family_catalog_ui.h"

#include "compat.h"
#include "font_family_catalog.h"
#include "font_family_catalog_cache.h"
#include "options.h"

#include <wx/fontenum.h>

std::string FontFamilyCatalogUiModel::PreferredName(std::string_view stored_name) const {
	if (!catalog || catalog->empty())
		return std::string(stored_name);
	return catalog->MapToPreferredWriteName(stored_name, prefer_localized);
}

FontFamilyCatalogUiModel BuildFontFamilyCatalogUiModel() {
	FontFamilyCatalogUiModel model;
	try {
		model.prefer_localized = OPT_GET("Subtitle/Font/Prefer Localized Family Names")->GetBool();
	}
	catch (...) {
		model.prefer_localized = true;
	}

	// Frame startup normally prewarms this snapshot. A snapshot is also needed
	// in localized mode so an existing English ASS name can be displayed as the
	// localized name without changing the document until the user accepts.
	model.catalog = font_family_catalog_cache::GetSnapshot();

	if (model.prefer_localized || !model.catalog || model.catalog->empty()) {
		model.choices = wxFontEnumerator::GetFacenames();
		model.choices.Sort();
		return model;
	}

	auto names = model.catalog->DisplayNames(false);
	model.choices.reserve(names.size());
	for (auto const& name : names)
		model.choices.Add(to_wx(name));
	// DisplayNames already sorts and deduplicates.
	return model;
}
