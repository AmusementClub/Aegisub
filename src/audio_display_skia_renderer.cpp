// Copyright (c) 2026
// All rights reserved.

#include "audio_display_skia_renderer.h"
#include "audio_display_skia_target.h"

#ifdef WITH_SKIA
#include <include/core/SkBitmap.h>
#include <include/core/SkCanvas.h>
#include <include/core/SkColor.h>
#include <include/core/SkData.h>
#include <include/core/SkFont.h>
#include <include/core/SkFontMetrics.h>
#include <include/core/SkFontMgr.h>
#include <include/core/SkImage.h>
#include <include/core/SkImageInfo.h>
#include <include/core/SkPoint.h>
#include <include/core/SkPath.h>
#include <include/core/SkPaint.h>
#include <include/core/SkRect.h>
#include <include/core/SkSurface.h>
#include <include/core/SkTypeface.h>
#include <include/effects/SkRuntimeEffect.h>
#ifdef _WIN32
#include <include/ports/SkTypeface_win.h>
#endif
#endif

#include <wx/rawbmp.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace {
#ifdef WITH_SKIA
struct RasterScratch {
	std::vector<uint32_t> pixels;
	sk_sp<SkSurface> surface;
	int width = 0;
	int height = 0;
};

RasterScratch& GetBitmapSurfaceScratch() {
	thread_local RasterScratch scratch;
	return scratch;
}

std::vector<uint32_t>& GetSpectrumPixelScratch() {
	thread_local std::vector<uint32_t> pixels;
	return pixels;
}

SkColor ToSkColor(uint32_t packed_bgra) {
	return SkColorSetARGB(
		AudioDisplayColourA(packed_bgra),
		AudioDisplayColourR(packed_bgra),
		AudioDisplayColourG(packed_bgra),
		AudioDisplayColourB(packed_bgra));
}

int RelativeXFromTime(int ms, int scroll_left, double ms_per_pixel) {
	return static_cast<int>(ms / ms_per_pixel) - scroll_left;
}

template <typename Fn>
void ForEachVisibleStyleRange(AudioDisplayRenderModel const& model, int visible_x1, int visible_x2, Fn&& fn) {
	auto pt = begin(model.style_ranges);
	auto pe = end(model.style_ranges);
	if (pt == pe) {
		fn(AudioStyle_Normal, visible_x1, visible_x2);
		return;
	}

	while (pt != pe && pt + 1 != pe && (pt + 1)->first < model.viewport.begin_ms)
		++pt;

	bool drew_any = false;
	while (pt != pe && pt->first < model.viewport.end_ms) {
		auto const range_style = static_cast<AudioRenderingStyle>(pt->second);
		int const range_x1 = std::max(
			visible_x1,
			RelativeXFromTime(pt->first, model.viewport.scroll_left, model.viewport.ms_per_pixel));
		int range_x2 = visible_x2;
		if (++pt != pe) {
			range_x2 = std::min(
				range_x2,
				RelativeXFromTime(pt->first, model.viewport.scroll_left, model.viewport.ms_per_pixel));
		}

		if (range_x2 > range_x1) {
			fn(range_style, range_x1, range_x2);
			drew_any = true;
		}
	}

	if (!drew_any)
		fn(AudioStyle_Normal, visible_x1, visible_x2);
}

bool PrepareBitmapSurface(wxBitmap const& bitmap, RasterScratch &scratch) {
	int const width = bitmap.GetWidth();
	int const height = bitmap.GetHeight();
	if (width <= 0 || height <= 0)
		return false;

	size_t const pixel_count = static_cast<size_t>(width) * static_cast<size_t>(height);
	if (scratch.pixels.size() != pixel_count)
		scratch.pixels.resize(pixel_count);
	std::fill(scratch.pixels.begin(), scratch.pixels.end(), 0);

	if (!scratch.surface || scratch.width != width || scratch.height != height) {
		auto const image_info = SkImageInfo::Make(
			width,
			height,
			kBGRA_8888_SkColorType,
			kPremul_SkAlphaType);
		scratch.surface = SkSurfaces::WrapPixels(
			image_info,
			scratch.pixels.data(),
			static_cast<size_t>(width) * 4);
		scratch.width = width;
		scratch.height = height;
	}

	return scratch.surface != nullptr;
}

sk_sp<SkTypeface> GetDefaultUiTypeface() {
	static sk_sp<SkTypeface> typeface = []() -> sk_sp<SkTypeface> {
#ifdef _WIN32
		if (auto mgr = SkFontMgr_New_DirectWrite()) {
			if (auto tf = mgr->matchFamilyStyle("Segoe UI", SkFontStyle()))
				return tf;
			if (auto tf = mgr->matchFamilyStyle(nullptr, SkFontStyle()))
				return tf;
		}
#endif
		return nullptr;
	}();
	return typeface;
}

sk_sp<SkTypeface> ResolveUiTypeface(const std::string& family_name) {
	if (family_name.empty())
		return GetDefaultUiTypeface();
	static std::string cached_name;
	static sk_sp<SkTypeface> cached_tf;
	if (cached_name == family_name && cached_tf)
		return cached_tf;
	cached_name = family_name;
	cached_tf = nullptr;
#ifdef _WIN32
	if (auto mgr = SkFontMgr_New_DirectWrite())
		cached_tf = mgr->matchFamilyStyle(family_name.c_str(), SkFontStyle());
#endif
	if (!cached_tf)
		cached_tf = GetDefaultUiTypeface();
	return cached_tf;
}

SkFont MakeUiFont(sk_sp<SkTypeface> const& typeface, float size, bool bold = false) {
	SkFont font(typeface, size);
	font.setSubpixel(true);
	font.setEdging(SkFont::Edging::kAntiAlias);
	font.setEmbolden(bold);
	return font;
}

float TopToBaseline(SkFont const& font, float top) {
	SkFontMetrics metrics;
	font.getMetrics(&metrics);
	return top - metrics.fAscent;
}

void DrawOutlinedText(
	SkCanvas &canvas,
	std::string const& utf8,
	float x,
	float top,
	SkFont const& font,
	SkColor fill,
	SkColor outline) {
	float const baseline = TopToBaseline(font, top);

	SkPaint outline_paint;
	outline_paint.setAntiAlias(true);
	outline_paint.setStyle(SkPaint::kStroke_Style);
	outline_paint.setStrokeWidth(2.0f);
	outline_paint.setStrokeJoin(SkPaint::kRound_Join);
	outline_paint.setColor(outline);

	canvas.drawString(utf8.c_str(), x, baseline, font, outline_paint);

	SkPaint fill_paint = outline_paint;
	fill_paint.setStyle(SkPaint::kFill_Style);
	fill_paint.setStrokeWidth(0.0f);
	fill_paint.setColor(fill);
	canvas.drawString(utf8.c_str(), x, baseline, font, fill_paint);
}

