#include "audio_provider_factory.h"
#include "include/aegisub/video_provider.h"
#include "provider_catalog.h"
#include "provider_factory_registry.h"
#include "video_provider_manager.h"

#include <libaegisub/audio/provider.h>

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::unique_ptr<agi::AudioProvider> NullAudioProvider(agi::fs::path const&,
                                                      agi::BackgroundRunner *,
                                                      std::shared_ptr<agi::SingleChoiceInteractionSink>) {
	return nullptr;
}

std::unique_ptr<VideoProvider> NullVideoProvider(agi::fs::path const&,
                                                 std::string const&,
                                                 agi::BackgroundRunner *,
                                                 std::shared_ptr<agi::SingleChoiceInteractionSink>) {
	return nullptr;
}

class CachingVideoProvider final : public VideoProvider {
public:
	void GetFrame(int, VideoFrame&) override {
	}

	void SetColorSpace(std::string const&) override {
	}

	int GetFrameCount() const override { return 1; }
	int GetWidth() const override { return 2; }
	int GetHeight() const override { return 2; }
	double GetDAR() const override { return 1.0; }
	agi::vfr::Framerate GetFPS() const override { return agi::vfr::Framerate(24.0); }
	std::vector<int> GetKeyFrames() const override { return {}; }
	std::string GetColorSpace() const override { return "BT.709"; }
	std::string GetDecoderName() const override { return "HostCachingVideo"; }
	bool WantsCaching() const override { return true; }
};

std::unique_ptr<VideoProvider> CachingVideoProviderFactory(agi::fs::path const&,
                                                           std::string const&,
                                                           agi::BackgroundRunner *,
                                                           std::shared_ptr<agi::SingleChoiceInteractionSink>) {
	return std::make_unique<CachingVideoProvider>();
}

int CountName(std::vector<std::string> const& names, char const* name) {
	return static_cast<int>(std::count(names.begin(), names.end(), std::string(name)));
}

std::vector<std::string> VisibleNames(aegisub::provider_catalog::ProviderCatalog const& catalog) {
	return aegisub::provider_catalog::VisibleProviderNames(catalog);
}

void RequireNameCount(std::vector<std::string> const& names, char const* name, int expected, char const* context) {
	auto count = CountName(names, name);
	if (count != expected) {
		std::cerr << context << " count for " << name << " was " << count << ", expected " << expected << "\n";
		std::cerr << context << " names:";
		for (auto const& provider_name : names)
			std::cerr << " " << provider_name;
		std::cerr << "\n";
		std::exit(1);
	}
}

void RequireBefore(std::vector<std::string> const& names, char const* earlier, char const* later, char const* context) {
	auto earlier_it = std::find(names.begin(), names.end(), std::string(earlier));
	auto later_it = std::find(names.begin(), names.end(), std::string(later));
	if (earlier_it == names.end() || later_it == names.end() || earlier_it > later_it) {
		std::cerr << context << " expected " << earlier << " before " << later << "\n";
		std::cerr << context << " visible names:";
		for (auto const& provider_name : names)
			std::cerr << " " << provider_name;
		std::cerr << "\n";
		std::exit(1);
	}
}

void RequireCatalogProvider(aegisub::provider_catalog::ProviderCatalog const& catalog, char const* name, bool hidden, char const* context) {
	auto provider = std::find_if(catalog.providers.begin(), catalog.providers.end(), [&](auto const& descriptor) {
		return descriptor.name == name;
	});
	if (provider == catalog.providers.end()) {
		std::cerr << context << " catalog did not include " << name << "\n";
		std::exit(1);
	}
	if (provider->hidden != hidden) {
		std::cerr << context << " catalog hidden flag for " << name << " was " << provider->hidden
		          << ", expected " << hidden << "\n";
		std::exit(1);
	}
}

