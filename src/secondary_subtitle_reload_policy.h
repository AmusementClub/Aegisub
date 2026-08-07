#pragma once

#include <libaegisub/fs_fwd.h>

#include <string>

enum class SecondarySubtitleExternalReloadAction {
	ReleaseProvider,
	ReloadInPlace,
	RebuildProvider
};

SecondarySubtitleExternalReloadAction PlanSecondarySubtitleExternalReload(
	bool active,
	bool has_provider,
	bool supports_in_place_reload) noexcept;

agi::fs::path ResolveSecondarySubtitleStyleCatalogWatchPath(
	bool is_external_source,
	bool is_srt,
	std::string const& catalog_name);
