#include "secondary_subtitle_decoder.h"

#include "include/aegisub/subtitles_provider.h"
#include "subtitle_overlay_blend.h"
#include "ragbag/subtitle_plugin_api.h"

#include <libaegisub/exception.h>
#include <libaegisub/fs.h>
#include <libaegisub/log.h>
#include <libaegisub/native_library.h>
#include <libaegisub/string_utils.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
constexpr char kLogTag[] = "subtitle/secondary/decoder";
constexpr char kPluginDirectory[] = "runtimes";
constexpr char kInitSymbol[] = "ragbag_subtitle_decoder_init_v1";

using InitFunction = int32_t (*)(RagbagSubtitleHostApiV1 const*, RagbagSubtitlePluginApiV1*);

struct LoadedPlugin {
	agi::native::Library library;
	RagbagSubtitlePluginApiV1 api = {};
	std::string path;
};

struct DecoderRecord {
	std::shared_ptr<LoadedPlugin> plugin;
	std::string decoder_id;
	std::string display_name;
	std::string debug_name;
	std::vector<std::string> codec_ids;
};

std::vector<std::shared_ptr<DecoderRecord>> g_decoders;
std::once_flag g_discover_once;

void HostLog(void*, RagbagSubtitleLogLevelV1 level, char const *message) {
	auto const *text = message ? message : "";
	switch (level) {
		case RAGBAG_SUBTITLE_LOG_ERROR: LOG_E(kLogTag) << text; break;
		case RAGBAG_SUBTITLE_LOG_WARNING: LOG_W(kLogTag) << text; break;
		case RAGBAG_SUBTITLE_LOG_INFO: LOG_I(kLogTag) << text; break;
		default: LOG_D(kLogTag) << text; break;
	}
}

std::string CopyString(char const *value) {
	return value ? value : "";
}

std::vector<std::string> ParseCodecIds(char const *value) {
	std::vector<std::string> codecs;
	if (!value)
		return codecs;
	agi::util::strings::for_each_split_any(value, ";", [&](std::string_view part) {
		auto codec = agi::util::strings::trim_copy(part);
		if (!codec.empty())
			codecs.emplace_back(std::move(codec));
	});
	return codecs;
}

bool IsApiUsable(RagbagSubtitlePluginApiV1 const& api) {
	return api.struct_size >= sizeof(RagbagSubtitlePluginApiV1)
		&& api.api_version == RAGBAG_SUBTITLE_DECODER_API_VERSION
		&& api.get_decoder_count
		&& api.get_decoder_descriptor
		&& api.create_decoder
		&& api.destroy_decoder
		&& api.get_last_error
		&& api.begin_stream
		&& api.push_packet
		&& api.end_stream
		&& api.render_at;
}

void DiscoverPlugins() {
	for (auto const& path : agi::native::EnumerateLibrariesInExecutableRelativeDirectory(kPluginDirectory)) {
		try {
			auto plugin = std::make_shared<LoadedPlugin>();
			plugin->path = path;
			plugin->library = agi::native::Library::Load(path);
			auto init = plugin->library.ResolveSymbol<InitFunction>(kInitSymbol);

			RagbagSubtitleHostApiV1 host = {};
			host.struct_size = sizeof(host);
			host.api_version = RAGBAG_SUBTITLE_DECODER_API_VERSION;
			host.log = HostLog;

			plugin->api.struct_size = sizeof(plugin->api);
			auto status = init(&host, &plugin->api);
			if (status != RAGBAG_SUBTITLE_STATUS_OK || !IsApiUsable(plugin->api))
				throw agi::EnvironmentError("Secondary subtitle decoder exported an incompatible v1 API.");

			for (uint32_t index = 0; index < plugin->api.get_decoder_count(); ++index) {
				auto const *descriptor = plugin->api.get_decoder_descriptor(index);
				if (!descriptor || descriptor->struct_size < sizeof(RagbagSubtitleDecoderDescriptorV1)
					|| !descriptor->decoder_id || !*descriptor->decoder_id)
					continue;

				auto decoder = std::make_shared<DecoderRecord>();
				decoder->plugin = plugin;
				decoder->decoder_id = descriptor->decoder_id;
				decoder->display_name = CopyString(descriptor->display_name);
				decoder->debug_name = CopyString(descriptor->debug_name);
				decoder->codec_ids = ParseCodecIds(descriptor->codec_ids_semicolon);
				if (!decoder->codec_ids.empty())
					g_decoders.emplace_back(std::move(decoder));
			}

			LOG_I(kLogTag) << "Loaded secondary subtitle decoder plugin from " << agi::fs::PathToString(path);
		}
		catch (agi::Exception const& err) {
			LOG_D(kLogTag) << "Ignoring non-decoder runtime " << agi::fs::PathToString(path) << ": " << err.GetMessage();
		}
		catch (std::exception const& err) {
			LOG_D(kLogTag) << "Ignoring non-decoder runtime " << agi::fs::PathToString(path) << ": " << err.what();
		}
		catch (...) {
			LOG_D(kLogTag) << "Ignoring non-decoder runtime " << agi::fs::PathToString(path) << ": unknown error";
		}
	}
}