void CheckInvalidRegistrationIsRejected() {
	if (TryRegisterAudioProviderFactory({nullptr, NullAudioProvider, nullptr, nullptr, false}))
		throw std::runtime_error("audio provider registration accepted a null name");
	if (TryRegisterAudioProviderFactory({"", NullAudioProvider, nullptr, nullptr, false}))
		throw std::runtime_error("audio provider registration accepted an empty name");
	if (TryRegisterAudioProviderFactory({"InvalidAudio", nullptr, nullptr, nullptr, false}))
		throw std::runtime_error("audio provider registration accepted a null create callback");

	bool audio_invalid_name_threw = false;
	try {
		RegisterAudioProviderFactory({"", NullAudioProvider, nullptr, nullptr, false});
	}
	catch (std::invalid_argument const&) {
		audio_invalid_name_threw = true;
	}
	if (!audio_invalid_name_threw)
		throw std::runtime_error("throwing audio registration API did not reject an empty name");

	bool audio_null_create_threw = false;
	try {
		RegisterAudioProviderFactory({"InvalidAudio", nullptr, nullptr, nullptr, false});
	}
	catch (std::invalid_argument const&) {
		audio_null_create_threw = true;
	}
	if (!audio_null_create_threw)
		throw std::runtime_error("throwing audio registration API did not reject a null create callback");

	if (TryRegisterVideoProviderFactory({nullptr, NullVideoProvider, nullptr, nullptr, false}))
		throw std::runtime_error("video provider registration accepted a null name");
	if (TryRegisterVideoProviderFactory({"", NullVideoProvider, nullptr, nullptr, false}))
		throw std::runtime_error("video provider registration accepted an empty name");
	if (TryRegisterVideoProviderFactory({"InvalidVideo", nullptr, nullptr, nullptr, false}))
		throw std::runtime_error("video provider registration accepted a null create callback");

	bool video_invalid_name_threw = false;
	try {
		RegisterVideoProviderFactory({"", NullVideoProvider, nullptr, nullptr, false});
	}
	catch (std::invalid_argument const&) {
		video_invalid_name_threw = true;
	}
	if (!video_invalid_name_threw)
		throw std::runtime_error("throwing video registration API did not reject an empty name");

	bool video_null_create_threw = false;
	try {
		RegisterVideoProviderFactory({"InvalidVideo", nullptr, nullptr, nullptr, false});
	}
	catch (std::invalid_argument const&) {
		video_null_create_threw = true;
	}
	if (!video_null_create_threw)
		throw std::runtime_error("throwing video registration API did not reject a null create callback");
}

void CheckAudioRegistry() {
	if (!TryRegisterAudioProviderFactory({"HostAudioA", NullAudioProvider, nullptr, nullptr, false}))
		throw std::runtime_error("initial audio provider registration failed before freeze");
	if (!TryRegisterAudioProviderFactory({"HostAudioB", NullAudioProvider, nullptr, nullptr, false}))
		throw std::runtime_error("second audio provider registration failed before freeze");
	if (!TryRegisterAudioProviderFactory({"HostAudioHidden", NullAudioProvider, nullptr, nullptr, true}))
		throw std::runtime_error("hidden audio provider registration failed before freeze");
	if (!TryRegisterAudioProviderFactory({"HostAudioA", NullAudioProvider, nullptr, nullptr, false}))
		throw std::runtime_error("duplicate audio provider registration should be idempotent before freeze");

	auto names = GetAudioProviderNames();
	RequireNameCount(names, "HostAudioA", 1, "audio visible provider");
	RequireNameCount(names, "HostAudioB", 1, "audio visible provider");
	RequireNameCount(names, "HostAudioHidden", 0, "audio visible provider");

	auto catalog = GetAudioProviderCatalog("HostAudioB");
	auto visible = VisibleNames(catalog);
	RequireBefore(visible, "HostAudioB", "HostAudioA", "audio preferred provider ordering");
	RequireCatalogProvider(catalog, "HostAudioHidden", true, "audio provider");
}