bool DrawWaveformContentToCanvas(
	SkCanvas &canvas,
	int /*width*/,
	int /*height*/,
	AudioDisplayRenderModel const& model) {
	auto const& waveform = model.waveform;
	int const audio_top = model.audio_bounds.y;
	int const audio_height = model.audio_bounds.height;

	auto draw_style_range = [&](AudioRenderingStyle style, int range_x1, int range_x2) {
		if (range_x2 <= range_x1)
			return;

		auto const& palette = waveform.palettes[static_cast<size_t>(style)];

		SkPaint background;
		background.setAntiAlias(false);
		background.setStyle(SkPaint::kFill_Style);
		background.setColor(ToSkColor(palette.background));
		canvas.drawRect(
			SkRect::MakeXYWH(
				static_cast<float>(range_x1),
				static_cast<float>(audio_top),
				static_cast<float>(range_x2 - range_x1),
				static_cast<float>(audio_height)),
			background);

		SkPaint peaks;
		peaks.setAntiAlias(false);
		peaks.setStyle(SkPaint::kStroke_Style);
		peaks.setStrokeWidth(1.0f);
		peaks.setColor(ToSkColor(palette.peak_line));

		SkPaint averages = peaks;
		averages.setColor(ToSkColor(palette.average_line));

		SkPaint baseline = peaks;
		baseline.setColor(ToSkColor(palette.baseline));

		int const half_height = audio_height / 2;
		int const midpoint = audio_top + half_height;
		int const local_begin = std::max(range_x1, waveform.pixel_origin);
		int const local_end = std::min(range_x2, waveform.pixel_origin + static_cast<int>(waveform.columns.size()));
		std::vector<SkPoint> peak_points;
		peak_points.reserve(static_cast<size_t>(std::max(0, local_end - local_begin)) * 2);
		std::vector<SkPoint> average_points;
		if (waveform.render_averages)
			average_points.reserve(static_cast<size_t>(std::max(0, local_end - local_begin)) * 2);
		for (int x = local_begin; x < local_end; ++x) {
			auto const& column = waveform.columns[static_cast<size_t>(x - waveform.pixel_origin)];
			if (!column.ready)
				continue;

			int const peak_min = std::max(
				static_cast<int>(column.summary.peak_min * waveform.amplitude_scale * half_height),
				-half_height);
			int const peak_max = std::min(
				static_cast<int>(column.summary.peak_max * waveform.amplitude_scale * half_height),
				half_height);
			int const avg_min = std::max(
				static_cast<int>(column.summary.avg_min * waveform.amplitude_scale * half_height),
				-half_height);
			int const avg_max = std::min(
				static_cast<int>(column.summary.avg_max * waveform.amplitude_scale * half_height),
				half_height);

			peak_points.push_back(SkPoint::Make(static_cast<float>(x), static_cast<float>(midpoint - peak_max)));
			peak_points.push_back(SkPoint::Make(static_cast<float>(x), static_cast<float>(midpoint - peak_min)));
			if (waveform.render_averages) {
				average_points.push_back(SkPoint::Make(static_cast<float>(x), static_cast<float>(midpoint - avg_max)));
				average_points.push_back(SkPoint::Make(static_cast<float>(x), static_cast<float>(midpoint - avg_min)));
			}
		}

		if (!peak_points.empty())
			canvas.drawPoints(SkCanvas::kLines_PointMode, SkSpan<const SkPoint>(peak_points.data(), peak_points.size()), peaks);
		if (!average_points.empty())
			canvas.drawPoints(SkCanvas::kLines_PointMode, SkSpan<const SkPoint>(average_points.data(), average_points.size()), averages);

		canvas.drawLine(
			static_cast<float>(range_x1),
			static_cast<float>(midpoint),
			static_cast<float>(range_x2),
			static_cast<float>(midpoint),
			baseline);
	};

	int const visible_x1 = waveform.pixel_origin;
	int const visible_x2 = waveform.pixel_origin + static_cast<int>(waveform.columns.size());
	ForEachVisibleStyleRange(model, visible_x1, visible_x2, draw_style_range);
	return true;
}

// ---------------------------------------------------------------------------
// Spectrum SkRuntimeEffect shader path
// ---------------------------------------------------------------------------

// SKSL source for the spectrum palette-lookup shader.
// Child shaders:
//   u_power   — Alpha8: columns × (bins_per_column × channel_count).
//               alpha = power / power_range, clamped to [0,1].
//   u_palette — BGRA8: 256 × AudioStyle_MAX.  Row = style index.
//   u_bands   — RGBA8: band_height × 2.
//               Row 0: RG = band_a (16-bit big-endian), BA = band_b (16-bit).
//               Row 1: R = band_frac (0-255 → 0-1).
//   u_styles  — Alpha8: num_columns × 1.  alpha = style index / 255.
static const char kSpectrumShaderSKSL[] = R"(
uniform shader u_power;
uniform shader u_palette;
uniform shader u_bands;
uniform shader u_styles;

uniform float u_amplitude_scale;
uniform float u_power_range;
uniform float u_band_height;
uniform float u_num_columns;
uniform float u_channel_count;
uniform float u_bins_per_column;
uniform float u_interpolated;
uniform float u_audio_top;
uniform float u_pixel_origin;

half4 main(float2 coord) {
    float px = floor(coord.x);
    float py = floor(coord.y);

    float audio_y = py - u_audio_top;
    float channel = floor(audio_y / u_band_height);
    float local_y = u_band_height - 1.0 - (audio_y - channel * u_band_height);

    if (channel < 0.0 || channel >= u_channel_count ||
        local_y < 0.0 || local_y >= u_band_height)
        return half4(0.0);

    float column = px - u_pixel_origin;
    if (column < 0.0 || column >= u_num_columns)
        return half4(0.0);

    // Band mapping (row 0): RG = band_a, BA = band_b (16-bit each)
    half4 br = u_bands.eval(float2(local_y + 0.5, 0.5));
    float band_a = floor(br.r * 255.0) * 256.0 + floor(br.g * 255.0);
    float band_b = floor(br.b * 255.0) * 256.0 + floor(br.a * 255.0);

    float py_base = channel * u_bins_per_column;
    float value = 0.0;

    if (u_interpolated > 0.5) {
        float frac = u_bands.eval(float2(local_y + 0.5, 1.5)).r;
        float pa = u_power.eval(float2(column + 0.5, py_base + band_a + 0.5)).a
                   * u_power_range;
        float pb = u_power.eval(float2(column + 0.5, py_base + band_b + 0.5)).a
                   * u_power_range;
        value = mix(pa, pb, frac);
    } else {
        float spread = band_b - band_a;
        for (int i = 0; i < 64; i++) {
            if (float(i) > spread) break;
            float p = u_power.eval(
                float2(column + 0.5, py_base + band_a + float(i) + 0.5)).a
                * u_power_range;
            value = max(value, p);
        }
    }

    float scaled = clamp(value * u_amplitude_scale, 0.0, 1.0);
    float style = floor(u_styles.eval(float2(column + 0.5, 0.5)).a * 255.0 + 0.5);
    return u_palette.eval(float2(scaled * 255.0 + 0.5, style + 0.5));
}
)";

/// Lazily compiled spectrum SkRuntimeEffect (thread-local for safety).
static sk_sp<SkRuntimeEffect> GetSpectrumEffect() {
	static sk_sp<SkRuntimeEffect> effect;
	static bool tried = false;
	if (!tried) {
		tried = true;
		auto result = SkRuntimeEffect::MakeForShader(SkString(kSpectrumShaderSKSL));
		if (result.effect) {
			effect = std::move(result.effect);
		}
	}
	return effect;
}

/// Maximum power value used for quantising power floats into Alpha8 texels.
static constexpr float kSpectrumPowerRange = 2.5f;

