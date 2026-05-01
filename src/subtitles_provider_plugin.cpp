// Copyright (c) 2026, MIRIMIRIM

#include "subtitles_provider_plugin.h"

#include "include/aegisub/subtitles_provider.h"
#include "ragbag/subtitle_plugin_api.h"
#include "source_frame.h"
#include "subtitle_overlay_blend.h"

#include <libaegisub/exception.h>
#include <libaegisub/fs.h>
#include <libaegisub/log.h>
#include <libaegisub/native_library.h>
#include <libaegisub/string_utils.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
constexpr char kLogTag[] = "subtitle/provider/plugin";
constexpr char kPluginDirectory[] = "runtimes";
constexpr char kInitSymbol[] = "ragbag_subtitle_plugin_init_v0";

using InitFunction = int32_t (*)(RagbagSubtitleHostApiV0 const*, RagbagSubtitlePluginApiV0*);

struct LoadedPlugin {
	agi::native::Library library;
	RagbagSubtitlePluginApiV0 api = { };
	std::string path;
};

struct ProviderRecord {
	std::shared_ptr<LoadedPlugin> plugin;
	std::string provider_id;
	std::string display_name;
	std::string debug_name;
	std::string extensions;
	std::string codecs;
	uint32_t capabilities = 0;
};

std::vector<std::shared_ptr<ProviderRecord>> g_providers;
std::once_flag g_discover_once;

void HostLog(void*, RagbagSubtitleLogLevelV0 level, char const *message) {
	auto const *text = message ? message : "";
	switch (level) {
		case RAGBAG_SUBTITLE_LOG_ERROR:
			LOG_E(kLogTag) << text;
			break;
		case RAGBAG_SUBTITLE_LOG_WARNING:
			LOG_W(kLogTag) << text;
			break;
		case RAGBAG_SUBTITLE_LOG_INFO:
			LOG_I(kLogTag) << text;
			break;
		default:
			LOG_D(kLogTag) << text;
			break;
	}
}

bool HasCapabilities(uint32_t capabilities, uint32_t required) {
	return (capabilities & required) == required;
}

bool IsUsableProviderDescriptor(RagbagSubtitleProviderDescriptorV0 const& descriptor) {
	uint32_t const required =
		RAGBAG_SUBTITLE_PROVIDER_CAP_FILE_INPUT |
		RAGBAG_SUBTITLE_PROVIDER_CAP_BITMAP_OUTPUT |
		RAGBAG_SUBTITLE_PROVIDER_CAP_PREMULTIPLIED_BGRA8;
	return descriptor.struct_size >= sizeof(RagbagSubtitleProviderDescriptorV0)
		&& descriptor.provider_id && *descriptor.provider_id
		&& HasCapabilities(descriptor.capabilities, required);
}

std::string CopyString(char const *value) {
	return value ? value : "";
}

std::string ProviderName(ProviderRecord const& provider) {
	if (!provider.debug_name.empty())
		return provider.debug_name;
	if (!provider.display_name.empty())
		return provider.display_name;
	return provider.provider_id;
}

std::string NormalizeExtension(std::string_view extension) {
	std::string value = agi::util::strings::trim_copy(extension);
	if (value.empty())
		return value;
	if (agi::util::strings::starts_with(value, "*."))
		value.erase(0, 1);
	else if (value.front() == '*')
		value.erase(value.begin());
	if (!value.empty() && value.front() != '.')
		value.insert(value.begin(), '.');
	agi::util::strings::to_lower_inplace(value);
	return value;
}

std::vector<std::string> ProviderExtensions(ProviderRecord const& provider) {
	std::vector<std::string> extensions;
	agi::util::strings::for_each_split_any(provider.extensions, ";, ", [&](std::string_view part) {
		auto extension = NormalizeExtension(part);
		if (!extension.empty())
			extensions.push_back(std::move(extension));
	});
	return extensions;
}

