#include "skia_audio_presenter.h"

#include "../../skia_runtime/skia_surface_provider.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#ifdef HAVE_OPENGL_GL_H
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

#include <include/core/SkCanvas.h>
#include <include/core/SkColor.h>
#include <include/core/SkColorSpace.h>
#include <include/core/SkData.h>
#include <include/core/SkFont.h>
#include <include/core/SkImage.h>
#include <include/core/SkImageInfo.h>
#include <include/core/SkPaint.h>
#include <include/core/SkRect.h>
#include <include/core/SkSamplingOptions.h>
#include <include/core/SkString.h>
#include <include/core/SkSurface.h>
#include <include/effects/SkRuntimeEffect.h>
#include <include/gpu/ganesh/GrDirectContext.h>
#include <include/gpu/ganesh/SkImageGanesh.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <iomanip>
#include <limits>
#include <optional>
#include <queue>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aegisub::skia::audio {
namespace {

constexpr int kWaveformMaskHeight = 256;
constexpr float kSpectrumPowerEncodingMaximum = 8.f;
constexpr std::size_t kDefaultContentCacheBudget = 32 * 1024 * 1024;
constexpr std::size_t kGaneshResourceCacheBudget = 64 * 1024 * 1024;

SkiaGlFailureInjection DeviceFailureInjection(FailureInjection injection) noexcept {
	switch (injection) {
		case FailureInjection::ContextInitialization:
			return SkiaGlFailureInjection::ContextInitialization;
		case FailureInjection::FlushSubmit:
			return SkiaGlFailureInjection::FlushSubmit;
		case FailureInjection::None:
		case FailureInjection::FrameBegin:
		case FailureInjection::Unsupported:
			return SkiaGlFailureInjection::None;
	}
	return SkiaGlFailureInjection::None;
}

std::string ReadGlString(GLenum name) {
	auto const *value = glGetString(name);
	return value ? reinterpret_cast<char const *>(value) : std::string{};
}

struct ContentTileKeyHash {
	std::size_t operator()(ContentTileKey const& key) const noexcept {
		auto combine = [](std::size_t seed, std::uint64_t value) {
			return seed ^ (static_cast<std::size_t>(value) + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2));
		};
		std::size_t hash = 0;
		hash = combine(hash, key.generation.provider);
		hash = combine(hash, key.generation.analysis);
		hash = combine(hash, static_cast<std::uint64_t>(key.kind));
		hash = combine(hash, key.tile_index);
		hash = combine(hash, key.column_count);
		return combine(hash, key.spectrum_bin_count);
	}
};

sk_sp<SkImage> UploadTexture(
	GrDirectContext *context,
	SkImageInfo const& info,
	void const *pixels,
	std::size_t byte_count,
	std::size_t row_bytes) {
	if (!context || !pixels || byte_count == 0)
		return nullptr;
	auto data = SkData::MakeWithCopy(pixels, byte_count);
	if (!data)
		return nullptr;
	auto raster = SkImages::RasterFromData(info, std::move(data), row_bytes);
	if (!raster)
		return nullptr;
	return SkImages::TextureFromImage(context, raster);
}

int WaveformMaskY(float value) noexcept {
	value = std::clamp(value, -1.f, 1.f);
	auto const coordinate = (1.f - value) * 0.5f * (kWaveformMaskHeight - 1);
	return std::clamp(static_cast<int>(std::lround(coordinate)), 0, kWaveformMaskHeight - 1);
}

std::vector<std::uint8_t> BuildWaveformMask(ContentTile const& tile, bool average) {
	auto const width = static_cast<std::size_t>(tile.key.column_count);
	std::vector<std::uint8_t> mask(width * kWaveformMaskHeight);
	for (std::size_t x = 0; x < width; ++x) {
		auto const& column = tile.waveform[x];
		auto low = average ? column.average_min : column.peak_min;
		auto high = average ? column.average_max : column.peak_max;
		if (low > high)
			std::swap(low, high);
		auto const top = WaveformMaskY(high);
		auto const bottom = WaveformMaskY(low);
		for (int y = top; y <= bottom; ++y)
			mask[static_cast<std::size_t>(y) * width + x] = 255;
	}
	return mask;
}

std::vector<std::uint8_t> EncodeSpectrumPower(ContentTile const& tile, SpectrumBandPlan const& plan) {
	auto const width = static_cast<std::size_t>(tile.key.column_count);
	auto const height = static_cast<std::size_t>(plan.output_height);
	std::vector<std::uint8_t> pixels(width * height * 4);
	for (std::size_t x = 0; x < width; ++x) {
		for (std::size_t y = 0; y < height; ++y) {
			auto const& band = plan.bands[y];
			float power = 0.f;
			if (plan.interpolated) {
				auto const lower = tile.spectrum_power[
					x * tile.key.spectrum_bin_count + band.first];
				auto const upper = tile.spectrum_power[
					x * tile.key.spectrum_bin_count + band.last];
				power = (1.f - band.fraction) * lower + band.fraction * upper;
			}
			else {
				for (auto bin = band.first; bin <= band.last; ++bin)
					power = std::max(power, tile.spectrum_power[
						x * tile.key.spectrum_bin_count + bin]);
			}
			power = std::clamp(power, 0.f, kSpectrumPowerEncodingMaximum);
			auto const encoded = static_cast<std::uint16_t>(std::lround(
				power / kSpectrumPowerEncodingMaximum * std::numeric_limits<std::uint16_t>::max()));
			auto const image_y = height - 1 - y;
			auto *pixel = pixels.data() + (image_y * width + x) * 4;
			pixel[0] = static_cast<std::uint8_t>(encoded >> 8);
			pixel[1] = static_cast<std::uint8_t>(encoded & 0xFF);
			pixel[2] = 0;
			pixel[3] = 255;
		}
	}
	return pixels;
}

