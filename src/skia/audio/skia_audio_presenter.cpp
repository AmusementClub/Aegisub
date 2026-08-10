#include "skia_audio_presenter.h"

#include "../../perf_trace.h"
#include "../../skia_runtime/platform_font_runtime.h"
#include "../../skia_runtime/skia_surface_provider.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
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
#include <include/core/SkFontMetrics.h>
#include <include/core/SkImage.h>
#include <include/core/SkImageInfo.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPicture.h>
#include <include/core/SkPictureRecorder.h>
#include <include/core/SkRect.h>
#include <include/core/SkSamplingOptions.h>
#include <include/core/SkString.h>
#include <include/core/SkSurface.h>
#include <include/core/SkTypeface.h>
#include <include/effects/SkGradient.h>
#include <include/effects/SkRuntimeEffect.h>
#include <include/gpu/GpuTypes.h>
#include <include/gpu/ganesh/GrDirectContext.h>
#include <include/gpu/ganesh/SkImageGanesh.h>
#include <include/gpu/ganesh/SkSurfaceGanesh.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <optional>
#include <queue>
#include <span>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aegisub::skia::audio {
namespace {

constexpr std::size_t kDefaultContentCacheBudget = 32 * 1024 * 1024;
constexpr std::size_t kGaneshResourceCacheBudget = 64 * 1024 * 1024;

using FrameTraceClock = std::chrono::steady_clock;

// Resolve a typeface for the given family name, cached because every repaint
// asks for the same two or three faces. An empty family means "the platform
// default UI face", which is what legacy gets from the window's wxFont.
sk_sp<SkTypeface> ResolveAudioTypeface(std::string const& family, bool bold) {
	return PlatformFontRuntime::Get().ResolveTypeface({
		family,
		bold ? SkFontStyle::kBold_Weight : SkFontStyle::kNormal_Weight,
		SkFontStyle::kNormal_Width,
		SkFontStyle::kUpright_Slant,
	});
}

// Build the SkFont for one draw site from the display's base text style. Returns
// nullopt when no typeface resolves, so callers skip the draw instead of
// emitting invisible glyphs from an empty typeface.
//
// face_override mirrors legacy's per-site wxFont::SetFaceName (only the track
// cursor uses it); bold mirrors its wxFONTWEIGHT_BOLD.
std::optional<SkFont> MakeAudioFont(
	TextStyleFrame const& style,
	bool bold,
	std::string const& face_override = {}) {
	auto const& family = face_override.empty() ? style.face : face_override;
	auto typeface = ResolveAudioTypeface(family, bold);
	if (!typeface && !face_override.empty()) {
		// A configured face that does not exist should not cost us the label.
		typeface = ResolveAudioTypeface(style.face, bold);
	}
	if (!typeface)
		return std::nullopt;

	auto const size = std::isfinite(style.size) && style.size > 0.f
		? std::clamp(style.size, 4.f, 256.f)
		: 11.f;
	SkFont font(std::move(typeface), size);
	font.setEdging(SkFont::Edging::kAntiAlias);
	// Legacy renders through wxDC, which snaps glyph origins to whole pixels.
	font.setSubpixel(false);
	// Synthesise bold when the resolved face has no real bold variant, which is
	// what GDI/DirectWrite do for legacy.
	if (bold && font.getTypeface() && !font.getTypeface()->isBold())
		font.setEmbolden(true);
	return font;
}

// wxDC draws a 1px line on one exact pixel column. Skia's drawLine centres a
// 1px stroke on the coordinate, so it straddles two columns and lands a half
// pixel left of where legacy puts it. Thin timeline rules and ticks are drawn as
// pixel-snapped rects instead so both renderers light the same columns.
void FillDeviceRect(
	SkCanvas *canvas, float x, float y, float width, float height, SkPaint const& paint) {
	canvas->drawRect(
		SkRect::MakeXYWH(
			std::floor(x),
			std::floor(y),
			std::max(1.f, std::round(width)),
			std::max(1.f, std::round(height))),
		paint);
}

// wxDC::DrawText anchors text by its top-left corner; Skia anchors it by the
// baseline. fAscent is negative, hence the subtraction.
float TextBaselineForTop(SkFont const& font, float top) {
	SkFontMetrics metrics;
	font.getMetrics(&metrics);
	return top - metrics.fAscent;
}

float TextLineHeight(SkFont const& font) {
	SkFontMetrics metrics;
	font.getMetrics(&metrics);
	return metrics.fDescent - metrics.fAscent;
}

// Device pixels per logical pixel, used to scale the pixel offsets legacy
// hardcodes for a 1x display.
float FrameContentScale(ContentFrame const& frame) noexcept {
	return std::isfinite(frame.content_scale)
		? std::clamp(frame.content_scale, 1.f, 8.f)
		: 1.f;
}

FrameTraceClock::time_point BeginFrameTrace(bool enabled) noexcept {
	return enabled ? FrameTraceClock::now() : FrameTraceClock::time_point {};
}

double EndFrameTrace(FrameTraceClock::time_point started) noexcept {
	if (started == FrameTraceClock::time_point {})
		return -1.0;
	return std::chrono::duration<double, std::milli>(FrameTraceClock::now() - started).count();
}

perf_trace::AudioContentTileEvent MakeTileEvent(
	char const *stage,
	ContentTileKey const& key) noexcept {
	perf_trace::AudioContentTileEvent event;
	event.stage = stage;
	event.spectrum = key.kind == ContentKind::Spectrum;
	event.provider_generation = key.generation.provider;
	event.analysis_generation = key.generation.analysis;
	event.tile_index = key.tile_index;
	event.column_count = key.column_count;
	event.spectrum_bin_count = key.spectrum_bin_count;
	return event;
}

perf_trace::AudioContentTileEvent MakePayloadEvent(
	char const *stage,
	ContentUploadPayloadKey const& key) noexcept {
	auto event = MakeTileEvent(stage, key.tile);
	event.variant_revision = key.variant_revision;
	return event;
}

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
		if (!IsValidDeviceStyleSpan(frame.x, frame.width, style.x, style.width))
			return "an Audio rendering style span is invalid";
		if (frame.kind == ContentKind::Spectrum
			&& (!style.spectrum_palette || !style.spectrum_palette->revision))
			return "an Audio spectrum rendering style palette is missing";
	}
	return {};
}

