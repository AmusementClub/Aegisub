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
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#if defined(_WIN32)
namespace {

std::mutex g_gdi_faces_mutex;
std::uint64_t g_gdi_faces_generation = std::numeric_limits<std::uint64_t>::max();
std::shared_ptr<GdiFontResolver::FamilyEnumeration const> g_gdi_faces;

std::shared_ptr<GdiFontResolver::FamilyEnumeration const> GetGdiFaces(
	std::uint64_t generation) {
	std::lock_guard lock(g_gdi_faces_mutex);
	if (g_gdi_faces && g_gdi_faces_generation == generation)
		return g_gdi_faces;

	GdiFontResolver resolver;
	auto faces = resolver.available()
		? std::make_shared<GdiFontResolver::FamilyEnumeration const>(
			resolver.EnumerateAllFamilies())
		: std::make_shared<GdiFontResolver::FamilyEnumeration const>();
	g_gdi_faces_generation = generation;
	g_gdi_faces = faces;
	return faces;
}

} // namespace
#endif

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
	auto const gdi_faces = GetGdiFaces(font_family_catalog_cache::GetGeneration());
#endif
	if (!catalog || catalog->empty()) {
#if defined(_WIN32)
		if (!gdi_faces->horizontal.empty() || !gdi_faces->vertical.empty()) {
			fallback_names = gdi_faces->horizontal;
			// Mode A: fallback list includes GDI '@' faces. Mode C: horizontal only.
			if (!compact_vertical) {
				fallback_names.insert(
					fallback_names.end(),
					gdi_faces->vertical.begin(), gdi_faces->vertical.end());
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
	FillVerticalCapability(model, gdi_faces->vertical);
	// Mode A only: attach '@' rows to the catalog-backed choice list.
	if (model.catalog && !model.catalog->empty() &&
	    model.vertical_ui_mode == VerticalFontUiMode::SystemList)
		AppendVerticalFacenameChoices(model, gdi_faces->vertical);
#endif
	return model;
}