std::vector<std::uint8_t> EncodePalette(SpectrumPalette const& palette) {
	std::vector<std::uint8_t> pixels(palette.colors.size() * 4);
	for (std::size_t i = 0; i < palette.colors.size(); ++i) {
		auto const color = palette.colors[i];
		pixels[i * 4 + 0] = static_cast<std::uint8_t>((color >> 16) & 0xFF);
		pixels[i * 4 + 1] = static_cast<std::uint8_t>((color >> 8) & 0xFF);
		pixels[i * 4 + 2] = static_cast<std::uint8_t>(color & 0xFF);
		pixels[i * 4 + 3] = static_cast<std::uint8_t>((color >> 24) & 0xFF);
	}
	return pixels;
}

std::string ValidateContentFrame(FrameTarget const& target, ContentFrame const& frame) {
	if (!frame.generation.provider || !frame.generation.analysis)
		return "the Audio content generation is zero";
	if (!std::isfinite(frame.x)
		|| !std::isfinite(frame.y)
		|| !std::isfinite(frame.width)
		|| !std::isfinite(frame.height)
		|| !std::isfinite(frame.first_column_offset)
		|| frame.x < 0.f
		|| frame.y < 0.f
		|| frame.width <= 0.f
		|| frame.height <= 0.f
		|| frame.first_column_offset > 0.f
		|| frame.first_column_offset <= -1.f)
		return "the Audio content bounds are invalid";
	if (frame.x > target.width - frame.width || frame.y > target.height - frame.height)
		return "the Audio content bounds exceed the frame target";
	if (!std::isfinite(frame.amplitude) || frame.amplitude < 0.f)
		return "the Audio content amplitude is invalid";
	if (frame.kind == ContentKind::Spectrum
		&& (!frame.spectrum_palette || !frame.spectrum_palette->revision
			|| !frame.spectrum_band_plan || !frame.spectrum_band_plan->IsValid()
			|| frame.spectrum_band_plan->output_height != static_cast<int>(std::lround(frame.height)))) {
		return "the spectrum palette, band plan, or its revision is missing";
	}
	for (auto const& style : frame.styles) {
		if (!std::isfinite(style.x) || !std::isfinite(style.width)
			|| style.width <= 0.f || style.x < frame.x
			|| style.x > frame.x + frame.width - style.width)
			return "an Audio rendering style span is invalid";
		if (frame.kind == ContentKind::Spectrum
			&& (!style.spectrum_palette || !style.spectrum_palette->revision))
			return "an Audio spectrum rendering style palette is missing";
	}
	return {};
}

std::string FormatTimelineLabel(std::int64_t milliseconds) {
	milliseconds = std::max<std::int64_t>(0, milliseconds);
	auto const hours = milliseconds / 3600000;
	milliseconds %= 3600000;
	auto const minutes = milliseconds / 60000;
	milliseconds %= 60000;
	auto const seconds = milliseconds / 1000;
	auto const centiseconds = (milliseconds % 1000) / 10;
	std::ostringstream out;
	if (hours > 0)
		out << hours << ':' << std::setfill('0') << std::setw(2) << minutes << ':'
			<< std::setw(2) << seconds;
	else if (minutes > 0)
		out << minutes << ':' << std::setfill('0') << std::setw(2) << seconds;
	else
		out << seconds << '.' << std::setfill('0') << std::setw(2) << centiseconds;
	return out.str();
}