enum class FrameLayerPart : std::uint32_t {
	None = 0,
	Timeline = 1u << 0,
	Marker = 1u << 1,
	CursorLine = 1u << 2,
	TimingLabel = 1u << 3,
	CursorLabel = 1u << 4,
	Scrollbar = 1u << 5,
	All = (1u << 6) - 1,
};

constexpr FrameLayerPart operator|(FrameLayerPart lhs, FrameLayerPart rhs) noexcept {
	return static_cast<FrameLayerPart>(
		static_cast<std::uint32_t>(lhs) | static_cast<std::uint32_t>(rhs));
}

constexpr bool HasFrameLayerPart(FrameLayerPart mask, FrameLayerPart part) noexcept {
	return (static_cast<std::uint32_t>(mask) & static_cast<std::uint32_t>(part)) != 0;
}

void DrawAudioFrameLayers(
	SkCanvas *canvas,
	FrameTarget const& target,
	ContentFrame const& frame,
	PresenterFrameTrace *frame_trace,
	FrameLayerPart parts = FrameLayerPart::All) {
	if (!canvas)
		return;

	SkPaint paint;
	paint.setAntiAlias(false);

	// Timeline is deliberately drawn after content in the same canvas submit.
	// Its scroll origin is expressed in device pixels, matching FrameViewport.
	if (HasFrameLayerPart(parts, FrameLayerPart::Timeline)
		&& frame.timeline && frame.timeline->height > 0) {
		auto const timeline_y = static_cast<float>(frame.timeline->y);
		auto const timeline_height = static_cast<float>(frame.timeline->height);
		auto const timeline_bottom = timeline_y + timeline_height;
		auto const scale = FrameContentScale(frame);
		paint.setColor(static_cast<SkColor>(frame.timeline->background_color));
		canvas->drawRect(SkRect::MakeXYWH(frame.x, timeline_y, frame.width, timeline_height), paint);

		paint.setColor(static_cast<SkColor>(frame.timeline->foreground_color));

		// Legacy's "Top line": a full-width rule along the bottom of the timeline
		// band, separating it from the waveform.
		FillDeviceRect(canvas, frame.x, timeline_bottom - scale, frame.width, scale, paint);

		auto const ms_per_pixel = frame.timeline->milliseconds_per_pixel;
		auto const plan = BuildTimelineScalePlan(ms_per_pixel);
		if (plan.valid) {
			auto const scroll_left = std::isfinite(frame.timeline->scroll_left_exact)
				&& (frame.timeline->scroll_left_exact != 0.0 || frame.timeline->scroll_left == 0)
				? frame.timeline->scroll_left_exact
				: static_cast<double>(frame.timeline->scroll_left);
			auto const marks = BuildTimelineMarks(plan, scroll_left, frame.width, ms_per_pixel);
			auto const font = MakeAudioFont(frame.text_style, false);
			TimelineLabelFormatter formatter(plan.scale, frame.timeline->duration_ms);
			// Clip to the timeline band so an edge label is truncated rather than
			// dropped, which is what wxDC does for the legacy timeline.
			canvas->save();
			canvas->clipRect(SkRect::MakeXYWH(frame.x, timeline_y, frame.width, timeline_height));
			SkPaint text_paint(paint);
			text_paint.setAntiAlias(true);
			double last_text_right = -1.0;
			for (auto const& mark : marks) {
				// Legacy tick heights: major bottom-6..bottom-1, minor
				// bottom-4..bottom-1, i.e. 5px and 3px stopping one pixel short of
				// the rule. Scaled so they keep their proportions on HiDPI.
				auto const tick_height = (mark.major ? 5.f : 3.f) * scale;
				FillDeviceRect(
					canvas,
					frame.x + static_cast<float>(mark.x),
					timeline_bottom - scale - tick_height,
					scale,
					tick_height,
					paint);
				// Legacy only formats a label when it will actually be drawn, so
				// the hour/minute suppression state advances on drawn labels only.
				if (!mark.major || !font || mark.x <= last_text_right)
					continue;
				auto const label = formatter.Format(mark.time_ms);
				if (label.empty())
					continue;
				last_text_right = mark.x + font->measureText(
					label.data(), label.size(), SkTextEncoding::kUTF8);
				canvas->drawSimpleText(
					label.data(), label.size(), SkTextEncoding::kUTF8,
					std::floor(frame.x + static_cast<float>(mark.x)),
					TextBaselineForTop(*font, timeline_y),
					*font,
					text_paint);
			}
			canvas->restore();
		}
	}

	// Marker lines, feet and labels are all batched into this frame. The frame
	// builder supplies device-space x coordinates, so no extra time conversion
	// or intermediate surface is needed here.
	if (HasFrameLayerPart(parts, FrameLayerPart::Marker)
		|| HasFrameLayerPart(parts, FrameLayerPart::CursorLine)) {
		canvas->save();
		canvas->clipRect(SkRect::MakeXYWH(frame.x, frame.y, frame.width, frame.height));
		if (HasFrameLayerPart(parts, FrameLayerPart::Marker)) {
			auto const marker_trace_started = BeginFrameTrace(frame_trace != nullptr);
			int markers_drawn = 0;
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
				++markers_drawn;
			}
			if (frame_trace) {
				frame_trace->marker_layer_rebuild_ms = EndFrameTrace(marker_trace_started);
				frame_trace->marker_count = static_cast<int>(frame.markers.size());
				frame_trace->markers_drawn = markers_drawn;
			}
		}
		if (HasFrameLayerPart(parts, FrameLayerPart::CursorLine)
			&& frame.cursor && std::isfinite(frame.cursor->x)) {
			paint.setColor(static_cast<SkColor>(frame.cursor->color));
			paint.setStrokeWidth(1.f);
			canvas->drawLine(frame.cursor->x, frame.y, frame.cursor->x, frame.y + frame.height, paint);
		}
		canvas->restore();
	}

	if ((HasFrameLayerPart(parts, FrameLayerPart::TimingLabel) && !frame.labels.empty())
		|| (HasFrameLayerPart(parts, FrameLayerPart::CursorLabel)
			&& frame.cursor && !frame.cursor->label.empty())) {
		auto const label_trace_started = BeginFrameTrace(
			frame_trace && HasFrameLayerPart(parts, FrameLayerPart::TimingLabel)
			&& !frame.labels.empty());
		// Legacy PaintLabels bolds the window font; PaintTrackCursor does the same
		// on top of its optional face override.
		auto const scale = FrameContentScale(frame);
		auto const bold_font = MakeAudioFont(frame.text_style, true);
		paint.setAntiAlias(true);
		paint.setColor(SK_ColorWHITE);
		int labels_drawn = 0;
		if (HasFrameLayerPart(parts, FrameLayerPart::TimingLabel) && bold_font) {
			// Legacy anchors timing labels 4px below the top of the waveform.
			auto const label_top = frame.y + 4.f * scale;
			auto const baseline = TextBaselineForTop(*bold_font, label_top);
			auto const line_height = TextLineHeight(*bold_font);
			for (auto const& label : frame.labels) {
				if (label.text.empty() || !std::isfinite(label.x)
					|| !std::isfinite(label.width) || label.width <= 0.f)
					continue;
				auto const text_width = bold_font->measureText(
					label.text.data(), label.text.size(), SkTextEncoding::kUTF8);
				if (label.width < text_width) {
					// Too narrow for the text: truncate it, as legacy does.
					canvas->save();
					canvas->clipRect(SkRect::MakeXYWH(label.x, label_top, label.width, line_height));
					canvas->drawSimpleText(label.text.data(), label.text.size(), SkTextEncoding::kUTF8,
						label.x, baseline, *bold_font, paint);
					canvas->restore();
				}
				else {
					// Otherwise centre it in the range.
					canvas->drawSimpleText(label.text.data(), label.text.size(), SkTextEncoding::kUTF8,
						std::floor(label.x + (label.width - text_width) * 0.5f),
						baseline, *bold_font, paint);
				}
				++labels_drawn;
			}
		}
		if (frame_trace && HasFrameLayerPart(parts, FrameLayerPart::TimingLabel)
			&& !frame.labels.empty()) {
			frame_trace->label_layer_rebuild_ms = EndFrameTrace(label_trace_started);
			frame_trace->label_count = static_cast<int>(frame.labels.size());
			frame_trace->labels_drawn = labels_drawn;
		}
		if (HasFrameLayerPart(parts, FrameLayerPart::CursorLabel)
			&& frame.cursor && !frame.cursor->label.empty() && std::isfinite(frame.cursor->x)) {
			// The cursor label is always bold (legacy PaintTrackCursor sets
			// wxFONTWEIGHT_BOLD unconditionally), with an optional face override
			// from "Audio/Track Cursor/Font Face".
			auto const cursor_font = MakeAudioFont(frame.text_style, true, frame.cursor->font_face);
			if (cursor_font) {
				auto const& text = frame.cursor->label;
				auto const text_width = cursor_font->measureText(
					text.data(), text.size(), SkTextEncoding::kUTF8);
				// Legacy centres the label on the cursor, then keeps it inside the
				// client area with a 2px margin on both sides.
				auto const margin = 2.f * scale;
				auto const limit = std::max(
					margin, static_cast<float>(target.width) - text_width - margin);
				auto const label_x = std::clamp(frame.cursor->x - text_width * 0.5f, margin, limit);
				auto const label_top = frame.y + margin;
				auto const baseline = TextBaselineForTop(*cursor_font, label_top);
				auto const draw = [&](float dx, float dy) {
					canvas->drawSimpleText(
						text.data(), text.size(), SkTextEncoding::kUTF8,
						std::floor(label_x) + dx, baseline + dy, *cursor_font, paint);
				};
				// Legacy draws the label four times offset by one pixel in a dark
				// grey to outline it, then once in white on top, so it stays
				// readable over a bright waveform.
				paint.setColor(SkColorSetRGB(64, 64, 64));
				draw(-scale, -scale);
				draw(-scale, scale);
				draw(scale, -scale);
				draw(scale, scale);
				paint.setColor(SK_ColorWHITE);
				draw(0.f, 0.f);
			}
		}
	}

	if (HasFrameLayerPart(parts, FrameLayerPart::Scrollbar)
		&& frame.scrollbar && frame.scrollbar->height > 0) {
		auto const scrollbar_trace_started = BeginFrameTrace(frame_trace != nullptr);
		auto const scrollbar = *frame.scrollbar;
		auto const y = static_cast<float>(std::clamp(scrollbar.y, 0, target.height - scrollbar.height));
		auto const h = static_cast<float>(scrollbar.height);
		auto const scale = std::isfinite(scrollbar.content_scale)
			? std::clamp(scrollbar.content_scale, 1.f, 8.f) : 1.f;
		auto const track = std::max(1.f, static_cast<float>(target.width));
		auto const geometry = BuildScrollbarGeometry(
			track,
			10.f * scale,
			25.f * scale,
			scrollbar.total,
			scrollbar.page,
			scrollbar.position,
			scrollbar.load_position,
			scrollbar.selection_start,
			scrollbar.selection_length);
		if (!geometry.valid) {
			if (frame_trace)
				frame_trace->scrollbar_layer_rebuild_ms = EndFrameTrace(scrollbar_trace_started);
			return;
		}

		canvas->save();
		canvas->clipRect(SkRect::MakeXYWH(0.f, y, track, h));
		paint.reset();
		paint.setAntiAlias(false);
		paint.setColor(static_cast<SkColor>(scrollbar.background_color));
		canvas->drawRect(SkRect::MakeXYWH(0.f, y, track, h), paint);

		// Match wx AudioDisplayScrollbar z-order: selection is part of the
		// track and must never obscure the load marker or draggable thumb.
		if (geometry.selection_visible) {
			paint.setColor(static_cast<SkColor>(scrollbar.selection_color));
			canvas->drawRect(SkRect::MakeXYWH(
				geometry.selection_x, y, geometry.selection_width, h), paint);
		}

		auto const border_width = std::max(1.f, scale);
		auto const border_inset = border_width * 0.5f;
		paint.setStyle(SkPaint::kStroke_Style);
		paint.setStrokeWidth(border_width);
		paint.setColor(static_cast<SkColor>(scrollbar.thumb_color));
		canvas->drawRect(SkRect::MakeLTRB(
			border_inset,
			y + border_inset,
			std::max(border_inset, track - border_inset),
			std::max(y + border_inset, y + h - border_inset)), paint);

		if (geometry.load_visible) {
			std::array<SkColor4f, 2> colors {
				SkColor4f::FromColor(static_cast<SkColor>(scrollbar.background_color)),
				SkColor4f::FromColor(static_cast<SkColor>(scrollbar.thumb_color)),
			};
			SkPoint points[] {
				{ geometry.load_x, y },
				{ geometry.load_x + geometry.load_width, y },
			};
			SkGradient gradient(
				SkGradient::Colors(SkSpan<const SkColor4f>(colors), SkTileMode::kClamp),
				{});
			paint.reset();
			paint.setAntiAlias(false);
			paint.setShader(SkShaders::LinearGradient(points, gradient));
			canvas->drawRect(SkRect::MakeXYWH(
				geometry.load_x,
				y + scale,
				geometry.load_width,
				std::max(1.f, h - 2.f * scale)), paint);
		}

		paint.reset();
		paint.setAntiAlias(false);
		paint.setColor(static_cast<SkColor>(scrollbar.thumb_color));
		canvas->drawRect(SkRect::MakeXYWH(
			geometry.thumb_x, y, geometry.thumb_width, h), paint);
		canvas->restore();
		if (frame_trace) {
			frame_trace->scrollbar_layer_rebuild_ms = EndFrameTrace(scrollbar_trace_started);
			frame_trace->scrollbar_selection_visible = geometry.selection_visible;
			frame_trace->scrollbar_load_visible = geometry.load_visible;
		}
	}
}