/// Build an Alpha8 image of quantised power data.
/// Layout: width = num_columns, height = bins_per_column × channel_count.
static sk_sp<SkImage> BuildSpectrumPowerImage(
	AudioDisplaySpectrumRenderData const& spectrum,
	int num_columns)
{
	int const channels = std::max(1, spectrum.channel_count);
	int const bins = spectrum.bins_per_column;
	if (num_columns <= 0 || bins <= 0)
		return nullptr;

	int const tex_h = bins * channels;
	SkImageInfo info = SkImageInfo::Make(num_columns, tex_h,
		kAlpha_8_SkColorType, kPremul_SkAlphaType);
	size_t row_bytes = static_cast<size_t>(num_columns);
	auto data = SkData::MakeUninitialized(row_bytes * tex_h);
	uint8_t *dst = static_cast<uint8_t*>(data->writable_data());
	std::memset(dst, 0, row_bytes * tex_h);

	float const inv_range = 255.0f / kSpectrumPowerRange;
	size_t const cols_per_chan = spectrum.ready.size() / std::max(1, channels);

	for (int ch = 0; ch < channels; ++ch) {
		for (int col = 0; col < num_columns && col < static_cast<int>(cols_per_chan); ++col) {
			size_t ready_idx = static_cast<size_t>(ch) * cols_per_chan + col;
			if (ready_idx >= spectrum.ready.size() || !spectrum.ready[ready_idx])
				continue;

			float const *power = spectrum.power.data()
				+ ready_idx * static_cast<size_t>(bins);
			uint8_t *col_dst = dst + static_cast<size_t>(ch * bins) * row_bytes + col;
			for (int b = 0; b < bins; ++b) {
				float v = std::clamp(power[b] * inv_range, 0.0f, 255.0f);
				col_dst[static_cast<size_t>(b) * row_bytes] = static_cast<uint8_t>(v);
			}
		}
	}

	return SkImages::RasterFromData(info, std::move(data), row_bytes);
}

/// Build the BGRA palette image (256 × AudioStyle_MAX).
static sk_sp<SkImage> BuildSpectrumPaletteImage(
	AudioDisplaySpectrumRenderData const& spectrum)
{
	constexpr int w = static_cast<int>(AudioDisplaySpectrumPaletteSize);
	constexpr int h = AudioStyle_MAX;
	SkImageInfo info = SkImageInfo::Make(w, h, kBGRA_8888_SkColorType, kPremul_SkAlphaType);
	size_t row_bytes = static_cast<size_t>(w) * 4;
	auto data = SkData::MakeUninitialized(row_bytes * h);
	auto *dst = static_cast<uint8_t*>(data->writable_data());

	for (int s = 0; s < h; ++s) {
		auto const& pal = spectrum.palettes[s];
		std::memcpy(dst + s * row_bytes, pal.colours.data(), row_bytes);
	}
	return SkImages::RasterFromData(info, std::move(data), row_bytes);
}