void DrawAudioFrameLayers(
	SkCanvas *canvas,
	FrameTarget const& target,
	ContentFrame const& frame) {
	if (!canvas)
		return;

	SkPaint paint;
	paint.setAntiAlias(false);

	// Timeline is deliberately drawn after content in the same canvas submit.
	// Its scroll origin is expressed in device pixels, matching FrameViewport.
	if (frame.timeline && frame.timeline->height > 0) {
		auto const timeline_y = static_cast<float>(frame.timeline->y);
		auto const timeline_height = static_cast<float>(frame.timeline->height);
		paint.setColor(static_cast<SkColor>(frame.timeline->background_color));
		canvas->drawRect(SkRect::MakeXYWH(frame.x, timeline_y, frame.width, timeline_height), paint);

		paint.setColor(static_cast<SkColor>(frame.timeline->foreground_color));
		paint.setStrokeWidth(1.f);
		auto const ms_per_pixel = frame.timeline->milliseconds_per_pixel;
		if (std::isfinite(ms_per_pixel) && ms_per_pixel > 0.0 && frame.timeline->duration_ms > 0) {
			auto const pixels_per_second = 1000.0 / ms_per_pixel;
			int tick_ms = 1000;
			if (pixels_per_second > 3000.0) tick_ms = 1;
			else if (pixels_per_second > 300.0) tick_ms = 10;
			else if (pixels_per_second > 30.0) tick_ms = 100;
			else if (pixels_per_second > 3.0) tick_ms = 1000;
			else if (pixels_per_second > 1.0 / 3.0) tick_ms = 10000;
			else if (pixels_per_second > 1.0 / 9.0) tick_ms = 60000;
			else if (pixels_per_second > 1.0 / 90.0) tick_ms = 600000;
			else tick_ms = 3600000;
			auto const visible_start = std::max<std::int64_t>(
				0, static_cast<std::int64_t>(std::floor(frame.timeline->scroll_left * ms_per_pixel)));
			auto const visible_end = std::min<std::int64_t>(
				frame.timeline->duration_ms,
				static_cast<std::int64_t>(std::ceil((frame.timeline->scroll_left + frame.width) * ms_per_pixel)));
			auto ms = visible_start / tick_ms * tick_ms;
			if (ms < visible_start) ms += tick_ms;
			SkFont timeline_font;
			timeline_font.setSize(11.f);
			float last_label_right = frame.x - 1.f;
			for (; ms <= visible_end; ms += tick_ms) {
				auto const x = frame.x + static_cast<float>(ms / ms_per_pixel)
					- static_cast<float>(frame.timeline->scroll_left);
				if (x < frame.x - 1.f || x > frame.x + frame.width + 1.f)
					continue;
				auto const major = ((ms / tick_ms) % 10) == 0;
				canvas->drawLine(x, timeline_y + timeline_height - (major ? 7.f : 4.f),
					x, timeline_y + timeline_height - 1.f, paint);
				if (major) {
					auto const label = FormatTimelineLabel(ms);
					auto const label_width = timeline_font.measureText(label.data(), label.size(), SkTextEncoding::kUTF8);
					if (x >= last_label_right && x + label_width <= frame.x + frame.width + 1.f) {
						canvas->drawSimpleText(label.data(), label.size(), SkTextEncoding::kUTF8,
							x, timeline_y + 11.f, timeline_font, paint);
						last_label_right = x + label_width + 2.f;
					}
				}
			}
		}
	}

	// Marker lines, feet and labels are all batched into this frame. The frame
	// builder supplies device-space x coordinates, so no extra time conversion
	// or intermediate surface is needed here.
	canvas->save();
	canvas->clipRect(SkRect::MakeXYWH(frame.x, frame.y, frame.width, frame.height));
	for (auto const& marker : frame.markers) {
		if (!std::isfinite(marker.x) || marker.x < frame.x - 2.f || marker.x > frame.x + frame.width + 2.f)
			continue;
		paint.setColor(static_cast<SkColor>(marker.color));
		paint.setStrokeWidth(static_cast<float>(std::max(1, marker.width)));
		canvas->drawLine(marker.x, frame.y, marker.x, frame.y + frame.height, paint);
		if (marker.feet & 1u) {
			canvas->drawLine(marker.x, frame.y, marker.x - 4.f, frame.y + 4.f, paint);
			canvas->drawLine(marker.x, frame.y + frame.height, marker.x - 4.f, frame.y + frame.height - 4.f, paint);
		}
		if (marker.feet & 2u) {
			canvas->drawLine(marker.x, frame.y, marker.x + 4.f, frame.y + 4.f, paint);
			canvas->drawLine(marker.x, frame.y + frame.height, marker.x + 4.f, frame.y + frame.height - 4.f, paint);
		}
	}
	if (frame.cursor && std::isfinite(frame.cursor->x)) {
		paint.setColor(static_cast<SkColor>(frame.cursor->color));
		paint.setStrokeWidth(1.f);
		canvas->drawLine(frame.cursor->x, frame.y, frame.cursor->x, frame.y + frame.height, paint);
	}
	canvas->restore();

	if (!frame.labels.empty() || (frame.cursor && !frame.cursor->label.empty())) {
		SkFont font;
		font.setSize(11.f);
		paint.setAntiAlias(true);
		paint.setColor(SK_ColorWHITE);
		auto const label_y = frame.y + 12.f;
		for (auto const& label : frame.labels) {
			if (label.text.empty() || !std::isfinite(label.x)
				|| !std::isfinite(label.width) || label.width <= 0.f)
				continue;
			canvas->save();
			canvas->clipRect(SkRect::MakeXYWH(label.x, frame.y, label.width, frame.height));
			canvas->drawSimpleText(label.text.data(), label.text.size(), SkTextEncoding::kUTF8,
				label.x, label_y, font, paint);
			canvas->restore();
		}
		if (frame.cursor && !frame.cursor->label.empty() && std::isfinite(frame.cursor->x)) {
			canvas->drawSimpleText(frame.cursor->label.data(), frame.cursor->label.size(), SkTextEncoding::kUTF8,
				frame.cursor->x + 3.f, frame.y + 12.f, font, paint);
		}
	}

	if (frame.scrollbar && frame.scrollbar->height > 0) {
		auto const scrollbar = *frame.scrollbar;
		auto const y = static_cast<float>(std::clamp(scrollbar.y, 0, target.height - scrollbar.height));
		auto const h = static_cast<float>(scrollbar.height);
		paint.setAntiAlias(false);
		paint.setColor(static_cast<SkColor>(scrollbar.background_color));
		canvas->drawRect(SkRect::MakeXYWH(0.f, y, static_cast<float>(target.width), h), paint);
		auto const total = std::max(1, scrollbar.total);
		auto const page = std::clamp(scrollbar.page, 1, total);
		auto const track = std::max(1.f, static_cast<float>(target.width));
		if (scrollbar.load_position >= 0 && scrollbar.load_position < scrollbar.total) {
			auto const loaded_x = track * static_cast<float>(scrollbar.load_position)
				/ static_cast<float>(std::max(1, scrollbar.total));
			paint.setColor(static_cast<SkColor>(scrollbar.thumb_color));
			canvas->drawRect(SkRect::MakeXYWH(
				std::max(0.f, loaded_x - 25.f), y + 1.f, std::min(25.f, loaded_x), std::max(1.f, h - 2.f)), paint);
		}
		auto const thumb_width = std::max(8.f, track * static_cast<float>(page) / static_cast<float>(total));
		auto const max_position = std::max(0, total - page);
		auto const thumb_x = max_position == 0 ? 0.f : (track - thumb_width)
			* static_cast<float>(std::clamp(scrollbar.position, 0, max_position)) / static_cast<float>(max_position);
		paint.setColor(static_cast<SkColor>(scrollbar.thumb_color));
		canvas->drawRect(SkRect::MakeXYWH(thumb_x, y, thumb_width, h), paint);
		if (scrollbar.selection_start >= 0 && scrollbar.selection_length > 0) {
			auto const selection_x = track * static_cast<float>(scrollbar.selection_start) / static_cast<float>(total);
			auto const selection_width = track * static_cast<float>(scrollbar.selection_length) / static_cast<float>(total);
			paint.setColor(static_cast<SkColor>(scrollbar.selection_color));
			canvas->drawRect(SkRect::MakeXYWH(selection_x, y, std::max(1.f, selection_width), h), paint);
		}
	}
}

}