void DrawTimelineLayer(SkCanvas *canvas, ContentFrame const& frame) {
	DrawAudioFrameLayers(canvas, {}, frame, nullptr, FrameLayerPart::Timeline);
}

void DrawMarkerLayer(SkCanvas *canvas, ContentFrame const& frame, PresenterFrameTrace *trace) {
	DrawAudioFrameLayers(canvas, {}, frame, trace, FrameLayerPart::Marker);
}

void DrawTimingLabelLayer(SkCanvas *canvas, ContentFrame const& frame, PresenterFrameTrace *trace) {
	DrawAudioFrameLayers(canvas, {}, frame, trace, FrameLayerPart::TimingLabel);
}

// The target is needed here: legacy clamps the cursor time label to the client
// width so it stays fully on screen when the cursor is near either edge.
void DrawCursorLayer(
	SkCanvas *canvas, FrameTarget const& target, ContentFrame const& frame, bool label) {
	DrawAudioFrameLayers(
		canvas,
		target,
		frame,
		nullptr,
		label ? FrameLayerPart::CursorLabel : FrameLayerPart::CursorLine);
}

void DrawScrollbarLayer(
	SkCanvas *canvas,
	FrameTarget const& target,
	ContentFrame const& frame,
	PresenterFrameTrace *trace) {
	DrawAudioFrameLayers(canvas, target, frame, trace, FrameLayerPart::Scrollbar);
}

