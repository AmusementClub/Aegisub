#pragma once

#include "provider_selection_diagnostics.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aegisub::provider_catalog {

enum class ProviderKind {
	Audio,
	Video,
	Subtitles,
};

struct ProviderDescriptor {
	ProviderKind kind = ProviderKind::Audio;
	std::string name;
	std::string display_name;
	bool hidden = false;
	bool available = true;
	std::string unavailable_reason;
};

struct ProviderCatalog {
	ProviderKind kind = ProviderKind::Audio;
	std::string preferred_provider;
	std::vector<ProviderDescriptor> providers;
};

inline std::string DisplayName(ProviderDescriptor const& provider) {
	if (provider.display_name.empty())
		return provider.name;
	return provider.display_name;
}

inline bool IsPreferred(ProviderDescriptor const& provider, std::string_view preferred_provider) {
	return provider_selection_diagnostics::SameProviderName(provider.name, preferred_provider);
}

inline std::vector<ProviderDescriptor> VisibleProviders(ProviderCatalog const& catalog) {
	std::vector<ProviderDescriptor> visible;
	for (auto const& provider : catalog.providers) {
		if (!provider.hidden)
			visible.push_back(provider);
	}
	return visible;
}

inline std::vector<std::string> VisibleProviderNames(ProviderCatalog const& catalog) {
	std::vector<std::string> names;
	for (auto const& provider : catalog.providers) {
		if (!provider.hidden)
			names.push_back(provider.name);
	}
	return names;
}

inline std::vector<std::pair<std::string, std::string>> VisibleProviderChoices(ProviderCatalog const& catalog) {
	std::vector<std::pair<std::string, std::string>> choices;
	for (auto const& provider : catalog.providers) {
		if (!provider.hidden)
			choices.emplace_back(DisplayName(provider), provider.name);
	}
	return choices;
}

} // namespace aegisub::provider_catalog
