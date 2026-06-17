#pragma once

#include "provider_catalog.h"
#include "provider_selection_diagnostics.h"

#include <cstddef>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aegisub::provider_catalog {

struct ProviderFactoryDescriptor {
	char const* name = nullptr;
	bool hidden = false;
	bool (*is_available)() = nullptr;
	std::string (*availability_error)() = nullptr;
};

inline std::string_view ProviderName(ProviderFactoryDescriptor const& provider) {
	return provider.name ? std::string_view(provider.name) : std::string_view();
}

template<typename Container, typename Describe>
std::vector<std::string> VisibleFactoryNames(Container const& providers, Describe describe) {
	std::vector<std::string> names;
	for (auto const& provider : providers) {
		auto descriptor = describe(provider);
		if (!descriptor.hidden)
			names.emplace_back(ProviderName(descriptor));
	}
	return names;
}

template<typename Container, typename Describe>
auto SortFactories(Container const& providers, std::string const& preferred, Describe describe) -> std::vector<decltype(&*std::begin(providers))> {
	using std::begin;
	using std::end;
	using value_ptr = decltype(&*begin(providers));

	std::vector<value_ptr> sorted;
	sorted.reserve(std::distance(begin(providers), end(providers)));
	size_t end_of_hidden = 0;
	bool any_hidden = false;
	for (auto const& provider : providers) {
		auto descriptor = describe(provider);
		if (descriptor.hidden) {
			sorted.push_back(&provider);
			any_hidden = true;
		}
		else if (any_hidden && end_of_hidden == 0) {
			end_of_hidden = sorted.size();
			sorted.push_back(&provider);
		}
		else if (preferred == ProviderName(descriptor))
			sorted.insert(sorted.begin() + end_of_hidden, &provider);
		else
			sorted.push_back(&provider);
	}
	return sorted;
}

template<typename Container, typename Describe>
ProviderCatalog BuildCatalog(ProviderKind kind,
                             Container const& providers,
                             std::string const& preferred_provider,
                             Describe describe) {
	auto preferred = provider_selection_diagnostics::CanonicalizeProviderName(preferred_provider);
	auto sorted = SortFactories(providers, preferred, describe);

	ProviderCatalog catalog;
	catalog.kind = kind;
	catalog.preferred_provider = preferred;
	catalog.providers.reserve(sorted.size());

	for (auto const* provider : sorted) {
		auto provider_descriptor = describe(*provider);
		auto unavailable_reason = ProviderUnavailableReason(provider_descriptor);
		catalog.providers.push_back(BuildProviderDescriptor(
			catalog.kind,
			provider_descriptor,
			!unavailable_reason,
			unavailable_reason ? std::move(*unavailable_reason) : std::string()));
	}

	return catalog;
}

std::string GetAvailabilityError(ProviderFactoryDescriptor const& provider);
bool IsProviderAvailable(ProviderFactoryDescriptor const& provider, std::string& availability_error);
std::optional<std::string> ProviderUnavailableReason(ProviderFactoryDescriptor const& provider);

ProviderDescriptor BuildProviderDescriptor(ProviderKind kind,
                                           ProviderFactoryDescriptor const& provider,
                                           bool available,
                                           std::string availability_error);

} // namespace aegisub::provider_catalog