std::vector<std::shared_ptr<DecoderRecord>> const& Decoders() {
	std::call_once(g_discover_once, DiscoverPlugins);
	return g_decoders;
}

std::shared_ptr<DecoderRecord> FindDecoder(std::string const& codec_id) {
	for (auto const& decoder : Decoders()) {
		if (std::find(decoder->codec_ids.begin(), decoder->codec_ids.end(), codec_id) != decoder->codec_ids.end())
			return decoder;
	}
	return {};
}

std::string DecoderName(DecoderRecord const& decoder) {
	if (!decoder.debug_name.empty()) return decoder.debug_name;
	if (!decoder.display_name.empty()) return decoder.display_name;
	return decoder.decoder_id;
}

class SecondaryBitmapSubtitlesProvider final : public SubtitlesProvider {
	std::shared_ptr<DecoderRecord> record;
	std::shared_ptr<const SecondarySubtitlePacketStream> stream;
	RagbagSubtitleDecoderV1 *decoder = nullptr;
	bool stream_ready = false;
	std::exception_ptr stream_error;

	std::string LastError() const {
		if (!record || !record->plugin || !record->plugin->api.get_last_error)
			return {};
		auto const *message = record->plugin->api.get_last_error(decoder);
		return message ? message : "";
	}

	[[noreturn]] void ThrowStatus(char const *action, int32_t status) const {
		auto message = LastError();
		if (message.empty())
			message = std::string(action) + " failed with status " + std::to_string(status);
		else
			message = std::string(action) + " failed: " + message;
		throw agi::InternalError(message);
	}

	void DestroyDecoder() noexcept {
		if (decoder && record && record->plugin && record->plugin->api.destroy_decoder)
			record->plugin->api.destroy_decoder(decoder);
		decoder = nullptr;
		stream_ready = false;
	}

	void EnsureStream() {
		if (stream_ready)
			return;
		if (stream_error)
			std::rethrow_exception(stream_error);
		if (!stream)
			throw agi::InternalError("Secondary bitmap subtitle packet stream is missing.");

		try {
			auto& api = record->plugin->api;
			if (!decoder) {
				auto status = api.create_decoder(record->decoder_id.c_str(), &decoder);
				if (status != RAGBAG_SUBTITLE_STATUS_OK)
					ThrowStatus("Creating secondary subtitle decoder", status);
			}

			RagbagSubtitleStreamInfoV1 info = {};
			info.struct_size = sizeof(info);
			info.codec_id = stream->codec_id.c_str();
			info.codec_private = reinterpret_cast<uint8_t const*>(stream->codec_private.data());
			info.codec_private_size = stream->codec_private.size();
			info.fallback_canvas_width = stream->fallback_canvas_width;
			info.fallback_canvas_height = stream->fallback_canvas_height;
			auto status = api.begin_stream(decoder, &info);
			if (status != RAGBAG_SUBTITLE_STATUS_OK)
				ThrowStatus("Opening secondary subtitle packet stream", status);

			for (auto const& source_packet : stream->packets) {
				RagbagSubtitlePacketV1 packet = {};
				packet.struct_size = sizeof(packet);
				packet.pts_ns = source_packet.pts_ns;
				packet.dts_ns = source_packet.dts_ns;
				packet.duration_ns = source_packet.duration_ns;
				packet.flags = source_packet.flags;
				packet.payload = reinterpret_cast<uint8_t const*>(source_packet.payload.data());
				packet.payload_size = source_packet.payload.size();
				status = api.push_packet(decoder, &packet);
				if (status != RAGBAG_SUBTITLE_STATUS_OK)
					ThrowStatus("Decoding secondary subtitle packet", status);
			}

			status = api.end_stream(decoder);
			if (status != RAGBAG_SUBTITLE_STATUS_OK)
				ThrowStatus("Finishing secondary subtitle packet stream", status);
			stream_ready = true;
		}
		catch (...) {
			stream_error = std::current_exception();
			DestroyDecoder();
			std::rethrow_exception(stream_error);
		}
	}

