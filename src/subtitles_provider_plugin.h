// Copyright (c) 2026, MIRIMIRIM

#pragma once

#include <libaegisub/fs_fwd.h>

#include <memory>
#include <string>
#include <vector>

class SubtitlesProvider;
struct SubtitleRenderEnvironment;

namespace subtitle_plugin {
	std::vector<std::string> List();
	bool HasExternalFileProviderFor(agi::fs::path const& filename);
	std::vector<std::string> GetExternalFileProviderWildcards();
	std::unique_ptr<SubtitlesProvider> Create(std::string const& provider_name, SubtitleRenderEnvironment const& env);
}