/// Build the RGBA8 band-mapping image (band_height × 2).
/// Row 0: R,G = band_a (big-endian uint16), B,A = band_b.
/// Row 1: R = band_frac * 255, G/B/A = 0.
static sk_sp<SkImage> BuildSpectrumBandImage(
	AudioDisplaySpectrumRenderData const& spectrum)
{
	int const h = spectrum.channel_band_height;
	if (h <= 0)
		return nullptr;

	// Must use kPremul so Skia's shader pipeline does NOT premultiply the raw
	// data channels.  With kUnpremul, eval() premultiplies R,G,B by A, which
	// destroys the packed 16-bit band indices when A (low byte of band_b) is 0.
	SkImageInfo info = SkImageInfo::Make(h, 2, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
	size_t row_bytes = static_cast<size_t>(h) * 4;
	auto data = SkData::MakeUninitialized(row_bytes * 2);
	auto *dst = static_cast<uint8_t*>(data->writable_data());
	std::memset(dst, 0, row_bytes * 2);

	// Row 0: band indices
	for (int y = 0; y < h; ++y) {
		int a = (y < static_cast<int>(spectrum.band_a.size())) ? spectrum.band_a[y] : 0;
		int b = (y < static_cast<int>(spectrum.band_b.size())) ? spectrum.band_b[y] : 0;
		uint8_t *px = dst + static_cast<size_t>(y) * 4;
		px[0] = static_cast<uint8_t>((a >> 8) & 0xFF);  // R = high byte
		px[1] = static_cast<uint8_t>(a & 0xFF);          // G = low byte
		px[2] = static_cast<uint8_t>((b >> 8) & 0xFF);   // B = high byte
		px[3] = static_cast<uint8_t>(b & 0xFF);           // A = low byte
	}

	// Row 1: band_frac
	uint8_t *row1 = dst + row_bytes;
	for (int y = 0; y < h; ++y) {
		float frac = (y < static_cast<int>(spectrum.band_frac.size()))
			? spectrum.band_frac[y] : 0.0f;
		row1[static_cast<size_t>(y) * 4] = static_cast<uint8_t>(
			std::clamp(frac * 255.0f, 0.0f, 255.0f));
	}
	return SkImages::RasterFromData(info, std::move(data), row_bytes);
}

/// Build the Alpha8 per-column style-index image (num_columns × 1).
static sk_sp<SkImage> BuildSpectrumStyleImage(
	AudioDisplayRenderModel const& model,
	int visible_x1, int visible_x2)
{
	int const num_columns = visible_x2 - visible_x1;
	if (num_columns <= 0)
		return nullptr;

	SkImageInfo info = SkImageInfo::Make(num_columns, 1,
		kAlpha_8_SkColorType, kPremul_SkAlphaType);
	auto data = SkData::MakeUninitialized(static_cast<size_t>(num_columns));
	auto *dst = static_cast<uint8_t*>(data->writable_data());
	std::memset(dst, 0, num_columns);

	ForEachVisibleStyleRange(model, visible_x1, visible_x2,
		[&](AudioRenderingStyle style, int x1, int x2) {
			uint8_t style_byte = static_cast<uint8_t>(style);
			for (int x = std::max(x1, visible_x1); x < std::min(x2, visible_x2); ++x)
				dst[x - visible_x1] = style_byte;
		});
	return SkImages::RasterFromData(info, std::move(data),
		static_cast<size_t>(num_columns));
}

/// Draw spectrum content using the SkRuntimeEffect shader path.
/// Returns true on success.  Only effective on GPU-backed canvases;
/// on CPU raster canvases, returns false so the caller falls back
/// to the handwritten pixel loop (which is faster than interpreted SKSL).
bool DrawSpectrumContentWithShader(
	SkCanvas &canvas,
	AudioDisplayRenderModel const& model)
{
	// Only use the shader on GPU-backed surfaces.
	if (!canvas.recordingContext())
		return false;

	auto effect = GetSpectrumEffect();
	if (!effect)
		return false;

	auto const& spectrum = model.spectrum;
	int const audio_top = model.audio_bounds.y;
	int const audio_height = model.audio_bounds.height;
	int const channel_count = std::max(1, spectrum.channel_count);
	int const band_height = std::max(1, spectrum.channel_band_height);

	int const visible_x1 = spectrum.pixel_origin;
	int const visible_x2 = spectrum.pixel_origin + (channel_count > 0
		? static_cast<int>(spectrum.ready.size()) / channel_count : 0);
	if (visible_x2 <= visible_x1 || audio_height <= 0)
		return false;

	int const num_columns = visible_x2 - visible_x1;

	// Build child shader textures.
	auto power_img = BuildSpectrumPowerImage(spectrum, num_columns);
	auto palette_img = BuildSpectrumPaletteImage(spectrum);
	auto band_img = BuildSpectrumBandImage(spectrum);
	auto style_img = BuildSpectrumStyleImage(model, visible_x1, visible_x2);
	if (!power_img || !palette_img || !band_img || !style_img)
		return false;

	SkSamplingOptions nearest(SkFilterMode::kNearest);

	SkRuntimeShaderBuilder builder(effect);
	builder.child("u_power")   = power_img->makeShader(SkTileMode::kClamp, SkTileMode::kClamp, nearest);
	builder.child("u_palette") = palette_img->makeShader(SkTileMode::kClamp, SkTileMode::kClamp, nearest);
	builder.child("u_bands")   = band_img->makeShader(SkTileMode::kClamp, SkTileMode::kClamp, nearest);
	builder.child("u_styles")  = style_img->makeShader(SkTileMode::kClamp, SkTileMode::kClamp, nearest);

	builder.uniform("u_amplitude_scale") = spectrum.amplitude_scale;
	builder.uniform("u_power_range")     = kSpectrumPowerRange;
	builder.uniform("u_band_height")     = static_cast<float>(band_height);
	builder.uniform("u_num_columns")     = static_cast<float>(num_columns);
	builder.uniform("u_channel_count")   = static_cast<float>(channel_count);
	builder.uniform("u_bins_per_column") = static_cast<float>(spectrum.bins_per_column);
	builder.uniform("u_interpolated")    = spectrum.interpolated ? 1.0f : 0.0f;
	builder.uniform("u_audio_top")       = static_cast<float>(audio_top);
	builder.uniform("u_pixel_origin")    = static_cast<float>(visible_x1);

	auto shader = builder.makeShader();
	if (!shader)
		return false;

	// First fill the background for each style range.
	{
		SkPaint bg;
		bg.setAntiAlias(false);
		bg.setStyle(SkPaint::kFill_Style);
		ForEachVisibleStyleRange(model, visible_x1, visible_x2,
			[&](AudioRenderingStyle style, int x1, int x2) {
				bg.setColor(ToSkColor(spectrum.palettes[static_cast<size_t>(style)].colours[0]));
				canvas.drawRect(SkRect::MakeLTRB(
					static_cast<float>(x1), static_cast<float>(audio_top),
					static_cast<float>(x2), static_cast<float>(audio_top + audio_height)), bg);
			});
	}

	// Draw the shader rect covering the audio content area.
	SkPaint paint;
	paint.setAntiAlias(false);
	paint.setShader(std::move(shader));
	canvas.drawRect(SkRect::MakeLTRB(
		static_cast<float>(visible_x1), static_cast<float>(audio_top),
		static_cast<float>(visible_x2), static_cast<float>(audio_top + audio_height)), paint);

	// Draw channel dividers on top.
	if (channel_count > 1) {
		SkPaint div;
		div.setAntiAlias(false);
		div.setStyle(SkPaint::kFill_Style);
		div.setColor(ToSkColor(spectrum.channel_divider));
		for (int ch = 1; ch < channel_count; ++ch) {
			float y = static_cast<float>(audio_top + ch * band_height);
			canvas.drawRect(SkRect::MakeLTRB(
				static_cast<float>(visible_x1), y,
				static_cast<float>(visible_x2), y + 1.0f), div);
		}
	}

	return true;
}

bool DrawSpectrumContentToPixels(
	std::vector<uint32_t> &pixels,
	int width,
	int height,
	int origin_x,
	int origin_y,
	AudioDisplayRenderModel const& model) {
	auto const& spectrum = model.spectrum;
	int const audio_top = model.audio_bounds.y;
	int const audio_height = model.audio_bounds.height;
	int const visible_x1 = spectrum.pixel_origin;
	int const visible_x2 = spectrum.pixel_origin + (spectrum.channel_count > 0
		? static_cast<int>(spectrum.ready.size()) / spectrum.channel_count
		: 0);
	if (visible_x2 <= visible_x1)
		return false;

	auto fill_rect = [&](int x1, int y1, int x2, int y2, uint32_t colour) {
		x1 -= origin_x;
		y1 -= origin_y;
		x2 -= origin_x;
		y2 -= origin_y;
		x1 = std::max(0, x1);
		y1 = std::max(0, y1);
		x2 = std::min(width, x2);
		y2 = std::min(height, y2);
		if (x2 <= x1 || y2 <= y1)
			return;

		uint32_t const packed = colour;
		for (int y = y1; y < y2; ++y) {
			auto *row = pixels.data() + static_cast<size_t>(y) * static_cast<size_t>(width);
			std::fill(row + x1, row + x2, packed);
		}
	};

	ForEachVisibleStyleRange(model, visible_x1, visible_x2, [&](AudioRenderingStyle style, int range_x1, int range_x2) {
		fill_rect(range_x1, audio_top, range_x2, audio_top + audio_height, spectrum.palettes[static_cast<size_t>(style)].colours[0]);
	});

	auto const* band_frac = spectrum.interpolated ? spectrum.band_frac.data() : nullptr;
	int const channel_count = std::max(1, spectrum.channel_count);
	int const band_height = std::max(1, spectrum.channel_band_height);

	auto draw_channel_range = [&](AudioRenderingStyle style, int range_x1, int range_x2) {
		auto const& palette = spectrum.palettes[static_cast<size_t>(style)];
		for (int x = std::max(range_x1, visible_x1); x < std::min(range_x2, visible_x2); ++x) {
			int const local_x = x - visible_x1;
			for (int channel = 0; channel < channel_count; ++channel) {
				size_t const ready_index = static_cast<size_t>(channel * (visible_x2 - visible_x1) + local_x);
				if (ready_index >= spectrum.ready.size() || !spectrum.ready[ready_index])
					continue;

				float const* power = spectrum.power.data()
					+ (ready_index * static_cast<size_t>(spectrum.bins_per_column));
				int const y_offset = audio_top + channel * band_height;
				for (int y = 0; y < band_height; ++y) {
					float value = 0.0f;
					if (spectrum.interpolated) {
						float const frac = band_frac[y];
						value = (1.0f - frac) * power[spectrum.band_a[y]] + frac * power[spectrum.band_b[y]];
					}
					else {
						int const first = spectrum.band_a[y];
						int const last = spectrum.band_b[y];
						value = power[first];
						for (int i = first + 1; i <= last; ++i)
							value = std::max(value, power[i]);
					}

					float scaled = std::clamp(value * spectrum.amplitude_scale, 0.0f, 1.0f);
					size_t palette_index = static_cast<size_t>(scaled * static_cast<float>(AudioDisplaySpectrumPaletteSize - 1));
					uint32_t const colour = palette.colours[palette_index];
					int const px = x - origin_x;
					int const py = y_offset + (band_height - 1 - y) - origin_y;
					if (px >= 0 && px < width && py >= 0 && py < height)
						pixels[static_cast<size_t>(py) * static_cast<size_t>(width) + static_cast<size_t>(px)] = colour;
				}
			}
		}
	};

	ForEachVisibleStyleRange(model, visible_x1, visible_x2, draw_channel_range);

	if (channel_count > 1) {
		uint32_t const divider = spectrum.channel_divider;
		for (int channel = 1; channel < channel_count; ++channel) {
			int const y = audio_top + channel * band_height - origin_y;
			if (y < 0 || y >= height)
				continue;
			auto *row = pixels.data() + static_cast<size_t>(y) * static_cast<size_t>(width);
			std::fill(row, row + width, divider);
		}
	}

	return true;
}

void RenderAudioAreaOverlaysOnCanvas(SkCanvas &canvas, AudioDisplayRenderModel const& model) {
	auto typeface = ResolveUiTypeface(model.audio_label_font_face);

	SkPaint line_paint;
	line_paint.setAntiAlias(false);
	line_paint.setStyle(SkPaint::kStroke_Style);

	SkPaint fill_paint;
	fill_paint.setAntiAlias(true);
	fill_paint.setStyle(SkPaint::kFill_Style);

	for (auto const& marker : model.marker_geometry) {
		line_paint.setColor(ToSkColor(marker.style.colour));
		line_paint.setStrokeWidth(static_cast<float>(std::max(1, marker.style.width)));
		canvas.drawLine(
			static_cast<float>(marker.x),
			static_cast<float>(marker.top),
			static_cast<float>(marker.x),
			static_cast<float>(marker.bottom),
			line_paint);

		if (marker.feet == AudioMarker::Feet_None)
			continue;

		fill_paint.setColor(ToSkColor(marker.style.colour));
		float const foot_size = 6.0f;
		auto draw_foot = [&](int dir, float y_top, float y_bottom) {
			SkPoint const points[] = {
				{ static_cast<float>(marker.x + dir * foot_size), y_top },
				{ static_cast<float>(marker.x), y_top },
				{ static_cast<float>(marker.x), y_bottom },
			};
			SkPath const path = SkPath::Polygon({points, 3}, true);
			canvas.drawPath(path, fill_paint);
		};

		if (marker.feet & AudioMarker::Feet_Left) {
			draw_foot(-1, static_cast<float>(marker.top), static_cast<float>(marker.top + foot_size));
			draw_foot(-1, static_cast<float>(marker.bottom), static_cast<float>(marker.bottom - foot_size));
		}
		if (marker.feet & AudioMarker::Feet_Right) {
			draw_foot(1, static_cast<float>(marker.top), static_cast<float>(marker.top + foot_size));
			draw_foot(1, static_cast<float>(marker.bottom), static_cast<float>(marker.bottom - foot_size));
		}
	}

	SkFont label_font = MakeUiFont(typeface, 12.0f, true);
	for (auto const& label : model.label_geometry) {
		std::string const& utf8 = label.text;
		SkPaint label_paint;
		label_paint.setAntiAlias(true);
		label_paint.setColor(SK_ColorWHITE);
		float x = static_cast<float>(label.left);
		if (label.width > 0) {
			float text_width = label_font.measureText(utf8.c_str(), utf8.size(), SkTextEncoding::kUTF8);
			if (label.width >= text_width)
				x += (static_cast<float>(label.width) - text_width) * 0.5f;
		}
		canvas.save();
		canvas.clipRect(SkRect::MakeXYWH(static_cast<float>(label.left), static_cast<float>(label.top), static_cast<float>(std::max(0, label.width)), 32.0f));
		canvas.drawString(utf8.c_str(), x, TopToBaseline(label_font, static_cast<float>(label.top)), label_font, label_paint);
		canvas.restore();
	}

	if (!model.split_channel_labels.empty() && model.audio_bounds.height > 0) {
		int const band_h = model.audio_bounds.height / static_cast<int>(model.split_channel_labels.size());
		SkFont split_font = MakeUiFont(typeface, 11.0f, true);
		for (size_t i = 0; i < model.split_channel_labels.size(); ++i) {
			float const label_x = static_cast<float>(model.audio_bounds.x + 4);
			float const label_y = static_cast<float>(model.audio_bounds.y + static_cast<int>(i) * band_h + 2);
			DrawOutlinedText(canvas, model.split_channel_labels[i], label_x, label_y, split_font, SkColorSetRGB(230, 230, 230), SK_ColorBLACK);
		}
	}

	if (model.track_cursor.visible) {
		line_paint.setColor(SK_ColorWHITE);
		line_paint.setStrokeWidth(1.0f);
		canvas.drawLine(
			static_cast<float>(model.track_cursor.x),
			static_cast<float>(model.track_cursor.top),
			static_cast<float>(model.track_cursor.x),
			static_cast<float>(model.track_cursor.bottom),
			line_paint);

		if (!model.track_cursor.label.empty()) {
			SkFont cursor_font = MakeUiFont(typeface, 12.0f, true);
			std::string const& utf8 = model.track_cursor.label;
			float const text_width = cursor_font.measureText(utf8.c_str(), utf8.size(), SkTextEncoding::kUTF8);
			float x = static_cast<float>(model.track_cursor.x) - text_width * 0.5f;
			float const max_x = static_cast<float>(model.audio_bounds.x + model.audio_bounds.width) - text_width - 2.0f;
			x = std::clamp(x, static_cast<float>(model.audio_bounds.x + 2), max_x);
			DrawOutlinedText(canvas, model.track_cursor.label, x, static_cast<float>(model.track_cursor.top + 2), cursor_font, SK_ColorWHITE, SkColorSetRGB(64, 64, 64));
		}
	}
}

void DrawChromeToCanvas(SkCanvas &canvas, AudioDisplayRenderModel const& model) {
	if (model.redraw_timeline && model.timeline.visible) {
		auto const& timeline = model.timeline;
		SkPaint fill;
		fill.setAntiAlias(false);
		fill.setStyle(SkPaint::kFill_Style);
		fill.setColor(ToSkColor(timeline.dark_colour));
		canvas.drawRect(
			SkRect::MakeXYWH(
				static_cast<float>(timeline.bounds.x),
				static_cast<float>(timeline.bounds.y),
				static_cast<float>(timeline.bounds.width),
				static_cast<float>(timeline.bounds.height)),
			fill);

		SkPaint stroke;
		stroke.setAntiAlias(false);
		stroke.setStyle(SkPaint::kStroke_Style);
		stroke.setStrokeWidth(1.0f);
		stroke.setColor(ToSkColor(timeline.light_colour));
		float const bottom = static_cast<float>(timeline.bounds.y + timeline.bounds.height - 1);
		canvas.drawLine(
			static_cast<float>(timeline.bounds.x),
			bottom,
			static_cast<float>(timeline.bounds.x + timeline.bounds.width),
			bottom,
			stroke);

		SkFont font = MakeUiFont(ResolveUiTypeface(std::string()), 11.0f, false);
		SkPaint text;
		text.setAntiAlias(true);
		text.setColor(ToSkColor(timeline.light_colour));
		int const major_tick_height = 6;
		int const minor_tick_height = 4;
		for (auto const& tick : timeline.ticks) {
			float const x = static_cast<float>(tick.x);
			float const tick_top = bottom - static_cast<float>(tick.major ? major_tick_height : minor_tick_height);
			canvas.drawLine(x, tick_top, x, bottom, stroke);
			if (!tick.label.empty())
				canvas.drawString(tick.label.c_str(), x, TopToBaseline(font, static_cast<float>(timeline.bounds.y)), font, text);
		}
	}

	if (model.redraw_scrollbar && model.scrollbar.visible) {
		auto const& scrollbar = model.scrollbar;
		SkPaint fill;
		fill.setAntiAlias(false);
		fill.setStyle(SkPaint::kFill_Style);
		fill.setColor(ToSkColor(scrollbar.dark_colour));
		canvas.drawRect(SkRect::MakeXYWH(
			static_cast<float>(scrollbar.bounds.x),
			static_cast<float>(scrollbar.bounds.y),
			static_cast<float>(scrollbar.bounds.width),
			static_cast<float>(scrollbar.bounds.height)), fill);

		if (scrollbar.has_selection) {
			fill.setColor(ToSkColor(scrollbar.selection_colour));
			canvas.drawRect(SkRect::MakeXYWH(
				static_cast<float>(scrollbar.selection_rect.x),
				static_cast<float>(scrollbar.selection_rect.y),
				static_cast<float>(scrollbar.selection_rect.width),
				static_cast<float>(scrollbar.selection_rect.height)), fill);
		}

		SkPaint stroke;
		stroke.setAntiAlias(false);
		stroke.setStyle(SkPaint::kStroke_Style);
		stroke.setStrokeWidth(1.0f);
		stroke.setColor(ToSkColor(scrollbar.light_colour));
		canvas.drawRect(SkRect::MakeXYWH(
			static_cast<float>(scrollbar.bounds.x),
			static_cast<float>(scrollbar.bounds.y),
			static_cast<float>(scrollbar.bounds.width),
			static_cast<float>(scrollbar.bounds.height)), stroke);

		if (scrollbar.has_load_marker) {
			fill.setColor(ToSkColor(scrollbar.light_colour));
			canvas.drawRect(SkRect::MakeXYWH(
				static_cast<float>(scrollbar.load_marker_rect.x),
				static_cast<float>(scrollbar.load_marker_rect.y),
				static_cast<float>(scrollbar.load_marker_rect.width),
				static_cast<float>(scrollbar.load_marker_rect.height)), fill);
		}

		fill.setColor(ToSkColor(scrollbar.light_colour));
		canvas.drawRect(SkRect::MakeXYWH(
			static_cast<float>(scrollbar.thumb.x),
			static_cast<float>(scrollbar.thumb.y),
			static_cast<float>(scrollbar.thumb.width),
			static_cast<float>(scrollbar.thumb.height)), fill);
	}
}
#endif
}