void CheckVideoRegistry() {
	if (!TryRegisterVideoProviderFactory({"HostVideoA", NullVideoProvider, nullptr, nullptr, false}))
		throw std::runtime_error("initial video provider registration failed before freeze");
	if (!TryRegisterVideoProviderFactory({"HostVideoB", NullVideoProvider, nullptr, nullptr, false}))
		throw std::runtime_error("second video provider registration failed before freeze");
	if (!TryRegisterVideoProviderFactory({"HostVideoHidden", NullVideoProvider, nullptr, nullptr, true}))
		throw std::runtime_error("hidden video provider registration failed before freeze");
	if (!TryRegisterVideoProviderFactory({"HostVideoA", NullVideoProvider, nullptr, nullptr, false}))
		throw std::runtime_error("duplicate video provider registration should be idempotent before freeze");

	auto names = VideoProviderFactory::GetClasses();
	RequireNameCount(names, "HostVideoA", 1, "video visible provider");
	RequireNameCount(names, "HostVideoB", 1, "video visible provider");
	RequireNameCount(names, "HostVideoHidden", 0, "video visible provider");

	auto catalog = VideoProviderFactory::GetCatalog("HostVideoB");
	auto visible = VisibleNames(catalog);
	RequireBefore(visible, "HostVideoB", "HostVideoA", "video preferred provider ordering");
	RequireCatalogProvider(catalog, "HostVideoHidden", true, "video provider");
}

void CheckVideoCacheOptionSemantics() {
	if (!TryRegisterVideoProviderFactory({"HostVideoCaching", CachingVideoProviderFactory, nullptr, nullptr, false}))
		throw std::runtime_error("caching video provider registration failed before freeze");

	auto default_cache = VideoProviderFactory::GetProviderWithPreferred(
		agi::fs::path(),
		{},
		"HostVideoCaching",
		nullptr,
		{},
		std::nullopt);
	if (!default_cache)
		throw std::runtime_error("default cache video provider open returned null");
	if (default_cache->WantsCaching())
		throw std::runtime_error("default video cache option did not wrap a provider that wants caching");

	auto disabled_cache = VideoProviderFactory::GetProviderWithPreferred(
		agi::fs::path(),
		{},
		"HostVideoCaching",
		nullptr,
		{},
		std::size_t{0});
	if (!disabled_cache)
		throw std::runtime_error("disabled cache video provider open returned null");
	if (!disabled_cache->WantsCaching())
		throw std::runtime_error("explicit zero video cache option should disable wrapping");

	auto limited_cache = VideoProviderFactory::GetProviderWithPreferred(
		agi::fs::path(),
		{},
		"HostVideoCaching",
		nullptr,
		{},
		std::size_t{64});
	if (!limited_cache)
		throw std::runtime_error("limited cache video provider open returned null");
	if (limited_cache->WantsCaching())
		throw std::runtime_error("explicit non-zero video cache option did not wrap a provider that wants caching");
}

void CheckHostLifecycleFinalizationRejectsLateRegistration() {
	if (AreProviderFactoryRegistriesFinalized())
		throw std::runtime_error("provider registries were finalized before host startup completed");

	FinalizeProviderFactoryRegistries();
	if (!AreProviderFactoryRegistriesFinalized())
		throw std::runtime_error("provider registries did not report finalized after host finalization");

	if (TryRegisterAudioProviderFactory({"LateAudio", NullAudioProvider, nullptr, nullptr, false}))
		throw std::runtime_error("late audio registration succeeded after freeze");
	if (TryRegisterVideoProviderFactory({"LateVideo", NullVideoProvider, nullptr, nullptr, false}))
		throw std::runtime_error("late video registration succeeded after freeze");

	bool audio_threw = false;
	try {
		RegisterAudioProviderFactory({"ThrowingLateAudio", NullAudioProvider, nullptr, nullptr, false});
	}
	catch (std::logic_error const&) {
		audio_threw = true;
	}
	if (!audio_threw)
		throw std::runtime_error("throwing audio registration API did not reject late registration");

	bool video_threw = false;
	try {
		RegisterVideoProviderFactory({"ThrowingLateVideo", NullVideoProvider, nullptr, nullptr, false});
	}
	catch (std::logic_error const&) {
		video_threw = true;
	}
	if (!video_threw)
		throw std::runtime_error("throwing video registration API did not reject late registration");
}

}

int main() {
	try {
		CheckInvalidRegistrationIsRejected();
		CheckAudioRegistry();
		CheckVideoRegistry();
		CheckVideoCacheOptionSemantics();
		CheckHostLifecycleFinalizationRejectsLateRegistration();
	}
	catch (std::exception const& err) {
		std::cerr << "provider_manager_registry_smoke failed: " << err.what() << "\n";
		return 1;
	}

	std::cout << "provider_manager_registry_smoke: provider registration, catalog visibility, ordering, and host finalization are stable\n";
	return 0;
}
