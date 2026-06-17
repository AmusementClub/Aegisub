#pragma once

#include "provider_catalog_builder.h"

#include <string>

namespace aegisub::provider_catalog {

template<typename Create>
struct ProviderFactoryEntry {
	char const* name = nullptr;
	Create create = nullptr;
	bool (*is_available)() = nullptr;
	std::string (*availability_error)() = nullptr;
	bool hidden = false;
};

template<typename Create>
ProviderFactoryDescriptor DescribeProviderFactoryEntry(ProviderFactoryEntry<Create> const& provider) {
	return { provider.name, provider.hidden, provider.is_available, provider.availability_error };
}

} // namespace aegisub::provider_catalog