bool AudioDisplaySkiaRenderer::CanDrawFrame(AudioDisplayRenderModel const& model) const {
#ifdef WITH_SKIA
	// Full-takeover: can draw a frame whenever there is any visible area.
	// Content data need not be ready — DrawFrameToCanvas fills the audio
	// area background even when spectrum/waveform data is still loading.
	return (model.audio_bounds.width > 0 && model.audio_bounds.height > 0)
		|| (model.redraw_timeline && model.timeline.visible)
		|| (model.redraw_scrollbar && model.scrollbar.visible);
#else
	(void)model;
	return false;
#endif
}

#ifdef WITH_SKIA
bool AudioDisplaySkiaRenderer::DrawFrameToCanvas(SkCanvas &canvas, wxRect const& target_rect, AudioDisplayRenderModel const& model) const {
	if (!CanDrawFrame(model))
		return false;

	canvas.clear(SK_ColorTRANSPARENT);
	canvas.save();
	canvas.translate(-static_cast<float>(target_rect.x), -static_cast<float>(target_rect.y));

	bool drew_any = false;

	// Fill audio area background even if content data is not ready yet,
	// to avoid transparent gaps when only overlays or chrome are drawn.
	if (model.audio_bounds.width > 0 && model.audio_bounds.height > 0) {
		SkColor bg_color = SK_ColorBLACK;
		if (model.content_kind == AudioDisplayContentKind::Waveform
			&& !model.waveform.palettes.empty())
			bg_color = ToSkColor(model.waveform.palettes[AudioStyle_Normal].background);
		else if (model.content_kind == AudioDisplayContentKind::Spectrum
			&& !model.spectrum.palettes.empty())
			bg_color = ToSkColor(model.spectrum.palettes[AudioStyle_Normal].colours[0]);

		SkPaint bg;
		bg.setAntiAlias(false);
		bg.setStyle(SkPaint::kFill_Style);
		bg.setColor(bg_color);
		canvas.drawRect(
			SkRect::MakeXYWH(
				static_cast<float>(model.audio_bounds.x),
				static_cast<float>(model.audio_bounds.y),
				static_cast<float>(model.audio_bounds.width),
				static_cast<float>(model.audio_bounds.height)),
			bg);
		drew_any = true;
	}

	if (DrawContentToCanvas(canvas, target_rect, model))
		drew_any = true;

	// Draw overlays directly without clear/save/translate (already active).
	if (CanDrawAudioAreaOverlays(model)) {
		RenderAudioAreaOverlaysOnCanvas(canvas, model);
		drew_any = true;
	}

	if ((model.redraw_timeline && model.timeline.visible) || (model.redraw_scrollbar && model.scrollbar.visible)) {
		::DrawChromeToCanvas(canvas, model);
		drew_any = true;
	}

	canvas.restore();
	return drew_any;
}
#endif