sk_sp<SkPicture> RecordLayerPicture(
	FrameTarget const& target,
	std::function<void(SkCanvas *)> const& draw) {
	SkPictureRecorder recorder;
	auto *canvas = recorder.beginRecording(
		static_cast<SkScalar>(target.width),
		static_cast<SkScalar>(target.height));
	if (!canvas)
		return nullptr;
	draw(canvas);
	return recorder.finishRecordingAsPicture();
}

}

struct Presenter::Impl {
	struct GpuContentEntry {
		sk_sp<SkImage> primary;
		sk_sp<SkImage> secondary;
		std::size_t bytes = 0;
		std::uint64_t touch = 0;
	};
	struct GpuContentTouch {
		std::uint64_t touch = 0;
		ContentUploadPayloadKey key;
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
	std::unordered_map<ContentUploadPayloadKey, GpuContentEntry, ContentUploadPayloadKeyHash> content_cache;
	std::priority_queue<GpuContentTouch, std::vector<GpuContentTouch>, std::greater<GpuContentTouch>> content_touches;
	std::size_t content_cache_budget = kDefaultContentCacheBudget;
	std::size_t content_cache_bytes = 0;
	std::uint64_t content_touch_counter = 0;
	sk_sp<SkRuntimeEffect> spectrum_effect;
	std::string spectrum_effect_error;
	std::unordered_map<std::uint64_t, sk_sp<SkImage>> palette_images;
	std::uint64_t retained_static_revision = 0;
	SurfaceKey retained_surface_key;
	bool retained_layers_ready = false;
	sk_sp<SkImage> retained_base_image;
	sk_sp<SkPicture> retained_timeline;
	sk_sp<SkPicture> retained_markers;
	sk_sp<SkPicture> retained_timing_labels;
	sk_sp<SkPicture> retained_post_cursor;
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
		palette_images.clear();
		UpdateContentMetrics();
	}

