#include "font_family_catalog_ui.h"

#include "compat.h"
#include "font_family_catalog_cache.h"
#include "options.h"

#include <wx/fontenum.h>
#include <wx/utils.h>

#include <utility>

FontFamilySelectionModel BuildFontFamilyCatalogUiModel() {
	bool prefer_localized = true;
	try {
		prefer_localized = OPT_GET("Subtitle/Font/Prefer Localized Family Names")->GetBool();
	}
	catch (...) {
		prefer_localized = true;
	}

	std::shared_ptr<FontFamilyCatalog const> catalog;
	if (prefer_localized) {
		// Localized mode uses the native system font dialog. Never block the UI
		// thread on catalog enumeration; warm the cache for the English path.
		catalog = font_family_catalog_cache::GetReadySnapshot();
		if (!catalog)
			font_family_catalog_cache::WarmAsync();
	}
	else {
		// English/custom Select Font needs the catalog (or at least a finished
		// attempt). Wait with a busy cursor so the first open after startup is
		// slow but subsequent opens hit the published snapshot.
		//
		// Typical Windows catalog build: every installed family × 4 RBIZ GDI
		// probes plus DWrite name reads — often ~1–10+ seconds depending on
		// font count and disk; see log "font/family_catalog" for measured ms.
		wxBusyCursor wait_cursor;
		catalog = font_family_catalog_cache::TryGetSnapshot();
		if (!catalog)
			font_family_catalog_cache::WarmAsync();
	}

	std::vector<std::string> fallback_names;
	if (!catalog || catalog->empty()) {
		// Preserve the legacy enumerator when the immutable catalog is not ready
		// or cannot be built so the custom dialog still has a face list.
		auto choices = wxFontEnumerator::GetFacenames();
		choices.Sort();
		fallback_names.reserve(choices.size());
		for (auto const& choice : choices)
			fallback_names.push_back(from_wx(choice));
	}

	return BuildFontFamilySelectionModel(
		std::move(catalog), prefer_localized, std::move(fallback_names));
}