struct Presenter::Impl {
	struct GpuContentEntry {
		sk_sp<SkImage> primary;
		sk_sp<SkImage> secondary;
		std::size_t bytes = 0;
		std::uint64_t touch = 0;
		std::uint64_t spectrum_revision = 0;
	};
	struct GpuContentTouch {
		std::uint64_t touch = 0;
		ContentTileKey key;
		bool operator>(GpuContentTouch const& other) const noexcept { return touch > other.touch; }
	};

	explicit Impl(FailureInjection failure_injection)
	: failure_injection(failure_injection)
	, device(DeviceFailureInjection(failure_injection)) {
		auto const sksl = SkString(R"(
			uniform shader power_texture;
			uniform shader palette_texture;
			uniform float amplitude;
			half4 main(float2 p) {
				half4 encoded = power_texture.eval(p);
				float normalized = encoded.r * (65280.0 / 65535.0)
				                 + encoded.g * (255.0 / 65535.0);
				float power = normalized * 8.0;
				float palette_x = clamp(power * amplitude, 0.0, 1.0) * 255.0 + 0.5;
				return palette_texture.eval(float2(palette_x, 0.5));
			}
		)");
		auto result = SkRuntimeEffect::MakeForShader(sksl);
		spectrum_effect = std::move(result.effect);
		if (!spectrum_effect)
			spectrum_effect_error = result.errorText.c_str();
	}

	FailureInjection failure_injection = FailureInjection::None;
	SkiaGlDevice device;
	SkiaSurfaceProvider surface_provider;
	sk_sp<SkSurface> surface;
	std::optional<SurfaceKey> surface_key;
	std::unordered_map<ContentTileKey, GpuContentEntry, ContentTileKeyHash> content_cache;
	std::priority_queue<GpuContentTouch, std::vector<GpuContentTouch>, std::greater<GpuContentTouch>> content_touches;
	std::size_t content_cache_budget = kDefaultContentCacheBudget;
	std::size_t content_cache_bytes = 0;
	std::uint64_t content_touch_counter = 0;
	sk_sp<SkRuntimeEffect> spectrum_effect;
	std::string spectrum_effect_error;
	std::uint64_t palette_revision = 0;
	sk_sp<SkImage> palette_image;
	std::unordered_map<std::uint64_t, sk_sp<SkImage>> palette_images;
	PresenterMetrics metrics;
	SkiaGlContextToken last_context;
	bool failure_logged = false;
	bool gl_probed = false;
	bool resource_budget_set = false;
	std::string gl_vendor;
	std::string gl_renderer;
	std::string gl_version;

	void UpdateContentMetrics() noexcept {
		metrics.content_cache_entries = content_cache.size();
		metrics.content_cache_bytes = content_cache_bytes;
		metrics.content_cache_budget_bytes = content_cache_budget;
	}

