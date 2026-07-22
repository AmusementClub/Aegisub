#include "scenechange_native_api.h"

#include "options.h"

#include <libaegisub/exception.h>
#include <libaegisub/native_library.h>

#include <mutex>
#include <string>

namespace scenechange {
namespace {

constexpr char kLogTag[] = "provider/scenechange/runtime";
constexpr char kScxvidLibraryName[] = "scenechange_xvid";
constexpr char kWwxdLibraryName[] = "scenechange_wwxd";

Api api;
Api scxvid_api;
Api wwxd_api;
std::mutex api_mutex;

char const *BackendName(Api::Backend backend) noexcept {
	switch (backend) {
		case Api::Backend::WwxdProvider: return "wwxd-provider";
		case Api::Backend::ScxvidProvider: return "scxvid";
		default: return "none";
	}
}

void ResolveProvider(scenechange_provider_get_api_fn get_api, int32_t expected_backend, Api& loaded) {
	scenechange_provider_api provider {};
	provider.struct_size = SCENECHANGE_PROVIDER_API_V1_SIZE;
	char error[1024] = {};
	std::int32_t const rc = get_api(
		SCENECHANGE_PROVIDER_API_VERSION,
		&provider,
		error,
		sizeof(error));
	if (rc != 0)
		throw agi::EnvironmentError(error[0] ? error : "SceneChange provider API negotiation failed.");
	if (provider.struct_size != SCENECHANGE_PROVIDER_API_V1_SIZE)
		throw agi::EnvironmentError("SceneChange provider returned a truncated function table.");
	if (provider.api_version != SCENECHANGE_PROVIDER_API_VERSION)
		throw agi::EnvironmentError("SceneChange provider returned an incompatible API version.");
	if (provider.backend != expected_backend)
		throw agi::EnvironmentError("SceneChange provider returned an unexpected backend.");
	int32_t const expected_pixel_format = expected_backend == SCENECHANGE_PROVIDER_BACKEND_SCXVID
		? SCENECHANGE_PROVIDER_PIXEL_FORMAT_YUV420P8
		: SCENECHANGE_PROVIDER_PIXEL_FORMAT_GRAY8_PADDED16;
	if (provider.input_pixel_format != expected_pixel_format)
		throw agi::EnvironmentError("SceneChange provider returned an incompatible input pixel format.");
	constexpr std::uint32_t required_capabilities =
		SCENECHANGE_PROVIDER_CAPABILITY_OWNED_WRITE_FRAME |
		SCENECHANGE_PROVIDER_CAPABILITY_RESET |
		SCENECHANGE_PROVIDER_CAPABILITY_PROGRESS_CALLBACK;
	if ((provider.capabilities & required_capabilities) != required_capabilities)
		throw agi::EnvironmentError("SceneChange provider does not expose the required capabilities.");
	if (!provider.create || !provider.destroy || !provider.reset ||
		!provider.set_progress_callback || !provider.get_write_frame ||
		!provider.commit_written_frame)
		throw agi::EnvironmentError("SceneChange provider returned an incomplete function table.");
	loaded.provider = provider;
	loaded.backend = expected_backend == SCENECHANGE_PROVIDER_BACKEND_SCXVID
		? Api::Backend::ScxvidProvider
		: Api::Backend::WwxdProvider;
}

std::string FormatRuntimeVersionDetail(Api const& loaded) {
	if (loaded.backend == Api::Backend::None)
		return {};
	return "provider-api=" + std::to_string(loaded.provider.api_version)
		+ ", backend=" + BackendName(loaded.backend);
}

std::string GetScxvidRuntimeVersionDetail() {
	std::lock_guard<std::mutex> lock(api_mutex);
	return FormatRuntimeVersionDetail(scxvid_api);
}

std::string GetWwxdRuntimeVersionDetail() {
	std::lock_guard<std::mutex> lock(api_mutex);
	return FormatRuntimeVersionDetail(wwxd_api);
}

void InitializeScxvidProvider(agi::native::Library& library) {
	Api loaded;
	auto get_api = library.ResolveSymbol<scenechange_provider_get_api_fn>("scenechange_provider_get_api");
	ResolveProvider(get_api, SCENECHANGE_PROVIDER_BACKEND_SCXVID, loaded);

	std::lock_guard<std::mutex> lock(api_mutex);
	scxvid_api = loaded;
	if (api.backend == Api::Backend::None)
		api = loaded;
}

void InitializeWwxdProvider(agi::native::Library& library) {
	Api loaded;
	auto get_api = library.ResolveSymbol<scenechange_provider_get_api_fn>("scenechange_provider_get_api");
	ResolveProvider(get_api, SCENECHANGE_PROVIDER_BACKEND_WWXD, loaded);

	std::lock_guard<std::mutex> lock(api_mutex);
	wwxd_api = loaded;
	if (api.backend == Api::Backend::None)
		api = loaded;
}

agi::native::CachedLibrary scxvid_provider_library(
	kScxvidLibraryName,
	"SceneChange Xvid provider",
	kLogTag,
	InitializeScxvidProvider,
	GetScxvidRuntimeVersionDetail,
	agi::native::DefaultAppLocalLoadOptions(false));

agi::native::CachedLibrary wwxd_provider_library(
	kWwxdLibraryName,
	"SceneChange WWXD provider",
	kLogTag,
	InitializeWwxdProvider,
	GetWwxdRuntimeVersionDetail,
	agi::native::DefaultAppLocalLoadOptions(false));

std::string FormatLoadError(std::string const& xvid_message, std::string const& wwxd_message) {
	return "Could not load SceneChange Xvid provider '" + std::string(kScxvidLibraryName)
		+ "' or WWXD provider '" + std::string(kWwxdLibraryName) + "'. xvid: "
		+ (xvid_message.empty() ? "unavailable" : xvid_message)
		+ "; wwxd: " + (wwxd_message.empty() ? "unavailable" : wwxd_message);
}

enum class BackendPreference {
	Auto,
	Scxvid,
	Wwxd,
};

BackendPreference GetBackendPreference() noexcept {
	auto const value = config::GetStringOptionOrDefault("Provider/SceneChange/Backend", "auto");
	if (value == "scxvid")
		return BackendPreference::Scxvid;
	if (value == "wwxd")
		return BackendPreference::Wwxd;
	return BackendPreference::Auto;
}

void Select(Api const& selected) {
	std::lock_guard<std::mutex> lock(api_mutex);
	api = selected;
}

} // namespace

void EnsureLoaded() {
	{
		std::lock_guard<std::mutex> lock(api_mutex);
		if (api.backend != Api::Backend::None)
			return;
	}
	if (scxvid_provider_library.IsAvailable()) {
		std::lock_guard<std::mutex> lock(api_mutex);
		if (api.backend == Api::Backend::None)
			api = scxvid_api;
		return;
	}
	if (wwxd_provider_library.IsAvailable()) {
		std::lock_guard<std::mutex> lock(api_mutex);
		if (api.backend == Api::Backend::None)
			api = wwxd_api;
		return;
	}
	throw agi::EnvironmentError(FormatLoadError(
		scxvid_provider_library.GetLoadError(),
		wwxd_provider_library.GetLoadError()));
}

bool IsAvailable() noexcept {
	try {
		EnsureLoaded();
		return true;
	}
	catch (...) {
		return false;
	}
}

std::string GetLoadError() {
	if (IsAvailable())
		return {};
	return FormatLoadError(
		scxvid_provider_library.GetLoadError(),
		wwxd_provider_library.GetLoadError());
}

std::string GetLoadedLibrary() {
	EnsureLoaded();
	Api::Backend backend;
	{
		std::lock_guard<std::mutex> lock(api_mutex);
		backend = api.backend;
	}
	return backend == Api::Backend::ScxvidProvider
		? scxvid_provider_library.GetLoadedLibrary()
		: wwxd_provider_library.GetLoadedLibrary();
}

Api const& GetApi() {
	EnsureLoaded();
	return api;
}

std::string GetBackendName() {
	EnsureLoaded();
	std::lock_guard<std::mutex> lock(api_mutex);
	return BackendName(api.backend);
}

std::string GetCacheToken() {
	if (!IsAvailable())
		return "wwxd";
	std::lock_guard<std::mutex> lock(api_mutex);
	return api.backend == Api::Backend::ScxvidProvider ? "scxvid" : "wwxd";
}

bool SupportsDimensions(int32_t width, int32_t height) noexcept {
	if (width <= 0 || height <= 0)
		return false;

	const bool even_dimensions = (width & 1) == 0 && (height & 1) == 0;
	const auto preference = GetBackendPreference();
	const bool xvid_available = even_dimensions && scxvid_provider_library.IsAvailable();
	const bool wwxd_available = wwxd_provider_library.IsAvailable();

	if (preference == BackendPreference::Scxvid) {
		if (xvid_available) {
			Select(scxvid_api);
			return true;
		}
		if (wwxd_available) {
			Select(wwxd_api);
			return true;
		}
	}
	else if (preference == BackendPreference::Wwxd) {
		if (wwxd_available) {
			Select(wwxd_api);
			return true;
		}
		if (xvid_available) {
			Select(scxvid_api);
			return true;
		}
	}
	else {
		if (xvid_available) {
			Select(scxvid_api);
			return true;
		}
		if (wwxd_available) {
			Select(wwxd_api);
			return true;
		}
	}

	return false;
}

} // namespace scenechange