bool SupportsExtension(ProviderRecord const& provider, agi::fs::path const& filename) {
	auto extension = NormalizeExtension(agi::fs::PathToString(filename.extension()));
	if (extension.empty())
		return false;
	for (auto const& supported : ProviderExtensions(provider)) {
		if (agi::util::strings::iequals(supported, extension))
			return true;
	}
	return false;
}

bool IsApiUsable(RagbagSubtitlePluginApiV0 const& api) {
	return api.struct_size >= sizeof(RagbagSubtitlePluginApiV0)
		&& api.api_version == RAGBAG_SUBTITLE_PLUGIN_API_VERSION
		&& api.get_provider_count
		&& api.get_provider_descriptor
		&& api.create_provider
		&& api.destroy_provider
		&& api.get_last_error
		&& api.open_file
		&& api.render_overlay;
}

void DiscoverPlugins() {
	for (auto const& path : agi::native::EnumerateLibrariesInExecutableRelativeDirectory(kPluginDirectory)) {
		try {
			auto plugin = std::make_shared<LoadedPlugin>();
			plugin->path = path;
			plugin->library = agi::native::Library::Load(path);
			auto init = plugin->library.ResolveSymbol<InitFunction>(kInitSymbol);

			RagbagSubtitleHostApiV0 host = { };
			host.struct_size = sizeof(host);
			host.api_version = RAGBAG_SUBTITLE_PLUGIN_API_VERSION;
			host.log = HostLog;

			plugin->api.struct_size = sizeof(plugin->api);
			auto status = init(&host, &plugin->api);
			if (status != RAGBAG_SUBTITLE_STATUS_OK)
				throw agi::EnvironmentError("Subtitle plugin init failed with status " + std::to_string(status));
			if (!IsApiUsable(plugin->api))
				throw agi::EnvironmentError("Subtitle plugin exported an incompatible v0 API table.");

			auto count = plugin->api.get_provider_count();
			for (uint32_t i = 0; i < count; ++i) {
				auto const *descriptor = plugin->api.get_provider_descriptor(i);
				if (!descriptor || !IsUsableProviderDescriptor(*descriptor))
					continue;

				auto provider = std::make_shared<ProviderRecord>();
				provider->plugin = plugin;
				provider->provider_id = CopyString(descriptor->provider_id);
				provider->display_name = CopyString(descriptor->display_name);
				provider->debug_name = CopyString(descriptor->debug_name);
				provider->extensions = CopyString(descriptor->extensions_semicolon);
				provider->codecs = CopyString(descriptor->codec_names_semicolon);
				provider->capabilities = descriptor->capabilities;
				g_providers.push_back(std::move(provider));
			}

			LOG_I(kLogTag) << "Loaded subtitle plugin from " << path;
		}
		catch (agi::Exception const& err) {
			LOG_W(kLogTag) << "Subtitle plugin unavailable: " << path << ": " << err.GetMessage();
		}
		catch (std::exception const& err) {
			LOG_W(kLogTag) << "Subtitle plugin unavailable: " << path << ": " << err.what();
		}
		catch (...) {
			LOG_W(kLogTag) << "Subtitle plugin unavailable: " << path << ": unknown error";
		}
	}
}

std::vector<std::shared_ptr<ProviderRecord>> const& Providers() {
	std::call_once(g_discover_once, DiscoverPlugins);
	return g_providers;
}

std::shared_ptr<ProviderRecord> FindProvider(std::string const& provider_name, agi::fs::path const& filename) {
	for (auto const& provider : Providers()) {
		if (provider_name != ProviderName(*provider))
			continue;
		if (!filename.empty() && !SupportsExtension(*provider, filename))
			continue;
		return provider;
	}
	return nullptr;
}

RagbagSubtitleVideoInfoV0 MakeVideoInfo(SourceFrame const *source) {
	RagbagSubtitleVideoInfoV0 video = { };
	video.struct_size = sizeof(video);
	if (!source || !source->IsValid())
		return video;

	video.width = source->width;
	video.height = source->height;
	video.storage_width = source->geometry.storage_width > 0 ? source->geometry.storage_width : source->width;
	video.storage_height = source->geometry.storage_height > 0 ? source->geometry.storage_height : source->height;
	return video;
}