	void ResetContentCache() noexcept {
		content_cache.clear();
		content_touches = {};
		content_cache_bytes = 0;
		content_touch_counter = 0;
		palette_image.reset();
		palette_images.clear();
		palette_revision = 0;
		UpdateContentMetrics();
	}

	void Fail(SkiaGlContextToken context, SkiaGlDeviceFailure failure, std::string detail) noexcept {
		last_context = context;
		surface.reset();
		surface_key.reset();
		ResetContentCache();
		device.Fail(context, failure, std::move(detail));
	}

	void TouchContent(ContentTileKey const& key, GpuContentEntry& entry) {
		entry.touch = ++content_touch_counter;
		content_touches.push({ entry.touch, key });
	}

	void TrimContentCache() {
		while (content_cache_bytes > content_cache_budget && !content_cache.empty()) {
			bool evicted = false;
			while (!content_touches.empty()) {
				auto const candidate = content_touches.top();
				content_touches.pop();
				auto entry = content_cache.find(candidate.key);
				if (entry == content_cache.end() || entry->second.touch != candidate.touch)
					continue;
				content_cache_bytes -= entry->second.bytes;
				content_cache.erase(entry);
				++metrics.content_evictions;
				evicted = true;
				break;
			}
			if (!evicted)
				break;
		}
		UpdateContentMetrics();
	}

	bool PrepareFrame(SkiaGlContextToken context, FrameTarget const& target) {
		last_context = context;
		auto validation = ValidateFrameTarget(target, context.generation);
		if (!validation.valid) {
			Fail(context, SkiaGlDeviceFailure::InvalidFrameTarget, std::move(validation.detail));
			return false;
		}
		if (failure_injection == FailureInjection::Unsupported) {
			Fail(
				context,
				SkiaGlDeviceFailure::UnsupportedFailureInjection,
				"AEGISUB_SKIA_AUDIO_FAILURE_INJECTION contains an unsupported value");
			return false;
		}

		if (!gl_probed) {
			gl_vendor = ReadGlString(GL_VENDOR);
			gl_renderer = ReadGlString(GL_RENDERER);
			gl_version = ReadGlString(GL_VERSION);
			gl_probed = true;
		}
		if (!SupportsSkiaGaneshDesktopGl(gl_version)) {
			Fail(
				context,
				SkiaGlDeviceFailure::GlVersionUnsupported,
				"Skia Audio Display requires desktop OpenGL 2.0 or newer; GL_VERSION=" + gl_version);
			return false;
		}
		if (IsSoftwareLikeGlRenderer(gl_vendor, gl_renderer)) {
			Fail(
				context,
				SkiaGlDeviceFailure::SoftwareRendererUnsupported,
				"software-like OpenGL renderer is not enabled for Audio Display; GL_RENDERER=" + gl_renderer);
			return false;
		}
		if (failure_injection == FailureInjection::FrameBegin) {
			Fail(
				context,
				SkiaGlDeviceFailure::FrameBeginInjected,
				"AEGISUB_SKIA_AUDIO_FAILURE_INJECTION requested frame-begin");
			return false;
		}
		if (!device.BeginExternalFrame(context))
			return false;
		if (!resource_budget_set) {
			device.Get()->setResourceCacheLimit(kGaneshResourceCacheBudget);
			resource_budget_set = true;
		}

		auto const key = MakeSurfaceKey(target);
		if (!surface || !surface_key || *surface_key != key) {
			surface.reset();
			surface_key.reset();

			SkiaFramebufferSurfaceDescriptor descriptor;
			descriptor.width = target.width;
			descriptor.height = target.height;
			descriptor.sample_count = target.sample_count;
			descriptor.stencil_bits = target.stencil_bits;
			descriptor.framebuffer_id = target.framebuffer_id;
			descriptor.bottom_left_origin = target.bottom_left_origin;
			surface = surface_provider.AcquireFramebufferSurface(device.Get(), descriptor);
			if (!surface) {
				Fail(
					context,
					SkiaGlDeviceFailure::SurfaceAcquisitionFailed,
					"failed to wrap the Audio Display back buffer");
				return false;
			}
			surface_key = key;
			++metrics.surface_acquisitions;
		}
		if (!surface->getCanvas()) {
			Fail(context, SkiaGlDeviceFailure::SurfaceAcquisitionFailed, "the wrapped Audio Display surface has no canvas");
			return false;
		}
		return true;
	}

	bool FinishFrame(SkiaGlContextToken context) {
		if (!device.FlushAndSubmit(context)) {
			device.ResetTextureBindingsForExternalUse(context);
			return false;
		}
		++metrics.submits;
		device.ResetTextureBindingsForExternalUse(context);
		return true;
	}