bool AudioDisplaySkiaRenderer::DrawFrameToBitmap(wxBitmap &bitmap, AudioDisplayRenderModel const& model, wxPoint origin) const {
#ifdef WITH_SKIA
	if (!CanDrawFrame(model) || !bitmap.IsOk())
		return false;

	auto &scratch = GetBitmapSurfaceScratch();
	if (!PrepareBitmapSurface(bitmap, scratch))
		return false;

	auto *canvas = scratch.surface->getCanvas();
	if (!DrawFrameToCanvas(*canvas, wxRect(origin, bitmap.GetSize()), model))
		return false;

	return CopyBgraPixelsToBitmap(scratch.pixels, bitmap.GetWidth(), bitmap.GetHeight(), bitmap);
#else
	(void)bitmap;
	(void)model;
	(void)origin;
	return false;
#endif
}

bool AudioDisplaySkiaRenderer::CanDrawContent(AudioDisplayRenderModel const& model) const {
#ifdef WITH_SKIA
	return CanDrawWaveformContent(model) || CanDrawSpectrumContent(model);
#else
	(void)model;
	return false;
#endif
}

#ifdef WITH_SKIA
bool AudioDisplaySkiaRenderer::DrawContentToCanvas(SkCanvas &canvas, wxRect const& target_rect, AudioDisplayRenderModel const& model) const {
	if (!CanDrawContent(model))
		return false;

	int const width = target_rect.width;
	int const height = target_rect.height;
	switch (model.content_kind) {
	case AudioDisplayContentKind::Waveform:
		return DrawWaveformContentToCanvas(canvas, width, height, model);
	case AudioDisplayContentKind::Spectrum: {
		// Try the SkRuntimeEffect shader path first.
		// This is beneficial on GPU present targets where the result stays
		// on the GPU surface (readback happens only once per frame anyway).
		// On bitmap content targets the shader call is a no-op because the
		// canvas is CPU-backed and SkRuntimeEffect falls back gracefully.
		if (DrawSpectrumContentWithShader(canvas, model))
			return true;

		// Fallback: CPU pixel rasterisation + drawImage.
		SkImageInfo const image_info = SkImageInfo::Make(width, height, kBGRA_8888_SkColorType, kPremul_SkAlphaType);
		size_t const row_bytes = static_cast<size_t>(width) * 4;
		auto &pixels = GetSpectrumPixelScratch();
		size_t const pixel_count = static_cast<size_t>(width) * static_cast<size_t>(height);
		if (pixels.size() != pixel_count)
			pixels.resize(pixel_count);
		std::fill(pixels.begin(), pixels.end(), 0);
		if (!DrawSpectrumContentToPixels(pixels, width, height, target_rect.x, target_rect.y, model))
			return false;
		SkBitmap tmp;
		if (!tmp.tryAllocPixels(image_info, row_bytes))
			return false;
		memcpy(tmp.getPixels(), pixels.data(), row_bytes * static_cast<size_t>(height));
		tmp.notifyPixelsChanged();
		sk_sp<SkImage> image = tmp.asImage();
		if (!image)
			return false;
		canvas.drawImage(image, static_cast<float>(target_rect.x), static_cast<float>(target_rect.y));
		return true;
	}
	default:
		return false;
	}
}
#endif

bool AudioDisplaySkiaRenderer::DrawContentToBitmap(wxBitmap &bitmap, AudioDisplayRenderModel const& model) const {
#ifdef WITH_SKIA
	if (!bitmap.IsOk())
		return false;

	auto &scratch = GetBitmapSurfaceScratch();
	if (!PrepareBitmapSurface(bitmap, scratch))
		return false;
	if (!DrawContentToCanvas(*scratch.surface->getCanvas(), wxRect(wxPoint(0, 0), bitmap.GetSize()), model))
		return false;
	return CopyBgraPixelsToBitmap(scratch.pixels, bitmap.GetWidth(), bitmap.GetHeight(), bitmap);
#else
	(void)bitmap;
	(void)model;
	return false;
#endif
}

