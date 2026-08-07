#include "secondary_subtitle_reload_policy.h"

#include "ass_style_storage.h"

SecondarySubtitleExternalReloadAction PlanSecondarySubtitleExternalReload(
	bool active,
	bool has_provider,
	bool supports_in_place_reload) noexcept {
	if (!active)
		return SecondarySubtitleExternalReloadAction::ReleaseProvider;
	if (has_provider && supports_in_place_reload)
		return SecondarySubtitleExternalReloadAction::ReloadInPlace;
	return SecondarySubtitleExternalReloadAction::RebuildProvider;
}

agi::fs::path ResolveSecondarySubtitleStyleCatalogWatchPath(
	bool is_external_source,
	bool is_srt,
	std::string const& catalog_name) {
	if (!is_external_source || !is_srt)
		return {};
	return AssStyleStorage::GetCatalogPath(catalog_name);
}
