// Copyright (c) 2014, Thomas Goyne <plorkyeran@aegisub.org>
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
//
// Aegisub Project http://www.aegisub.org/

#pragma once

#include "provider_selection_diagnostics.h"
#include "provider_catalog.h"
#include "provider_factory_entry.h"

#include <libaegisub/fs_fwd.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class VideoProvider;
namespace agi {
	class BackgroundRunner;
	class SingleChoiceInteractionSink;
}

using VideoProviderCreate = std::unique_ptr<VideoProvider> (*)(agi::fs::path const&,
                                                               std::string const&,
                                                               agi::BackgroundRunner *,
                                                               std::shared_ptr<agi::SingleChoiceInteractionSink>);
using VideoProviderFactoryEntry = aegisub::provider_catalog::ProviderFactoryEntry<VideoProviderCreate>;

bool TryRegisterVideoProviderFactory(VideoProviderFactoryEntry factory);
void RegisterVideoProviderFactory(VideoProviderFactoryEntry factory);
void FreezeVideoProviderFactoryRegistry();
bool IsVideoProviderFactoryRegistryFrozen();
std::unique_ptr<VideoProvider> CreateCacheVideoProvider(std::unique_ptr<VideoProvider> parent);
std::unique_ptr<VideoProvider> CreateCacheVideoProvider(std::unique_ptr<VideoProvider> parent, size_t max_cache_size_bytes);

struct VideoProviderFactory {
	static aegisub::provider_catalog::ProviderCatalog GetCatalog(std::string const& preferred_provider = {});
	static std::vector<std::string> GetClasses();
	static std::vector<std::pair<std::string, std::string>> GetChoices();
	static std::unique_ptr<VideoProvider> GetProvider(agi::fs::path const& video_file, std::string const& colormatrix, agi::BackgroundRunner *br, std::shared_ptr<agi::SingleChoiceInteractionSink> choice_sink);
	static std::unique_ptr<VideoProvider> GetProviderWithPreferred(agi::fs::path const& video_file,
	                                                               std::string const& colormatrix,
	                                                               std::string const& preferred_provider,
	                                                               agi::BackgroundRunner *br,
	                                                               std::shared_ptr<agi::SingleChoiceInteractionSink> choice_sink,
	                                                               std::optional<size_t> max_cache_size_bytes = std::nullopt);
};

aegisub::provider_selection_diagnostics::SelectionReport GetLastVideoProviderSelectionReport();
void ClearLastVideoProviderSelectionReport();