bool AudioDisplaySkiaRenderer::CanDrawAudioAreaFrame(AudioDisplayRenderModel const& model) const {
#ifdef WITH_SKIA
	return CanDrawContent(model);
#else
	(void)model;
	return false;
#endif
}

#ifdef WITH_SKIA
bool AudioDisplaySkiaRenderer::DrawAudioAreaFrameToCanvas(SkCanvas &canvas, wxRect const& target_rect, AudioDisplayRenderModel const& model) const {
	if (!CanDrawAudioAreaFrame(model))
		return false;

	// Apply translate so model coordinates (display-space) map to canvas-local
	// coordinates.  writePixels (spectrum) ignores the transform and is already
	// computed relative to target_rect origin, so it remains correct.
	canvas.save();
	canvas.translate(-static_cast<float>(target_rect.x), -static_cast<float>(target_rect.y));

	bool drew_any = DrawContentToCanvas(canvas, target_rect, model);

	if (CanDrawAudioAreaOverlays(model)) {
		RenderAudioAreaOverlaysOnCanvas(canvas, model);
		drew_any = true;
	}

	canvas.restore();
	return drew_any;
}
#endif

bool AudioDisplaySkiaRenderer::DrawAudioAreaFrameToBitmap(wxBitmap &bitmap, AudioDisplayRenderModel const& model, wxPoint origin) const {
#ifdef WITH_SKIA
	if (!CanDrawAudioAreaFrame(model) || !bitmap.IsOk())
		return false;

	auto &scratch = GetBitmapSurfaceScratch();
	if (!PrepareBitmapSurface(bitmap, scratch))
		return false;

	if (!DrawAudioAreaFrameToCanvas(*scratch.surface->getCanvas(), wxRect(origin, bitmap.GetSize()), model))
		return false;

	return CopyBgraPixelsToBitmap(scratch.pixels, bitmap.GetWidth(), bitmap.GetHeight(), bitmap);
#else
	(void)bitmap;
	(void)model;
	(void)origin;
	return false;
#endif
}

bool AudioDisplaySkiaRenderer::CanDrawAudioAreaOverlays(AudioDisplayRenderModel const& model) const {
#ifdef WITH_SKIA
	return model.audio_bounds.width > 0
		&& model.audio_bounds.height > 0
		&& (!model.marker_geometry.empty()
			|| !model.label_geometry.empty()
			|| !model.split_channel_labels.empty()
			|| model.track_cursor.visible);
#else
	(void)model;
	return false;
#endif
}

#ifdef WITH_SKIA
bool AudioDisplaySkiaRenderer::CompositeAudioAreaOverlaysToCanvas(SkCanvas &canvas, wxRect const& target_rect, AudioDisplayRenderModel const& model) const {
	if (!CanDrawAudioAreaOverlays(model))
		return false;

	canvas.save();
	canvas.translate(-static_cast<float>(target_rect.x), -static_cast<float>(target_rect.y));
	RenderAudioAreaOverlaysOnCanvas(canvas, model);
	canvas.restore();
	return true;
}

bool AudioDisplaySkiaRenderer::DrawAudioAreaOverlaysToCanvas(SkCanvas &canvas, wxRect const& target_rect, AudioDisplayRenderModel const& model) const {
	if (!CanDrawAudioAreaOverlays(model))
		return false;

	canvas.clear(SK_ColorTRANSPARENT);
	return CompositeAudioAreaOverlaysToCanvas(canvas, target_rect, model);
}

bool AudioDisplaySkiaRenderer::DrawChromeToCanvas(SkCanvas &canvas, wxRect const& target_rect, AudioDisplayRenderModel const& model) const {
	if (!((model.redraw_timeline && model.timeline.visible) || (model.redraw_scrollbar && model.scrollbar.visible)))
		return false;

	canvas.save();
	canvas.translate(-static_cast<float>(target_rect.x), -static_cast<float>(target_rect.y));
	::DrawChromeToCanvas(canvas, model);
	canvas.restore();
	return true;
}
#endif

bool AudioDisplaySkiaRenderer::DrawAudioAreaOverlaysToBitmap(wxBitmap &bitmap, AudioDisplayRenderModel const& model, wxPoint origin) const {
#ifdef WITH_SKIA
	if (!CanDrawAudioAreaOverlays(model) || !bitmap.IsOk())
		return false;

	auto &scratch = GetBitmapSurfaceScratch();
	if (!PrepareBitmapSurface(bitmap, scratch))
		return false;
	if (!DrawAudioAreaOverlaysToCanvas(*scratch.surface->getCanvas(), wxRect(origin, bitmap.GetSize()), model))
		return false;
	return CopyBgraPixelsToBitmap(scratch.pixels, bitmap.GetWidth(), bitmap.GetHeight(), bitmap);
#else
	(void)bitmap;
	(void)model;
	(void)origin;
	return false;
#endif
}

bool AudioDisplaySkiaRenderer::CanDrawWaveformContent(AudioDisplayRenderModel const& model) const {
#ifdef WITH_SKIA
	return model.content_kind == AudioDisplayContentKind::Waveform
		&& !model.waveform.columns.empty()
		&& model.audio_bounds.width > 0
		&& model.audio_bounds.height > 0;
#else
	(void)model;
	return false;
#endif
}