	void ResetRetainedLayers() noexcept {
		retained_static_revision = 0;
		retained_surface_key = {};
		retained_layers_ready = false;
		retained_base_image.reset();
		retained_timeline.reset();
		retained_markers.reset();
		retained_timing_labels.reset();
		retained_post_cursor.reset();
	}

	void Fail(SkiaGlContextToken context, SkiaGlDeviceFailure failure, std::string detail) noexcept {
		last_context = context;
		surface.reset();
		surface_key.reset();
		ResetContentCache();
		ResetRetainedLayers();
		device.Fail(context, failure, std::move(detail));
	}

	void TouchContent(ContentUploadPayloadKey const& key, GpuContentEntry& entry) {
		entry.touch = ++content_touch_counter;
		content_touches.push({ entry.touch, key });
		if (content_touches.size() > content_cache.size() * 4 + 64) {
			decltype(content_touches) compacted;
			for (auto const& [current_key, current_entry] : content_cache)
				compacted.push({ current_entry.touch, current_key });
			content_touches.swap(compacted);
		}
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
				auto event = MakePayloadEvent("gpu_evict", entry->first);
				event.outcome = "budget";
				event.bytes = entry->second.bytes;
				content_cache_bytes -= entry->second.bytes;
				content_cache.erase(entry);
				++metrics.content_evictions;
				perf_trace::ObserveAudioContentTileEvent(event);
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
		perf_trace::AudioUiDurationScope trace("audio_display.submit");
		if (!device.FlushAndSubmit(context)) {
			trace.SetDetails(0);
			device.ResetTextureBindingsForExternalUse(context);
			return false;
		}
		++metrics.submits;
		device.ResetTextureBindingsForExternalUse(context);
		trace.SetDetails(1);
		return true;
	}