	std::optional<GpuContentEntry> UploadContentTile(
		ContentTile const& tile,
		SpectrumBandPlan const *spectrum_band_plan) {
		auto *context = device.Get();
		if (!context)
			return std::nullopt;

		GpuContentEntry uploaded;
		if (tile.key.kind == ContentKind::Waveform) {
			auto peak_mask = BuildWaveformMask(tile, false);
			auto average_mask = BuildWaveformMask(tile, true);
			auto const info = SkImageInfo::MakeA8(
				static_cast<int>(tile.key.column_count),
				kWaveformMaskHeight);
			uploaded.primary = UploadTexture(
				context,
				info,
				peak_mask.data(),
				peak_mask.size(),
				tile.key.column_count);
			uploaded.secondary = UploadTexture(
				context,
				info,
				average_mask.data(),
				average_mask.size(),
				tile.key.column_count);
			if (!uploaded.primary || !uploaded.secondary)
				return std::nullopt;
			uploaded.bytes = uploaded.primary->textureSize() + uploaded.secondary->textureSize();
			if (!uploaded.bytes)
				uploaded.bytes = peak_mask.size() + average_mask.size();
		}
		else {
			if (!spectrum_band_plan || !spectrum_band_plan->IsValid())
				return std::nullopt;
			auto pixels = EncodeSpectrumPower(tile, *spectrum_band_plan);
			auto const info = SkImageInfo::Make(
				static_cast<int>(tile.key.column_count),
				spectrum_band_plan->output_height,
				kRGBA_8888_SkColorType,
				kOpaque_SkAlphaType,
				nullptr);
			uploaded.primary = UploadTexture(
				context,
				info,
				pixels.data(),
				pixels.size(),
				static_cast<std::size_t>(tile.key.column_count) * 4);
			if (!uploaded.primary)
				return std::nullopt;
			uploaded.spectrum_revision = spectrum_band_plan->revision;
			uploaded.bytes = uploaded.primary->textureSize();
			if (!uploaded.bytes)
				uploaded.bytes = pixels.size();
		}
		return uploaded;
	}

	std::optional<GpuContentEntry> AcquireContentTile(
		ContentTile const& tile,
		SpectrumBandPlan const *spectrum_band_plan) {
		auto found = content_cache.find(tile.key);
		if (found != content_cache.end()) {
			if (tile.key.kind != ContentKind::Spectrum
				|| (spectrum_band_plan && found->second.spectrum_revision == spectrum_band_plan->revision)) {
				TouchContent(found->first, found->second);
				++metrics.content_cache_hits;
				return found->second;
			}
			content_cache_bytes -= found->second.bytes;
			content_cache.erase(found);
			UpdateContentMetrics();
		}

		++metrics.content_cache_misses;
		if (!tile.IsValid())
			return std::nullopt;
		auto uploaded = UploadContentTile(tile, spectrum_band_plan);
		if (!uploaded)
			return std::nullopt;
		if (uploaded->bytes > content_cache_budget)
			return std::nullopt;

		auto const key = tile.key;
		auto [entry, inserted] = content_cache.emplace(key, std::move(*uploaded));
		if (!inserted)
			return entry->second;
		content_cache_bytes += entry->second.bytes;
		metrics.content_upload_bytes += entry->second.bytes;
		++metrics.content_uploads;
		TouchContent(entry->first, entry->second);
		TrimContentCache();
		return entry->second;
	}

	sk_sp<SkImage> AcquirePalette(SpectrumPalette const& palette) {
		if (auto found = palette_images.find(palette.revision); found != palette_images.end())
			return found->second;

		auto pixels = EncodePalette(palette);
		auto const info = SkImageInfo::Make(
			static_cast<int>(palette.colors.size()),
			1,
			kRGBA_8888_SkColorType,
			kUnpremul_SkAlphaType,
			SkColorSpace::MakeSRGB());
		auto uploaded = UploadTexture(
			device.Get(),
			info,
			pixels.data(),
			pixels.size(),
			pixels.size());
		if (!uploaded)
			return nullptr;
		palette_image = uploaded;
		palette_revision = palette.revision;
		if (palette_images.size() >= 8)
			palette_images.erase(palette_images.begin());
		palette_images.emplace(palette.revision, uploaded);
		++metrics.palette_uploads;
		return palette_image;
	}
};

Presenter::Presenter(FailureInjection failure_injection)
: impl(std::make_unique<Impl>(failure_injection)) {
	impl->UpdateContentMetrics();
}

Presenter::~Presenter() = default;

bool Presenter::RenderDiagnosticFrame(
	SkiaGlContextToken context,
	FrameTarget const& target) try {
	++impl->metrics.frame_attempts;
	if (!impl->PrepareFrame(context, target))
		return false;

	auto *canvas = impl->surface->getCanvas();
	canvas->clear(SkColorSetRGB(24, 34, 48));
	SkPaint paint;
	paint.setAntiAlias(false);
	paint.setColor(SkColorSetRGB(42, 157, 143));
	canvas->drawRect(SkRect::MakeXYWH(0.f, 0.f, target.width * 0.32f, static_cast<float>(target.height)), paint);
	paint.setColor(SkColorSetRGB(233, 196, 106));
	canvas->drawRect(SkRect::MakeXYWH(
		target.width * 0.32f,
		target.height * 0.58f,
		target.width * 0.68f,
		target.height * 0.42f), paint);
	return impl->FinishFrame(context);
}
catch (std::exception const& err) {
	impl->Fail(context, SkiaGlDeviceFailure::SurfaceAcquisitionFailed, err.what());
	return false;
}
catch (...) {
	impl->Fail(context, SkiaGlDeviceFailure::SurfaceAcquisitionFailed, "an unknown exception escaped the Audio Display presenter");
	return false;
}

