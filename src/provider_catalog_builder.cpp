#include "provider_catalog_builder.h"

#include <libaegisub/exception.h>

#include <exception>

namespace aegisub::provider_catalog {

std::string GetAvailabilityError(ProviderFactoryDescriptor const& provider) {
	if (!provider.availability_error)
		return "runtime library is unavailable.";

	try {
		return provider.availability_error();
	}
	catch (agi::Exception const& err) {
		return err.GetMessage();
	}
	catch (std::exception const& err) {
		return err.what();
	}
	catch (...) {
		return "unknown availability error";
	}
}

bool IsProviderAvailable(ProviderFactoryDescriptor const& provider, std::string& availability_error) {
	if (!provider.is_available)
		return true;

	try {
		if (provider.is_available())
			return true;
	}
	catch (agi::Exception const& err) {
		availability_error = err.GetMessage();
		return false;
	}
	catch (std::exception const& err) {
		availability_error = err.what();
		return false;
	}
	catch (...) {
		availability_error = "unknown availability exception";
		return false;
	}

	availability_error = GetAvailabilityError(provider);
	return false;
}

std::optional<std::string> ProviderUnavailableReason(ProviderFactoryDescriptor const& provider) {
	std::string availability_error;
	if (IsProviderAvailable(provider, availability_error))
		return std::nullopt;
	if (availability_error.empty())
		availability_error = GetAvailabilityError(provider);
	if (availability_error.empty())
		availability_error = "runtime library is unavailable.";
	return availability_error;
}

ProviderDescriptor BuildProviderDescriptor(ProviderKind kind,
                                           ProviderFactoryDescriptor const& provider,
                                           bool available,
                                           std::string availability_error) {
	ProviderDescriptor descriptor;
	descriptor.kind = kind;
	descriptor.name = provider.name ? provider.name : "";
	descriptor.display_name = descriptor.name;
	descriptor.hidden = provider.hidden;
	descriptor.available = available;
	descriptor.unavailable_reason = std::move(availability_error);
	if (!descriptor.hidden && !descriptor.available)
		descriptor.display_name.append(" (Unavailable)");
	return descriptor;
}

} // namespace aegisub::provider_catalog