	std::optional<GpuContentEntry> UploadContentTile(
		ContentUploadPayload const& payload) {
		auto *context = device.Get();
		if (!context || !payload.IsValid())
			return std::nullopt;

		GpuContentEntry uploaded;
		if (payload.key.tile.kind == ContentKind::Waveform) {
			auto const info = SkImageInfo::MakeA8(
				static_cast<int>(payload.width),
				static_cast<int>(payload.height));
			uploaded.primary = UploadTexture(
				context,
				info,
				payload.primary.data(),
				payload.primary.size(),
				payload.width);
			uploaded.secondary = UploadTexture(
				context,
				info,
				payload.secondary.data(),
				payload.secondary.size(),
				payload.width);
			if (!uploaded.primary || !uploaded.secondary)
				return std::nullopt;
			uploaded.bytes = uploaded.primary->textureSize() + uploaded.secondary->textureSize();
			if (!uploaded.bytes)
				uploaded.bytes = payload.primary.size() + payload.secondary.size();
		}
		else {
			auto const info = SkImageInfo::Make(
				static_cast<int>(payload.width),
				static_cast<int>(payload.height),
				kRGBA_8888_SkColorType,
				kOpaque_SkAlphaType,
				nullptr);
			uploaded.primary = UploadTexture(
				context,
				info,
				payload.primary.data(),
				payload.primary.size(),
				static_cast<std::size_t>(payload.width) * 4);
			if (!uploaded.primary)
				return std::nullopt;
			uploaded.bytes = uploaded.primary->textureSize();
			if (!uploaded.bytes)
				uploaded.bytes = payload.primary.size();
		}
		return uploaded;
	}