std::string ProviderLastError(ProviderRecord const& record, RagbagSubtitleProviderV0 *provider) {
	if (!record.plugin || !record.plugin->api.get_last_error || !provider)
		return {};
	auto const *message = record.plugin->api.get_last_error(provider);
	return message ? message : "";
}

class PluginSubtitlesProvider final : public SubtitlesProvider {
	std::shared_ptr<ProviderRecord> record;
	RagbagSubtitleProviderV0 *provider = nullptr;
	agi::fs::path filename;
	bool opened = false;
	std::vector<RagbagSubtitleDirtyRectV0> plugin_dirty_rects;
	std::vector<SubtitleOverlayDirtyRect> overlay_dirty_rects;

	void ThrowStatus(char const *action, int32_t status) const {
		auto message = ProviderLastError(*record, provider);
		if (message.empty())
			message = std::string(action) + " failed with status " + std::to_string(status);
		else
			message = std::string(action) + " failed: " + message;
		throw agi::InternalError(message);
	}

	void DestroyProvider() noexcept {
		if (provider && record && record->plugin && record->plugin->api.destroy_provider)
			record->plugin->api.destroy_provider(provider);
		provider = nullptr;
		opened = false;
	}

	void EnsureProvider() {
		if (provider)
			return;
		auto status = record->plugin->api.create_provider(record->provider_id.c_str(), &provider);
		if (status != RAGBAG_SUBTITLE_STATUS_OK)
			ThrowStatus("Creating subtitle plugin provider", status);
	}

	void OpenIfNeeded(SourceFrame const *source) {
		if (opened)
			return;
		EnsureProvider();

		auto path = agi::fs::PathToString(filename);
		auto video = MakeVideoInfo(source);
		auto status = record->plugin->api.open_file(provider, path.c_str(), &video);
		if (status != RAGBAG_SUBTITLE_STATUS_OK)
			ThrowStatus("Opening subtitle file", status);
		opened = true;
	}

	void LoadSubtitles(const char *, size_t) override {
		// Plugin providers decode directly from external files;
		// the ASS buffer path is not used.  Just mark the file
		// as needing re-open on the next render — the provider
		// instance is reused because the source file never
		// changes during this provider's lifetime.
		opened = false;
	}

public:
	PluginSubtitlesProvider(std::shared_ptr<ProviderRecord> record, agi::fs::path filename)
	: record(std::move(record))
	, filename(std::move(filename)) {
		if (!this->record || !this->record->plugin)
			throw agi::InternalError("Subtitle plugin provider record is invalid.");
		// Provider instance is created lazily on first RenderOverlay.
	}

	~PluginSubtitlesProvider() override {
		DestroyProvider();
	}

	std::string GetDebugName() const override {
		return ProviderName(*record);
	}

	SubtitleRenderMode GetRenderMode() const override {
		return SubtitleRenderMode::PremultipliedOverlay;
	}

	bool RenderOverlayClearsTarget() const override {
		return true;
	}

	bool SupportsOverlayDirtyRects() const override {
		return HasCapabilities(record->capabilities, RAGBAG_SUBTITLE_PROVIDER_CAP_DIRTY_RECTS);
	}

