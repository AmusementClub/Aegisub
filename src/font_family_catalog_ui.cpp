#include "font_family_catalog_ui.h"

#include "compat.h"
#include "font_family_catalog_cache.h"
#include "options.h"

#if defined(_WIN32)
#include "gdi_font_resolver.h"
#endif

#include <wx/fontenum.h>
#include <wx/utils.h>

#include <algorithm>
#include <string>
#include <utility>

FontFamilySelectionModel BuildFontFamilyCatalogUiModel() {
	bool prefer_localized = true;
	try {
		prefer_localized = OPT_GET("Subtitle/Font/Prefer Localized Family Names")->GetBool();
	}
	catch (...) {
		prefer_localized = true;
	}

	// Default A: system-like list with '@' rows. Option enables C: compact toggle.
	bool compact_vertical = false;
#if defined(_WIN32)
	try {
		compact_vertical = OPT_GET("Subtitle/Font/Compact Vertical Font List")->GetBool();
	}
	catch (...) {
		compact_vertical = false;
	}
#endif

	std::shared_ptr<FontFamilyCatalog const> catalog;
	if (prefer_localized) {
		catalog = font_family_catalog_cache::GetReadySnapshot();
		if (!catalog)
			font_family_catalog_cache::WarmAsync();
	}
	else {
		wxBusyCursor wait_cursor;
		catalog = font_family_catalog_cache::TryGetSnapshot();
		if (!catalog)
			font_family_catalog_cache::WarmAsync();
	}

	std::vector<std::string> fallback_names;
#if defined(_WIN32)
	GdiFontResolver gdi_resolver;
	GdiFontResolver::FamilyEnumeration gdi_faces;
	if (gdi_resolver.available())
		gdi_faces = gdi_resolver.EnumerateAllFamilies();
#endif
	if (!catalog || catalog->empty()) {
#if defined(_WIN32)
		if (!gdi_faces.horizontal.empty() || !gdi_faces.vertical.empty()) {
			fallback_names = gdi_faces.horizontal;
			// Mode A: fallback list includes GDI '@' faces. Mode C: horizontal only.
			if (!compact_vertical) {
				fallback_names.insert(
					fallback_names.end(),
					gdi_faces.vertical.begin(), gdi_faces.vertical.end());
			}
			std::sort(fallback_names.begin(), fallback_names.end());
			fallback_names.erase(
				std::unique(fallback_names.begin(), fallback_names.end()),
				fallback_names.end());
		}
#endif
		if (fallback_names.empty()) {
			auto choices = wxFontEnumerator::GetFacenames();
			choices.Sort();
			fallback_names.reserve(choices.size());
			for (auto const& choice : choices) {
				auto name = from_wx(choice);
#if defined(_WIN32)
				if (compact_vertical && !name.empty() && name.front() == '@')
					continue;
#endif
				fallback_names.push_back(std::move(name));
			}
		}
	}

	auto model = BuildFontFamilySelectionModel(
		std::move(catalog), prefer_localized, std::move(fallback_names));
	model.vertical_ui_mode = compact_vertical
		? VerticalFontUiMode::CompactToggle
		: VerticalFontUiMode::SystemList;

#if defined(_WIN32)
	FillVerticalCapability(model, gdi_faces.vertical);
	// Mode A only: attach '@' rows to the catalog-backed choice list.
	if (model.catalog && !model.catalog->empty() &&
	    model.vertical_ui_mode == VerticalFontUiMode::SystemList)
		AppendVerticalFacenameChoices(model, gdi_faces.vertical);
#endif
	return model;
}