bool Presenter::RenderContentFrame(
	SkiaGlContextToken context,
	FrameTarget const& target,
	ContentFrame const& frame) try {
	++impl->metrics.frame_attempts;
	if (!impl->PrepareFrame(context, target))
		return false;
	if (auto const error = ValidateContentFrame(target, frame); !error.empty()) {
		impl->Fail(context, SkiaGlDeviceFailure::InvalidFrameTarget, error);
		return false;
	}

	auto *canvas = impl->surface->getCanvas();
	canvas->clear(static_cast<SkColor>(frame.background_color));
	SkRect const content_bounds = SkRect::MakeXYWH(
		static_cast<float>(frame.x),
		static_cast<float>(frame.y),
		static_cast<float>(frame.width),
		static_cast<float>(frame.height));
	SkPaint paint;
	paint.setAntiAlias(false);
	std::vector<StyleFrame> styles = frame.styles;
	if (styles.empty()) {
		StyleFrame style;
		style.x = frame.x;
		style.width = frame.width;
		style.background_color = frame.background_color;
		style.waveform_peak_color = frame.waveform_peak_color;
		style.waveform_average_color = frame.waveform_average_color;
		style.waveform_zero_color = frame.waveform_zero_color;
		style.spectrum_palette = frame.spectrum_palette;
		styles.push_back(std::move(style));
	}
	for (auto const& style : styles) {
		paint.setColor(static_cast<SkColor>(style.background_color));
		canvas->drawRect(SkRect::MakeXYWH(style.x, frame.y, style.width, frame.height), paint);
	}

	std::unordered_map<std::uint64_t, sk_sp<SkImage>> palettes;
	if (frame.kind == ContentKind::Spectrum) {
		if (!impl->spectrum_effect) {
			impl->Fail(
				context,
				SkiaGlDeviceFailure::ContentShaderUnavailable,
				"Skia spectrum runtime effect failed to compile: " + impl->spectrum_effect_error);
			return false;
		}
		for (auto const& style : styles) {
			auto const revision = style.spectrum_palette->revision;
			if (palettes.contains(revision))
				continue;
			auto palette = impl->AcquirePalette(*style.spectrum_palette);
			if (!palette) {
				impl->Fail(context, SkiaGlDeviceFailure::ContentUploadFailed, "failed to upload an Audio spectrum style palette");
				return false;
			}
			palettes.emplace(revision, std::move(palette));
		}
	}

	canvas->save();
	canvas->clipRect(content_bounds);
	for (auto const& tile : frame.tiles) {
		if (!tile
			|| !tile->HasValidShape()
			|| tile->key.generation != frame.generation
			|| tile->key.kind != frame.kind
			|| (frame.kind == ContentKind::Spectrum
				&& tile->key.spectrum_bin_count != frame.spectrum_band_plan->bin_count)
			|| tile->key.tile_index > std::numeric_limits<std::uint64_t>::max() / tile->key.column_count) {
			++impl->metrics.content_tiles_skipped;
			continue;
		}

		auto const tile_first = tile->key.tile_index * tile->key.column_count;
		double relative_x = tile_first >= frame.first_column
			? static_cast<double>(tile_first - frame.first_column)
			: -static_cast<double>(frame.first_column - tile_first);
		if (relative_x >= frame.width
			|| relative_x + tile->key.column_count <= 0.0) {
			continue;
		}

		auto gpu_tile = impl->AcquireContentTile(
			*tile,
			frame.kind == ContentKind::Spectrum ? frame.spectrum_band_plan.get() : nullptr);
		if (!gpu_tile) {
			impl->Fail(context, SkiaGlDeviceFailure::ContentUploadFailed, "failed to upload or retain an Audio content tile");
			return false;
		}
		auto const destination_x = frame.x + frame.first_column_offset + static_cast<float>(relative_x);

		if (frame.kind == ContentKind::Waveform) {
			auto const amplitude = std::clamp(frame.amplitude, 0.f, 64.f);
			auto const scaled_height = frame.height * amplitude;
			SkRect const source = SkRect::MakeWH(
				static_cast<float>(tile->key.column_count),
				static_cast<float>(kWaveformMaskHeight));
			SkRect const destination = SkRect::MakeXYWH(
				destination_x,
				frame.y + (frame.height - scaled_height) * 0.5f,
				static_cast<float>(tile->key.column_count),
				scaled_height);
			if (destination.height() > 0.f) {
				for (auto const& style : styles) {
					auto const style_bounds = SkRect::MakeXYWH(style.x, frame.y, style.width, frame.height);
					if (!SkRect::Intersects(style_bounds, destination))
						continue;
					canvas->save();
					canvas->clipRect(style_bounds);
					paint.reset();
					paint.setAntiAlias(false);
					paint.setColor(static_cast<SkColor>(style.waveform_peak_color));
					canvas->drawImageRect(
						gpu_tile->primary,
						source,
						destination,
						SkSamplingOptions(SkFilterMode::kNearest),
						&paint,
						SkCanvas::kStrict_SrcRectConstraint);
					if (frame.draw_waveform_average) {
						paint.setColor(static_cast<SkColor>(style.waveform_average_color));
						canvas->drawImageRect(
							gpu_tile->secondary,
							source,
							destination,
							SkSamplingOptions(SkFilterMode::kNearest),
							&paint,
							SkCanvas::kStrict_SrcRectConstraint);
					}
					canvas->restore();
				}
			}
		}
		else {
			SkRect const destination = SkRect::MakeXYWH(
				destination_x,
				frame.y,
				static_cast<float>(tile->key.column_count),
				frame.height);
			for (auto const& style : styles) {
				auto const style_bounds = SkRect::MakeXYWH(style.x, frame.y, style.width, frame.height);
				if (!SkRect::Intersects(style_bounds, destination))
					continue;
				auto power_shader = gpu_tile->primary->makeRawShader(
					SkSamplingOptions(SkFilterMode::kLinear),
					nullptr);
				auto palette_shader = palettes.at(style.spectrum_palette->revision)->makeShader(
					SkSamplingOptions(SkFilterMode::kLinear),
					nullptr);
				if (!power_shader || !palette_shader) {
					impl->Fail(context, SkiaGlDeviceFailure::ContentShaderUnavailable, "failed to create a spectrum child shader");
					return false;
				}
				SkRuntimeShaderBuilder builder(impl->spectrum_effect);
				builder.child("power_texture") = std::move(power_shader);
				builder.child("palette_texture") = std::move(palette_shader);
				builder.uniform("amplitude") = std::clamp(frame.amplitude, 0.f, 64.f);
				auto shader = builder.makeShader();
				if (!shader) {
					impl->Fail(context, SkiaGlDeviceFailure::ContentShaderUnavailable, "failed to instantiate the spectrum runtime shader");
					return false;
				}

				paint.reset();
				paint.setShader(std::move(shader));
				canvas->save();
				canvas->clipRect(style_bounds);
				canvas->translate(destination_x, frame.y);
				canvas->drawRect(SkRect::MakeWH(
					static_cast<float>(tile->key.column_count),
					frame.height), paint);
				canvas->restore();
			}
		}
		++impl->metrics.content_tiles_drawn;
	}

	if (frame.kind == ContentKind::Waveform) {
		for (auto const& style : styles) {
			paint.reset();
			paint.setAntiAlias(false);
			paint.setColor(static_cast<SkColor>(style.waveform_zero_color));
			canvas->drawLine(
				style.x,
				frame.y + frame.height * 0.5f,
				style.x + style.width,
				frame.y + frame.height * 0.5f,
				paint);
		}
	}
	canvas->restore();
	DrawAudioFrameLayers(canvas, target, frame);
	return impl->FinishFrame(context);
}
catch (std::exception const& err) {
	impl->Fail(context, SkiaGlDeviceFailure::ContentUploadFailed, err.what());
	return false;
}
catch (...) {
	impl->Fail(context, SkiaGlDeviceFailure::ContentUploadFailed, "an unknown exception escaped Audio retained content rendering");
	return false;
}