	bool RenderOverlay(SourceFrame const& source, SubtitleOverlay& overlay, double time) override {
		OpenIfNeeded(&source);
		if (!overlay.IsValid() || overlay.pixel_format != SubtitleOverlayPixelFormat::Bgra8 || !overlay.premultiplied_alpha)
			return false;

		overlay.composition_mode = SubtitleOverlayCompositionMode::PremultipliedAlpha;
		overlay.has_visible_content = false;
		overlay_dirty_rects.clear();

		uint8_t *data = overlay.planes[0].data;
		int32_t stride = static_cast<int32_t>(overlay.planes[0].stride);
		if (overlay.flipped && data && stride > 0) {
			data += static_cast<ptrdiff_t>(overlay.height - 1) * stride;
			stride = -stride;
		}

		if (SupportsOverlayDirtyRects())
			plugin_dirty_rects.resize(256);
		else
			plugin_dirty_rects.clear();

		RagbagSubtitleRenderRequestV0 request = { };
		request.struct_size = sizeof(request);
		request.time_seconds = time;
		request.video = MakeVideoInfo(&source);
		request.flags = RAGBAG_SUBTITLE_RENDER_CLEAR_TARGET;

		RagbagSubtitleOverlayTargetV0 target = { };
		target.struct_size = sizeof(target);
		target.pixel_format = RAGBAG_SUBTITLE_PIXEL_FORMAT_BGRA8;
		target.alpha_mode = RAGBAG_SUBTITLE_ALPHA_PREMULTIPLIED;
		target.plane0 = data;
		target.stride0 = stride;
		target.width = overlay.width;
		target.height = overlay.height;
		target.dirty_rects = plugin_dirty_rects.empty() ? nullptr : plugin_dirty_rects.data();
		target.dirty_rect_capacity = static_cast<uint32_t>(plugin_dirty_rects.size());

		auto status = record->plugin->api.render_overlay(provider, &request, &target);
		if (status != RAGBAG_SUBTITLE_STATUS_OK)
			ThrowStatus("Rendering subtitle overlay", status);

		overlay.has_visible_content = target.has_visible_content != 0;
		if (SupportsOverlayDirtyRects() && target.dirty_rect_count > 0) {
			auto count = std::min<uint32_t>(target.dirty_rect_count, static_cast<uint32_t>(plugin_dirty_rects.size()));
			overlay_dirty_rects.reserve(count);
			for (uint32_t i = 0; i < count; ++i) {
				auto const& rect = plugin_dirty_rects[i];
				overlay_dirty_rects.push_back({ rect.x, rect.y, rect.width, rect.height });
			}
		}
		overlay.dirty_rects = overlay_dirty_rects.empty() ? nullptr : overlay_dirty_rects.data();
		overlay.dirty_rect_count = static_cast<int>(overlay_dirty_rects.size());
		return true;
	}

	void DrawSubtitles(VideoFrame &dst, double time) override {
		SubtitleOverlayStorage storage;
		storage.Reset(static_cast<int>(dst.width), static_cast<int>(dst.height), dst.flipped, false);
		auto overlay = storage.MakeView(true);
		auto source = MakeSourceFrameView(dst);
		if (RenderOverlay(source, overlay, time) && overlay.has_visible_content)
			CompositePremultipliedBgraOverlayOntoVideoFrame(dst, overlay);
	}
};
}

namespace subtitle_plugin {

std::vector<std::string> List() {
	std::vector<std::string> names;
	for (auto const& provider : Providers())
		names.push_back(ProviderName(*provider));
	return names;
}

bool HasExternalFileProviderFor(agi::fs::path const& filename) {
	for (auto const& provider : Providers()) {
		if (SupportsExtension(*provider, filename))
			return true;
	}
	return false;
}

std::vector<std::string> GetExternalFileProviderWildcards() {
	std::set<std::string> patterns;
	for (auto const& provider : Providers()) {
		for (auto const& extension : ProviderExtensions(*provider))
			patterns.insert("*" + extension);
	}
	return { patterns.begin(), patterns.end() };
}

std::unique_ptr<SubtitlesProvider> Create(std::string const& provider_name, SubtitleRenderEnvironment const& env) {
	// The caller (SubtitlesProviderFactory::GetProvider) already filters
	// factories by require_external_file_provider, so we are only invoked
	// when a plugin provider is genuinely needed.

	auto provider = FindProvider(provider_name, env.external_subtitle_file);
	if (!provider)
		return nullptr;

	return std::make_unique<PluginSubtitlesProvider>(std::move(provider), env.external_subtitle_file);
}

}