	std::optional<GpuContentEntry> AcquireContentTile(
		ContentUploadPayload const& payload) {
		auto found = content_cache.find(payload.key);
		if (found != content_cache.end()) {
			TouchContent(found->first, found->second);
			++metrics.content_cache_hits;
			return found->second;
		}

		++metrics.content_cache_misses;
		if (!payload.IsValid())
			return std::nullopt;
		auto uploaded = UploadContentTile(payload);
		if (!uploaded)
			return std::nullopt;
		if (uploaded->bytes > content_cache_budget)
			return std::nullopt;

		auto const key = payload.key;
		auto [entry, inserted] = content_cache.emplace(key, std::move(*uploaded));
		if (!inserted)
			return entry->second;
		content_cache_bytes += entry->second.bytes;
		metrics.content_upload_bytes += entry->second.bytes;
		++metrics.content_uploads;
		auto event = MakePayloadEvent("gpu_upload", entry->first);
		event.outcome = "uploaded";
		event.bytes = entry->second.bytes;
		perf_trace::ObserveAudioContentTileEvent(event);
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
		if (palette_images.size() >= 8)
			palette_images.erase(palette_images.begin());
		auto [entry, inserted] = palette_images.emplace(palette.revision, std::move(uploaded));
		if (!inserted)
			return entry->second;
		++metrics.palette_uploads;
		return entry->second;
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
	impl->metrics.last_frame_trace = {};
	if (!impl->PrepareFrame(context, target))
		return false;
	if (auto const error = ValidateContentFrame(target, frame); !error.empty()) {
		impl->Fail(context, SkiaGlDeviceFailure::InvalidFrameTarget, error);
		return false;
	}

	auto const trace_enabled = perf_trace::IsCategoryEnabled(perf_trace::Category::Audio);
	PresenterFrameTrace frame_trace;
	auto *trace = trace_enabled ? &frame_trace : nullptr;
	auto const compose_trace_started = BeginFrameTrace(trace_enabled);
	auto const base_trace_started = BeginFrameTrace(trace_enabled);
	auto *canvas = impl->surface->getCanvas();
	canvas->clear(static_cast<SkColor>(frame.background_color));
	SkRect const content_bounds = SkRect::MakeXYWH(
		static_cast<float>(frame.x),
		static_cast<float>(frame.y),
		static_cast<float>(frame.width),
		static_cast<float>(frame.height));
	SkPaint paint;
	paint.setAntiAlias(false);
	StyleFrame default_style;
	std::span<StyleFrame const> styles = frame.styles;
	if (styles.empty()) {
		default_style.x = frame.x;
		default_style.width = frame.width;
		default_style.background_color = frame.background_color;
		default_style.waveform_peak_color = frame.waveform_peak_color;
		default_style.waveform_average_color = frame.waveform_average_color;
		default_style.waveform_zero_color = frame.waveform_zero_color;
		default_style.spectrum_palette = frame.spectrum_palette;
		styles = std::span<StyleFrame const>(&default_style, 1);
	}
	for (auto const& style : styles) {
		paint.setColor(static_cast<SkColor>(style.background_color));
		canvas->drawRect(SkRect::MakeXYWH(style.x, frame.y, style.width, frame.height), paint);
	}

	if (frame.kind == ContentKind::Spectrum) {
		if (!impl->spectrum_effect) {
			impl->Fail(
				context,
				SkiaGlDeviceFailure::ContentShaderUnavailable,
				"Skia spectrum runtime effect failed to compile: " + impl->spectrum_effect_error);
			return false;
		}
		for (auto const& style : styles) {
			if (!impl->AcquirePalette(*style.spectrum_palette)) {
				impl->Fail(context, SkiaGlDeviceFailure::ContentUploadFailed, "failed to upload an Audio spectrum style palette");
				return false;
			}
		}
	}

	canvas->save();
	canvas->clipRect(content_bounds);
	for (auto const& tile : frame.tiles) {
		if (!tile
			|| !tile->IsValid()
			|| tile->key.tile.generation != frame.generation
			|| tile->key.tile.kind != frame.kind
			|| (frame.kind == ContentKind::Waveform
				&& tile->key.variant_revision != kWaveformUploadPayloadRevision)
			|| (frame.kind == ContentKind::Spectrum
				&& (tile->key.tile.spectrum_bin_count != frame.spectrum_band_plan->bin_count
					|| tile->key.variant_revision != frame.spectrum_band_plan->revision
					|| tile->height != static_cast<std::uint32_t>(frame.spectrum_band_plan->output_height)))
			|| tile->key.tile.tile_index
				> std::numeric_limits<std::uint64_t>::max() / tile->key.tile.column_count) {
			++impl->metrics.content_tiles_skipped;
			continue;
		}

		auto const& tile_key = tile->key.tile;
		auto const tile_first = tile_key.tile_index * tile_key.column_count;
		double relative_x = tile_first >= frame.first_column
			? static_cast<double>(tile_first - frame.first_column)
			: -static_cast<double>(frame.first_column - tile_first);
		if (relative_x >= frame.width
			|| relative_x + tile_key.column_count <= 0.0) {
			continue;
		}

		auto gpu_tile = impl->AcquireContentTile(*tile);
		if (!gpu_tile) {
			impl->Fail(context, SkiaGlDeviceFailure::ContentUploadFailed, "failed to upload or retain an Audio content tile");
			return false;
		}
		auto const destination_x = frame.x + frame.first_column_offset + static_cast<float>(relative_x);

		if (frame.kind == ContentKind::Waveform) {
			auto const amplitude = std::clamp(frame.amplitude, 0.f, 64.f);
			auto const scaled_height = frame.height * amplitude;
			SkRect const source = SkRect::MakeWH(
				static_cast<float>(tile_key.column_count),
				static_cast<float>(kWaveformUploadMaskHeight));
			SkRect const destination = SkRect::MakeXYWH(
				destination_x,
				frame.y + (frame.height - scaled_height) * 0.5f,
				static_cast<float>(tile_key.column_count),
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
				static_cast<float>(tile_key.column_count),
				frame.height);
			for (auto const& style : styles) {
				auto const style_bounds = SkRect::MakeXYWH(style.x, frame.y, style.width, frame.height);
				if (!SkRect::Intersects(style_bounds, destination))
					continue;
				auto power_shader = gpu_tile->primary->makeRawShader(
					SkSamplingOptions(SkFilterMode::kLinear),
					nullptr);
				auto palette = impl->AcquirePalette(*style.spectrum_palette);
				if (!palette) {
					impl->Fail(context, SkiaGlDeviceFailure::ContentUploadFailed, "failed to retain an Audio spectrum style palette");
					return false;
				}
				auto palette_shader = palette->makeShader(
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
					static_cast<float>(tile_key.column_count),
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
	auto retained_base_image = frame.static_revision
		? impl->surface->makeImageSnapshot()
		: sk_sp<SkImage>{};
	if (trace) {
		trace->base_layer_rebuild_ms = EndFrameTrace(base_trace_started);
		trace->tile_count = static_cast<int>(frame.tiles.size());
		trace->style_count = static_cast<int>(styles.size());
	}
	DrawTimelineLayer(canvas, frame);
	DrawMarkerLayer(canvas, frame, trace);
	DrawCursorLayer(canvas, target, frame, false);
	DrawTimingLabelLayer(canvas, frame, trace);
	DrawCursorLayer(canvas, target, frame, true);
	DrawScrollbarLayer(canvas, target, frame, trace);

	if (frame.static_revision) {
		auto timeline = RecordLayerPicture(target, [&frame](SkCanvas *recording) {
			DrawTimelineLayer(recording, frame);
		});
		auto markers = RecordLayerPicture(target, [&frame](SkCanvas *recording) {
			DrawMarkerLayer(recording, frame, nullptr);
		});
		auto timing_labels = RecordLayerPicture(target, [&frame](SkCanvas *recording) {
			DrawTimingLabelLayer(recording, frame, nullptr);
		});
		auto post_cursor = RecordLayerPicture(target, [&target, &frame](SkCanvas *recording) {
			DrawScrollbarLayer(recording, target, frame, nullptr);
		});
		if (!retained_base_image || !timeline || !markers || !timing_labels || !post_cursor) {
			impl->ResetRetainedLayers();
		}
		else {
			impl->retained_static_revision = frame.static_revision;
			impl->retained_surface_key = MakeSurfaceKey(target);
			impl->retained_base_image = std::move(retained_base_image);
			impl->retained_timeline = std::move(timeline);
			impl->retained_markers = std::move(markers);
			impl->retained_timing_labels = std::move(timing_labels);
			impl->retained_post_cursor = std::move(post_cursor);
			impl->retained_layers_ready = true;
		}
	}
	else {
		impl->ResetRetainedLayers();
	}
	if (trace) {
		trace->frame_compose_ms = EndFrameTrace(compose_trace_started);
		trace->valid = true;
	}
	auto const finished = impl->FinishFrame(context);
	if (finished && trace)
		impl->metrics.last_frame_trace = frame_trace;
	return finished;
}
catch (std::exception const& err) {
	impl->Fail(context, SkiaGlDeviceFailure::ContentUploadFailed, err.what());
	return false;
}
catch (...) {
	impl->Fail(context, SkiaGlDeviceFailure::ContentUploadFailed, "an unknown exception escaped Audio retained content rendering");
	return false;
}

bool Presenter::RenderCursorFrame(
	SkiaGlContextToken context,
	FrameTarget const& target,
	ContentFrame const& frame) {
	return RenderRetainedOverlayFrame(context, target, frame, Layer::Cursor);
}

bool Presenter::RenderRetainedOverlayFrame(
	SkiaGlContextToken context,
	FrameTarget const& target,
	ContentFrame const& frame,
	Layer updated_layers) try {
	++impl->metrics.frame_attempts;
	impl->metrics.last_frame_trace = {};
	if (!CanRenderRetainedOverlay(updated_layers)
		|| !frame.static_revision
		|| !impl->retained_layers_ready
		|| impl->retained_static_revision != frame.static_revision
		|| impl->retained_surface_key != MakeSurfaceKey(target)
		|| !impl->retained_base_image
		|| !impl->retained_timeline
		|| !impl->retained_markers
		|| !impl->retained_timing_labels
		|| !impl->retained_post_cursor) {
		return false;
	}
	sk_sp<SkPicture> updated_markers;
	auto const trace_enabled = perf_trace::IsCategoryEnabled(perf_trace::Category::Audio);
	PresenterFrameTrace frame_trace;
	frame_trace.retained_layers_reused = true;
	frame_trace.cursor_only = updated_layers == Layer::Cursor;
	frame_trace.base_layer_rebuild_ms = -1.0;
	if (HasLayer(updated_layers, Layer::Marker)) {
		updated_markers = RecordLayerPicture(target, [&frame, &frame_trace, trace_enabled](SkCanvas *recording) {
			DrawMarkerLayer(recording, frame, trace_enabled ? &frame_trace : nullptr);
		});
		if (!updated_markers)
			return false;
	}
	if (!impl->PrepareFrame(context, target))
		return false;

	auto const compose_trace_started = BeginFrameTrace(trace_enabled);
	auto *canvas = impl->surface->getCanvas();
	canvas->clear(SK_ColorTRANSPARENT);
	canvas->drawImage(impl->retained_base_image, 0.f, 0.f);
	canvas->drawPicture(impl->retained_timeline);
	if (updated_markers)
		impl->retained_markers = std::move(updated_markers);
	canvas->drawPicture(impl->retained_markers);
	DrawCursorLayer(canvas, target, frame, false);
	canvas->drawPicture(impl->retained_timing_labels);
	DrawCursorLayer(canvas, target, frame, true);
	canvas->drawPicture(impl->retained_post_cursor);
	if (trace_enabled) {
		frame_trace.frame_compose_ms = EndFrameTrace(compose_trace_started);
		frame_trace.valid = true;
	}
	auto const finished = impl->FinishFrame(context);
	if (finished && trace_enabled)
		impl->metrics.last_frame_trace = frame_trace;
	return finished;
}
catch (std::exception const& err) {
	impl->Fail(context, SkiaGlDeviceFailure::ContentUploadFailed, err.what());
	return false;
}
catch (...) {
	impl->Fail(context, SkiaGlDeviceFailure::ContentUploadFailed, "an unknown exception escaped Audio retained overlay rendering");
	return false;
}

void Presenter::SetContentCacheBudget(std::size_t budget_bytes) {
	impl->content_cache_budget = std::max<std::size_t>(1, budget_bytes);
	impl->TrimContentCache();
}

void Presenter::SetFailureInjection(FailureInjection failure_injection) noexcept {
	impl->failure_injection = failure_injection;
	impl->device.SetFailureInjection(DeviceFailureInjection(failure_injection));
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
	impl->ResetRetainedLayers();
	impl->device.ReleaseResourcesAndAbandon(context);
}

void Presenter::Abandon() noexcept {
	impl->device.Abandon();
	impl->surface.reset();
	impl->surface_key.reset();
	impl->ResetContentCache();
	impl->ResetRetainedLayers();
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