bool AudioDisplaySkiaRenderer::DrawWaveformContentToBitmap(wxBitmap &bitmap, AudioDisplayRenderModel const& model) const {
#ifdef WITH_SKIA
	if (!CanDrawWaveformContent(model) || !bitmap.IsOk())
		return false;

	int const width = bitmap.GetWidth();
	int const height = bitmap.GetHeight();
	if (width <= 0 || height <= 0)
		return false;

	auto &scratch = GetBitmapSurfaceScratch();
	if (!PrepareBitmapSurface(bitmap, scratch))
		return false;

	auto *canvas = scratch.surface->getCanvas();
	auto const& waveform = model.waveform;
	canvas->clear(ToSkColor(waveform.palettes[AudioStyle_Normal].background));

	auto draw_style_range = [&](AudioRenderingStyle style, int range_x1, int range_x2) {
		if (range_x2 <= range_x1)
			return;

		auto const& palette = waveform.palettes[static_cast<size_t>(style)];

		SkPaint background;
		background.setAntiAlias(false);
		background.setStyle(SkPaint::kFill_Style);
		background.setColor(ToSkColor(palette.background));
		canvas->drawRect(
			SkRect::MakeXYWH(
				static_cast<float>(range_x1),
				0.0f,
				static_cast<float>(range_x2 - range_x1),
				static_cast<float>(height)),
			background);

		SkPaint peaks;
		peaks.setAntiAlias(false);
		peaks.setStyle(SkPaint::kStroke_Style);
		peaks.setStrokeWidth(1.0f);
		peaks.setColor(ToSkColor(palette.peak_line));

		SkPaint averages = peaks;
		averages.setColor(ToSkColor(palette.average_line));

		SkPaint baseline = peaks;
		baseline.setColor(ToSkColor(palette.baseline));

		int const midpoint = height / 2;
		int const local_begin = std::max(range_x1, waveform.pixel_origin);
		int const local_end = std::min(range_x2, waveform.pixel_origin + static_cast<int>(waveform.columns.size()));
		std::vector<SkPoint> peak_points;
		peak_points.reserve(static_cast<size_t>(std::max(0, local_end - local_begin)) * 2);
		std::vector<SkPoint> average_points;
		if (waveform.render_averages)
			average_points.reserve(static_cast<size_t>(std::max(0, local_end - local_begin)) * 2);
		for (int x = local_begin; x < local_end; ++x) {
			auto const& column = waveform.columns[static_cast<size_t>(x - waveform.pixel_origin)];
			if (!column.ready)
				continue;

			int const peak_min = std::max(
				static_cast<int>(column.summary.peak_min * waveform.amplitude_scale * midpoint),
				-midpoint);
			int const peak_max = std::min(
				static_cast<int>(column.summary.peak_max * waveform.amplitude_scale * midpoint),
				midpoint);
			int const avg_min = std::max(
				static_cast<int>(column.summary.avg_min * waveform.amplitude_scale * midpoint),
				-midpoint);
			int const avg_max = std::min(
				static_cast<int>(column.summary.avg_max * waveform.amplitude_scale * midpoint),
				midpoint);

			peak_points.push_back(SkPoint::Make(static_cast<float>(x), static_cast<float>(midpoint - peak_max)));
			peak_points.push_back(SkPoint::Make(static_cast<float>(x), static_cast<float>(midpoint - peak_min)));
			if (waveform.render_averages) {
				average_points.push_back(SkPoint::Make(static_cast<float>(x), static_cast<float>(midpoint - avg_max)));
				average_points.push_back(SkPoint::Make(static_cast<float>(x), static_cast<float>(midpoint - avg_min)));
			}
		}

		if (!peak_points.empty())
			canvas->drawPoints(SkCanvas::kLines_PointMode, SkSpan<const SkPoint>(peak_points.data(), peak_points.size()), peaks);
		if (!average_points.empty())
			canvas->drawPoints(SkCanvas::kLines_PointMode, SkSpan<const SkPoint>(average_points.data(), average_points.size()), averages);

		canvas->drawLine(
			static_cast<float>(range_x1),
			static_cast<float>(midpoint),
			static_cast<float>(range_x2),
			static_cast<float>(midpoint),
			baseline);
	};

	int const visible_x1 = waveform.pixel_origin;
	int const visible_x2 = waveform.pixel_origin + static_cast<int>(waveform.columns.size());
	ForEachVisibleStyleRange(model, visible_x1, visible_x2, draw_style_range);

	return CopyBgraPixelsToBitmap(scratch.pixels, width, height, bitmap);
#else
	(void)bitmap;
	(void)model;
	return false;
#endif
}

bool AudioDisplaySkiaRenderer::CanDrawSpectrumContent(AudioDisplayRenderModel const& model) const {
#ifdef WITH_SKIA
	return model.content_kind == AudioDisplayContentKind::Spectrum
		&& model.audio_bounds.width > 0
		&& model.audio_bounds.height > 0
		&& model.spectrum.bins_per_column > 0
		&& !model.spectrum.band_a.empty()
		&& !model.spectrum.band_b.empty()
		&& !model.spectrum.ready.empty()
		&& !model.spectrum.power.empty();
#else
	(void)model;
	return false;
#endif
}

bool AudioDisplaySkiaRenderer::DrawSpectrumContentToBitmap(wxBitmap &bitmap, AudioDisplayRenderModel const& model) const {
#ifdef WITH_SKIA
	if (!CanDrawSpectrumContent(model) || !bitmap.IsOk())
		return false;

	int const width = bitmap.GetWidth();
	int const height = bitmap.GetHeight();
	if (width <= 0 || height <= 0)
		return false;

	auto const& spectrum = model.spectrum;
	int const visible_x1 = spectrum.pixel_origin;
	int const visible_x2 = spectrum.pixel_origin + (spectrum.channel_count > 0
		? static_cast<int>(spectrum.ready.size()) / spectrum.channel_count
		: 0);
	if (visible_x2 <= visible_x1)
		return false;

	auto &pixels = GetSpectrumPixelScratch();
	size_t const pixel_count = static_cast<size_t>(width) * static_cast<size_t>(height);
	if (pixels.size() != pixel_count)
		pixels.resize(pixel_count);
	std::fill(pixels.begin(), pixels.end(), 0);

	auto fill_rect = [&](int x1, int y1, int x2, int y2, uint32_t colour) {
		x1 = std::max(0, x1);
		y1 = std::max(0, y1);
		x2 = std::min(width, x2);
		y2 = std::min(height, y2);
		if (x2 <= x1 || y2 <= y1)
			return;

		uint32_t const packed = colour;
		for (int y = y1; y < y2; ++y) {
			auto *row = pixels.data() + static_cast<size_t>(y) * static_cast<size_t>(width);
			std::fill(row + x1, row + x2, packed);
		}
	};

	ForEachVisibleStyleRange(model, visible_x1, visible_x2, [&](AudioRenderingStyle style, int range_x1, int range_x2) {
		fill_rect(range_x1, 0, range_x2, height, spectrum.palettes[static_cast<size_t>(style)].colours[0]);
	});

	auto const* band_frac = spectrum.interpolated ? spectrum.band_frac.data() : nullptr;
	int const channel_count = std::max(1, spectrum.channel_count);
	int const band_height = std::max(1, spectrum.channel_band_height);

	auto draw_channel_range = [&](AudioRenderingStyle style, int range_x1, int range_x2) {
		auto const& palette = spectrum.palettes[static_cast<size_t>(style)];
		for (int x = std::max(range_x1, visible_x1); x < std::min(range_x2, visible_x2); ++x) {
			int const local_x = x - visible_x1;
			for (int channel = 0; channel < channel_count; ++channel) {
				size_t const ready_index = static_cast<size_t>(channel * (visible_x2 - visible_x1) + local_x);
				if (ready_index >= spectrum.ready.size() || !spectrum.ready[ready_index])
					continue;

				float const* power = spectrum.power.data()
					+ (ready_index * static_cast<size_t>(spectrum.bins_per_column));
				int const y_offset = channel * band_height;
				for (int y = 0; y < band_height && y_offset + y < height; ++y) {
					float value = 0.0f;
					if (spectrum.interpolated) {
						float const frac = band_frac[y];
						value = (1.0f - frac) * power[spectrum.band_a[y]] + frac * power[spectrum.band_b[y]];
					}
					else {
						int const first = spectrum.band_a[y];
						int const last = spectrum.band_b[y];
						value = power[first];
						for (int i = first + 1; i <= last; ++i)
							value = std::max(value, power[i]);
					}

					float scaled = std::clamp(value * spectrum.amplitude_scale, 0.0f, 1.0f);
					size_t palette_index = static_cast<size_t>(scaled * static_cast<float>(AudioDisplaySpectrumPaletteSize - 1));
					uint32_t const colour = palette.colours[palette_index];
					pixels[static_cast<size_t>(y_offset + (band_height - 1 - y)) * static_cast<size_t>(width) + static_cast<size_t>(x)] = colour;
				}
			}
		}
	};

	ForEachVisibleStyleRange(model, visible_x1, visible_x2, draw_channel_range);

	if (channel_count > 1) {
		uint32_t const divider = spectrum.channel_divider;
		for (int channel = 1; channel < channel_count; ++channel) {
			int const y = channel * band_height;
			if (y < 0 || y >= height)
				continue;
			auto *row = pixels.data() + static_cast<size_t>(y) * static_cast<size_t>(width);
			std::fill(row, row + width, divider);
		}
	}

	return CopyBgraPixelsToBitmap(pixels, width, height, bitmap);
#else
	(void)bitmap;
	(void)model;
	return false;
#endif
}
