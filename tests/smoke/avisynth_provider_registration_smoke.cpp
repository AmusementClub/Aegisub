#ifdef WITH_AVISYNTH

#include "audio_provider_factory.h"
#include "avisynth_provider_registration.h"
#include "provider_factory_registry.h"
#include "video_provider_manager.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace {

int CountName(std::vector<std::string> const& names, char const* name) {
	return static_cast<int>(std::count(names.begin(), names.end(), std::string(name)));
}

int CountCatalogName(aegisub::provider_catalog::ProviderCatalog const& catalog, char const* name) {
	return static_cast<int>(std::count_if(catalog.providers.begin(), catalog.providers.end(), [&](auto const& provider) {
		return provider.name == name;
	}));
}

void RequireSingleAvisynth(std::vector<std::string> const& names, char const* kind) {
	auto count = CountName(names, "Avisynth");
	if (count != 1) {
		std::cerr << kind << " Avisynth provider registration count was " << count << ", expected 1\n";
		std::cerr << kind << " providers:";
		for (auto const& name : names)
			std::cerr << " " << name;
		std::cerr << "\n";
		std::exit(1);
	}
}

}

int main() {
	if (CountName(GetAudioProviderNames(), "Avisynth") != 0) {
		std::cerr << "Avisynth audio provider was registered before explicit initialization\n";
		return 1;
	}
	if (CountName(VideoProviderFactory::GetClasses(), "Avisynth") != 0) {
		std::cerr << "Avisynth video provider was registered before explicit initialization\n";
		return 1;
	}

	RegisterAvisynthProviderFactories();
	RegisterAvisynthProviderFactories();

	RequireSingleAvisynth(GetAudioProviderNames(), "audio");
	RequireSingleAvisynth(VideoProviderFactory::GetClasses(), "video");
	if (CountCatalogName(GetAudioProviderCatalog(), "Avisynth") != 1) {
		std::cerr << "audio catalog did not include exactly one Avisynth provider\n";
		return 1;
	}
	if (CountCatalogName(VideoProviderFactory::GetCatalog(), "Avisynth") != 1) {
		std::cerr << "video catalog did not include exactly one Avisynth provider\n";
		return 1;
	}

	if (AreProviderFactoryRegistriesFinalized()) {
		std::cerr << "provider registries were finalized before host initialization completed\n";
		return 1;
	}

	FinalizeProviderFactoryRegistries();
	if (!AreProviderFactoryRegistriesFinalized()) {
		std::cerr << "provider registries did not report finalized after FinalizeProviderFactoryRegistries\n";
		return 1;
	}
	if (TryRegisterAudioProviderFactory({"LateAudio", nullptr, nullptr, nullptr, false})) {
		std::cerr << "late audio provider registration succeeded after freeze\n";
		return 1;
	}
	if (TryRegisterVideoProviderFactory({"LateVideo", nullptr, nullptr, nullptr, false})) {
		std::cerr << "late video provider registration succeeded after freeze\n";
		return 1;
	}

	std::cout << "avisynth_provider_registration_smoke: explicit registration is visible, idempotent, and finalized after host init\n";
	return 0;
}

#else

int main() {
	return 0;
}

#endif