	void LoadSubtitles(const char*, size_t) override { }

public:
	SecondaryBitmapSubtitlesProvider(std::shared_ptr<DecoderRecord> record, std::shared_ptr<const SecondarySubtitlePacketStream> stream)
	: record(std::move(record)), stream(std::move(stream)) { }

	~SecondaryBitmapSubtitlesProvider() override { DestroyDecoder(); }

	std::string GetDebugName() const override { return DecoderName(*record); }
	SubtitleRenderMode GetRenderMode() const override { return SubtitleRenderMode::PremultipliedOverlay; }
	bool RenderOverlayClearsTarget() const override { return true; }

	bool RenderOverlay(SourceFrame const&, SubtitleOverlay& overlay, double time) override {
		EnsureStream();
		if (!overlay.IsValid() || overlay.pixel_format != SubtitleOverlayPixelFormat::Bgra8 || !overlay.premultiplied_alpha)
			return false;

		auto *data = overlay.planes[0].data;
		auto stride = static_cast<int32_t>(overlay.planes[0].stride);
		if (overlay.flipped && data && stride > 0) {
			data += static_cast<ptrdiff_t>(overlay.height - 1) * stride;
			stride = -stride;
		}

		RagbagSubtitleRenderTargetV1 target = {};
		target.struct_size = sizeof(target);
		target.plane0 = data;
		target.stride0 = stride;
		target.width = overlay.width;
		target.height = overlay.height;

		RagbagSubtitleRenderResultV1 result = {};
		result.struct_size = sizeof(result);
		double const bounded = std::clamp(time, -9223372036.0, 9223372036.0);
		auto const time_ns = static_cast<int64_t>(std::llround(bounded * 1000000000.0));
		auto status = record->plugin->api.render_at(decoder, time_ns, &target, &result);
		if (status != RAGBAG_SUBTITLE_STATUS_OK)
			ThrowStatus("Rendering secondary subtitle", status);

		overlay.composition_mode = SubtitleOverlayCompositionMode::PremultipliedAlpha;
		overlay.has_visible_content = result.has_visible_content != 0;
		overlay.dirty_rects = nullptr;
		overlay.dirty_rect_count = 0;
		return true;
	}

	void DrawSubtitles(VideoFrame& dst, double time) override {
		SubtitleOverlayStorage storage;
		storage.Reset(static_cast<int>(dst.width), static_cast<int>(dst.height), dst.flipped, false);
		auto overlay = storage.MakeView(true);
		auto source = MakeSourceFrameView(dst);
		if (RenderOverlay(source, overlay, time) && overlay.has_visible_content)
			CompositePremultipliedBgraOverlayOntoVideoFrame(dst, overlay);
	}
};
}

namespace secondary_subtitle_decoder {

bool IsAvailable(std::string const& codec_id) {
	return static_cast<bool>(FindDecoder(codec_id));
}

std::unique_ptr<SubtitlesProvider> Create(std::shared_ptr<const SecondarySubtitlePacketStream> stream) {
	if (!stream)
		throw agi::InternalError("Secondary bitmap subtitle packet stream is missing.");
	auto decoder = FindDecoder(stream->codec_id);
	if (!decoder)
		throw agi::EnvironmentError("No secondary subtitle decoder is available for codec " + stream->codec_id + ".");
	return std::make_unique<SecondaryBitmapSubtitlesProvider>(std::move(decoder), std::move(stream));
}

}