void Presenter::SetContentCacheBudget(std::size_t budget_bytes) {
	impl->content_cache_budget = std::max<std::size_t>(1, budget_bytes);
	impl->TrimContentCache();
}

void Presenter::Fail(
	SkiaGlContextToken context,
	SkiaGlDeviceFailure failure,
	std::string detail) noexcept {
	impl->Fail(context, failure, std::move(detail));
}

void Presenter::Release(SkiaGlContextToken context) noexcept {
	impl->last_context = context;
	impl->surface.reset();
	impl->surface_key.reset();
	impl->ResetContentCache();
	impl->device.ReleaseResourcesAndAbandon(context);
}

void Presenter::Abandon() noexcept {
	impl->device.Abandon();
	impl->surface.reset();
	impl->surface_key.reset();
	impl->ResetContentCache();
}

SkiaGlDeviceHealth Presenter::Health() const noexcept {
	return impl->device.Health();
}

SkiaGlDeviceFailure Presenter::LastFailure() const noexcept {
	return impl->device.LastFailure();
}

PresenterMetrics Presenter::Metrics() const noexcept {
	impl->UpdateContentMetrics();
	return impl->metrics;
}

std::string Presenter::TakeFailureLogMessage() {
	if (impl->failure_logged || impl->device.LastFailure() == SkiaGlDeviceFailure::None)
		return {};
	impl->failure_logged = true;

	std::ostringstream message;
	message
		<< "Skia Audio Display is disabled for this widget: context="
		<< impl->last_context.identity
		<< ", generation=" << impl->last_context.generation
		<< ", health=" << ToString(impl->device.Health())
		<< ", failure=" << ToString(impl->device.LastFailure());
	if (!impl->device.LastFailureDetail().empty())
		message << ", detail=" << impl->device.LastFailureDetail();
	if (!impl->gl_vendor.empty())
		message << ", GL_VENDOR=" << impl->gl_vendor;
	if (!impl->gl_renderer.empty())
		message << ", GL_RENDERER=" << impl->gl_renderer;
	if (!impl->gl_version.empty())
		message << ", GL_VERSION=" << impl->gl_version;
	return message.str();
}

}
