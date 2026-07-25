// Windows FontFamilyCatalog entry point. Live GDI/DirectWrite selection is
// isolated in ObserveWindowsFontFamilies; DeriveWindowsFontFamilyCatalog is a
// pure replay of those facts. The observation repository supplies a trusted
// disk hit (M0+M1) or a full re-observe with atomic save.

#include "font_family_catalog.h"
#include "font_family_catalog_cache.h"
#include "font_family_obs_repository_win.h"
#include "gdi_font_resolver.h"

#include <libaegisub/charset_conv_win.h>
#include <libaegisub/log.h>

#include <chrono>
#include <string>

namespace {

std::string user_default_locale() {
	wchar_t locale[LOCALE_NAME_MAX_LENGTH] = {};
	if (GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH) <= 0)
		return {};
	try {
		return agi::charset::ConvertW(locale);
	}
	catch (...) {
		return {};
	}
}

} // namespace

FontFamilyCatalog BuildFontFamilyCatalog() {
	auto const build_started = std::chrono::steady_clock::now();
	GdiFontResolver resolver;
	auto const cache_path = DefaultWindowsFontFamilyObsCachePath();
	auto const result = LoadOrRebuildWindowsFontFamilyCatalog(
		resolver, user_default_locale(),
		[] { return font_family_catalog_cache::IsShutdownRequested(); }, cache_path);
	auto const finished = std::chrono::steady_clock::now();
	auto const elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
		finished - build_started);
	auto const stats = resolver.stats();

	if (result.hit == FontFamilyObsHitKind::RebuildAborted) {
		LOG_I("font/family_catalog")
			<< "Aborting Windows font family catalog build: shutdown requested";
		return {};
	}

	LOG_I("font/family_catalog")
		<< "Windows font family catalog " << FontFamilyObsHitKindName(result.hit)
		<< " (" << result.catalog.size() << " families, faces="
		<< result.face_count
		<< (result.weak_m1 ? ", weak_m1" : "")
		<< ", " << result.physical_probe_count << " physical probes, "
		<< stats.memo_hit_count << " memo hits, "
		<< stats.fingerprint_read_count << " fingerprint reads, "
		<< elapsed.count() << " ms"
		<< (result.wrote_cache ? ", cache written" : "")
		<< (result.store_status != FontFamilyObsStoreStatus::Ok &&
					result.hit != FontFamilyObsHitKind::Hit
				? std::string(", store=") +
					FontFamilyObsStoreStatusName(result.store_status)
				: std::string())
		<< ")";
	return result.catalog;
}
