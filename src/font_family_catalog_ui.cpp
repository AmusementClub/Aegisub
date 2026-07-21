#include "font_family_catalog_ui.h"

#include "compat.h"
#include "font_family_catalog_cache.h"
#include "options.h"

#include <wx/fontenum.h>

#include <utility>

FontFamilySelectionModel BuildFontFamilyCatalogUiModel() {
	bool prefer_localized = true;
	try {
		prefer_localized = OPT_GET("Subtitle/Font/Prefer Localized Family Names")->GetBool();
	}
	catch (...) {
		prefer_localized = true;
	}

	// Never wait for font enumeration on an interactive path. Start a build for
	// the next invocation and use the legacy list until this generation is ready.
	auto catalog = font_family_catalog_cache::GetReadySnapshot();
	if (!catalog)
		font_family_catalog_cache::WarmAsync();

	std::vector<std::string> fallback_names;
	if (!catalog || catalog->empty()) {
		// Preserve the legacy enumerator while the immutable catalog is not ready
		// or cannot be built.
		auto choices = wxFontEnumerator::GetFacenames();
		choices.Sort();
		fallback_names.reserve(choices.size());
		for (auto const& choice : choices)
			fallback_names.push_back(from_wx(choice));
	}

	return BuildFontFamilySelectionModel(
		std::move(catalog), prefer_localized, std::move(fallback_names));
}
