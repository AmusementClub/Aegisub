#include "skia_grid_renderer.h"

#include "grid_column_painter.h"
#include "skia_grid_text_backend.h"
#ifdef _WIN32
#include "skia_runtime/dwrite_runtime.h"
#endif

#include <include/core/SkCanvas.h>
#include <include/core/SkColor.h>
#include <include/core/SkImageInfo.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPixmap.h>
#include <include/core/SkRegion.h>
#include <include/core/SkSurface.h>

#include <libaegisub/color.h>

#include <wx/bitmap.h>
#include <wx/dc.h>
#include <wx/dcmemory.h>
#include <wx/font.h>
#ifndef _WIN32
#include <wx/rawbmp.h>
#endif
#include <wx/region.h>

#ifdef _WIN32
#include <dwrite.h>
#include <windows.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace aegisub::grid {
namespace {

constexpr int MaxRasterDimension = 32768;
constexpr std::size_t MaxRasterStorageBytes = 512u * 1024u * 1024u;
#ifdef _WIN32
constexpr std::size_t RasterBufferCount = 1u;
#else
constexpr std::size_t RasterBufferCount = 2u;
#endif

#ifdef _WIN32
template<class T>
struct ComRelease {
	void operator()(T *value) const noexcept {
		if (value) value->Release();
	}
};

template<class T>
using ComPtr = std::unique_ptr<T, ComRelease<T>>;

struct GdiRegionRelease {
	void operator()(std::remove_pointer_t<HRGN> *value) const noexcept {
		if (value) DeleteObject(value);
	}
};

using GdiRegionPtr = std::unique_ptr<std::remove_pointer_t<HRGN>, GdiRegionRelease>;

class NativeDcClip final {
	HDC dc_ = nullptr;
	int saved_state_ = 0;
	GdiRegionPtr region_;
	bool valid_ = false;

public:
	NativeDcClip(
		IDWriteBitmapRenderTarget *target,
		SkRegion const& clip,
		std::string& error) {
		dc_ = target ? target->GetMemoryDC() : nullptr;
		if (!dc_) {
			error = "DirectWrite grid raster DC is unavailable";
			return;
		}

		region_.reset(CreateRectRgn(0, 0, 0, 0));
		if (!region_) {
			error = "failed to allocate DirectWrite grid clip region";
			return;
		}
		for (SkRegion::Iterator it(clip); !it.done(); it.next()) {
			auto const& rect = it.rect();
			GdiRegionPtr part(CreateRectRgn(
				rect.left(), rect.top(), rect.right(), rect.bottom()));
			if (!part
				|| CombineRgn(region_.get(), region_.get(), part.get(), RGN_OR) == ERROR) {
				error = "failed to build DirectWrite grid clip region";
				return;
			}
		}

		saved_state_ = SaveDC(dc_);
		if (!saved_state_ || SelectClipRgn(dc_, region_.get()) == ERROR) {
			error = "failed to apply DirectWrite grid clip region";
			return;
		}
		valid_ = true;
	}

	~NativeDcClip() {
		if (saved_state_)
			RestoreDC(dc_, saved_state_);
	}

	bool valid() const noexcept { return valid_; }
};
#endif

double ElapsedMilliseconds(std::chrono::steady_clock::time_point started) noexcept {
	return std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now() - started).count();
}

SkColor ToSkColor(agi::Color const& color) noexcept {
	return SkColorSetARGB(255 - color.a, color.r, color.g, color.b);
}

SkIRect ToSkRect(wxRect const& rect, int width, int height) noexcept {
	auto const left = std::clamp(rect.x, 0, width);
	auto const top = std::clamp(rect.y, 0, height);
	auto const right = std::clamp(rect.x + std::max(rect.width, 0), 0, width);
	auto const bottom = std::clamp(rect.y + std::max(rect.height, 0), 0, height);
	return SkIRect::MakeLTRB(left, top, right, bottom);
}

class SkiaGridColumnPainter final : public GridColumnPainter {
	SkSurface *surface_ = nullptr;
	SkCanvas *canvas_ = nullptr;
	SkRegion const *clip_ = nullptr;
	SkiaGridTextBackend& text_;
	agi::Color text_color_;
	agi::Color row_background_;
	std::string error_;

	void ThrowIfFailed(bool ok) {
		if (!ok)
			throw std::runtime_error(error_.empty() ? "Skia grid text backend failed" : error_);
	}

public:
	SkiaGridColumnPainter(
		SkSurface *surface,
		SkRegion const *clip,
		SkiaGridTextBackend& text)
	: surface_(surface), canvas_(surface ? surface->getCanvas() : nullptr), clip_(clip), text_(text) {
	}

	void SetFont(wxFont const& font) override {
		error_.clear();
		ThrowIfFailed(text_.SetFont(font, error_));
	}

	void Clear(agi::Color const& color) override {
		if (canvas_)
			canvas_->drawColor(ToSkColor(color), SkBlendMode::kSrc);
	}

	void MeasureText(std::string const& utf8, int& width, int& height) override {
		if (utf8.empty()) return;
		wxString const converted = wxString::FromUTF8(utf8);
		MeasureText(std::wstring(converted.wx_str()), width, height);
	}

	void MeasureText(std::wstring const& text, int& width, int& height) override {
		if (text.empty()) return;
		error_.clear();
		ThrowIfFailed(text_.Measure(text, width, height, error_));
	}

	void DrawText(std::string const& utf8, int x, int y) override {
		if (utf8.empty() || !surface_ || !clip_) return;
		wxString const converted = wxString::FromUTF8(utf8);
		DrawText(std::wstring(converted.wx_str()), x, y);
	}

	void DrawText(std::wstring const& text, int x, int y) override {
		if (text.empty() || !surface_ || !clip_) return;
		error_.clear();
		ThrowIfFailed(text_.Draw(*surface_, *clip_, text, x, y, text_color_, error_));
	}

	void FillRectangle(int x, int y, int width, int height, agi::Color const& color) override {
		if (!canvas_ || width <= 0 || height <= 0) return;
		SkPaint paint;
		paint.setAntiAlias(false);
		paint.setColor(ToSkColor(color));
		canvas_->drawRect(SkRect::MakeXYWH(
			static_cast<float>(x), static_cast<float>(y),
			static_cast<float>(width), static_cast<float>(height)), paint);
	}

	void DrawLine(int x0, int y0, int x1, int y1, agi::Color const& color) override {
		if (!canvas_) return;
		if (y0 == y1) {
			FillRectangle(std::min(x0, x1), y0, std::abs(x1 - x0) + 1, 1, color);
			return;
		}
		if (x0 == x1) {
			FillRectangle(x0, std::min(y0, y1), 1, std::abs(y1 - y0) + 1, color);
			return;
		}
		SkPaint paint;
		paint.setAntiAlias(false);
		paint.setStyle(SkPaint::kStroke_Style);
		paint.setStrokeWidth(1.f);
		paint.setColor(ToSkColor(color));
		canvas_->drawLine(static_cast<float>(x0), static_cast<float>(y0),
			static_cast<float>(x1), static_cast<float>(y1), paint);
	}

	void StrokeRectangle(int x, int y, int width, int height, agi::Color const& color) override {
		if (width <= 0 || height <= 0) return;
		FillRectangle(x, y, width, 1, color);
		FillRectangle(x, y + height - 1, width, 1, color);
		FillRectangle(x, y, 1, height, color);
		FillRectangle(x + width - 1, y, 1, height, color);
	}

	void SetTextColor(agi::Color const& color) override { text_color_ = color; }
	agi::Color CurrentTextColor() const override { return text_color_; }
	void SetRowBackground(agi::Color const& color) override { row_background_ = color; }
	agi::Color CurrentRowBackground() const override { return row_background_; }
};

bool ValidRasterSize(int width, int height) noexcept {
	if (width <= 0 || height <= 0 || width > MaxRasterDimension || height > MaxRasterDimension)
		return false;
	auto const pixels = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
	return pixels <= MaxRasterStorageBytes / (4u * RasterBufferCount);
}

} // namespace

struct SkiaGridRenderer::Impl {
#ifdef _WIN32
	std::shared_ptr<aegisub::font::DWriteRuntime const> dwrite_runtime;
	ComPtr<IDWriteBitmapRenderTarget> bitmap_target;
#else
	wxBitmap bitmap;
#endif
	sk_sp<SkSurface> surface;
	std::unique_ptr<SkiaGridTextBackend> text;
	SkiaGridRendererStats stats;
	int width = 0;
	int height = 0;

	~Impl() {
		// The SkSurface borrows the native bitmap pixels on Windows.
		surface.reset();
#ifdef _WIN32
		bitmap_target.reset();
#endif
	}

	bool EnsureTarget(wxSize const& size, std::string& error) {
		auto const new_width = size.GetWidth();
		auto const new_height = size.GetHeight();
		if (!ValidRasterSize(new_width, new_height)) {
			error = "invalid or excessive subtitle grid raster dimensions";
			return false;
		}
#ifdef _WIN32
		if (surface && bitmap_target && width == new_width && height == new_height)
#else
		if (surface && bitmap.IsOk() && width == new_width && height == new_height)
#endif
			return true;

#ifdef _WIN32
		if (!dwrite_runtime)
			dwrite_runtime = aegisub::font::DWriteRuntime::Acquire(
				aegisub::font::DWriteRuntimeMode::SystemOnly);
		if (!dwrite_runtime || !dwrite_runtime->available()) {
			error = "system DirectWrite bitmap raster target is unavailable";
			return false;
		}

		IDWriteBitmapRenderTarget *raw_target = nullptr;
		if (FAILED(dwrite_runtime->gdi_interop()->CreateBitmapRenderTarget(
			nullptr, new_width, new_height, &raw_target)) || !raw_target) {
			error = "failed to allocate DirectWrite subtitle grid bitmap target";
			return false;
		}
		ComPtr<IDWriteBitmapRenderTarget> candidate_target(raw_target);
		if (FAILED(candidate_target->SetPixelsPerDip(1.f))) {
			error = "failed to configure DirectWrite subtitle grid bitmap target";
			return false;
		}

		auto const memory_dc = candidate_target->GetMemoryDC();
		auto const bitmap_handle = memory_dc
			? static_cast<HBITMAP>(GetCurrentObject(memory_dc, OBJ_BITMAP)) : nullptr;
		DIBSECTION section {};
		auto const object_bytes = bitmap_handle
			? GetObjectW(bitmap_handle, sizeof(section), &section) : 0;
		if (!bitmap_handle
			|| object_bytes != sizeof(section)
			|| !section.dsBm.bmBits
			|| section.dsBm.bmWidth != new_width
			|| section.dsBm.bmHeight != new_height
			|| section.dsBm.bmBitsPixel != 32
			|| std::abs(section.dsBmih.biHeight) != new_height
			|| section.dsBm.bmWidthBytes < new_width * 4) {
			error = "DirectWrite subtitle grid target is not a supported 32bpp DIB"
				" (object=" + std::to_string(object_bytes)
				+ " width=" + std::to_string(section.dsBm.bmWidth)
				+ " height=" + std::to_string(section.dsBm.bmHeight)
				+ " bpp=" + std::to_string(section.dsBm.bmBitsPixel)
				+ " header_height=" + std::to_string(section.dsBmih.biHeight)
				+ " stride=" + std::to_string(section.dsBm.bmWidthBytes) + ")";
			return false;
		}

		auto candidate = SkSurfaces::WrapPixels(
			SkImageInfo::Make(
				new_width, new_height, kBGRA_8888_SkColorType, kOpaque_SkAlphaType),
			section.dsBm.bmBits,
			static_cast<std::size_t>(section.dsBm.bmWidthBytes));
#else
		auto candidate = SkSurfaces::Raster(
			SkImageInfo::MakeN32Premul(new_width, new_height));
#endif
		if (!candidate) {
			error = "failed to allocate subtitle grid raster surface";
			return false;
		}
#ifndef _WIN32
		wxBitmap candidate_bitmap(new_width, new_height, 32);
		if (!candidate_bitmap.IsOk()) {
			error = "failed to allocate subtitle grid presentation bitmap";
			return false;
		}
#endif

		// Release the old borrowed-pixel surface before releasing its owner.
		surface.reset();
#ifdef _WIN32
		bitmap_target = std::move(candidate_target);
#else
		bitmap = std::move(candidate_bitmap);
#endif
		surface = std::move(candidate);
		width = new_width;
		height = new_height;
		++stats.surface_allocations;
		return true;
	}

	bool EnsureText(std::string& error) {
		if (text) return true;
		text = CreateSkiaGridTextBackend(error);
		return text != nullptr;
	}

	bool CopyAndPresent(
		wxDC& target,
		wxRegion const& update_region,
		SkiaGridFrameTrace& trace,
		std::string& error) {
#ifndef _WIN32
		SkPixmap source;
		if (!surface || !surface->peekPixels(&source)) {
			error = "subtitle grid raster pixels are unavailable";
			return false;
		}
		if (source.colorType() != kBGRA_8888_SkColorType
			&& source.colorType() != kRGBA_8888_SkColorType) {
			error = "unexpected subtitle grid raster pixel format";
			return false;
		}
#endif

		std::vector<wxRect> rects;
		for (wxRegionIterator it(update_region); it; ++it) {
			auto rect = it.GetRect().Intersect(wxRect(0, 0, width, height));
			if (rect.width > 0 && rect.height > 0)
				rects.push_back(rect);
		}
		if (rects.empty())
			return true;
		trace.dirty_rectangles = rects.size();
		for (auto const& rect : rects)
			trace.dirty_pixels += static_cast<unsigned long long>(rect.width)
				* static_cast<unsigned long long>(rect.height);

		auto const copy_started = std::chrono::steady_clock::now();
#ifdef _WIN32
		trace.copy_ms = ElapsedMilliseconds(copy_started);
		auto const source_dc = bitmap_target ? bitmap_target->GetMemoryDC() : nullptr;
		auto const destination_dc = static_cast<HDC>(target.GetHandle());
		if (!source_dc || !destination_dc) {
			error = "subtitle grid native presentation DC is unavailable";
			return false;
		}
#else
		{
			wxAlphaPixelData destination(bitmap);
			if (!destination) {
				error = "subtitle grid presentation bitmap cannot be mapped";
				return false;
			}

			for (auto const& rect : rects) {
				for (int y = rect.y; y < rect.y + rect.height; ++y) {
					wxAlphaPixelData::Iterator out(destination);
					out.MoveTo(destination, rect.x, y);
					auto const *in = static_cast<std::uint8_t const *>(source.addr(rect.x, y));
					for (int x = 0; x < rect.width; ++x, ++out, in += 4) {
						if (source.colorType() == kBGRA_8888_SkColorType) {
							out.Red() = in[2];
							out.Green() = in[1];
							out.Blue() = in[0];
						}
						else {
							out.Red() = in[0];
							out.Green() = in[1];
							out.Blue() = in[2];
						}
						out.Alpha() = 255;
					}
				}
				stats.copied_bytes += static_cast<unsigned long long>(rect.width)
					* static_cast<unsigned long long>(rect.height) * 4ull;
				trace.copied_bytes += static_cast<unsigned long long>(rect.width)
					* static_cast<unsigned long long>(rect.height) * 4ull;
			}
		}
		bitmap.ResetAlpha();
		trace.copy_ms = ElapsedMilliseconds(copy_started);
#endif

		auto const present_started = std::chrono::steady_clock::now();
#ifdef _WIN32
		for (auto const& rect : rects) {
			if (!BitBlt(destination_dc, rect.x, rect.y, rect.width, rect.height,
				source_dc, rect.x, rect.y, SRCCOPY | NOMIRRORBITMAP)) {
				error = "subtitle grid native bitmap blit failed";
				return false;
			}
		}
#else
		wxMemoryDC memory(bitmap);
		if (!memory.IsOk()) {
			error = "failed to select subtitle grid presentation bitmap";
			return false;
		}
		for (auto const& rect : rects) {
			if (!target.Blit(rect.x, rect.y, rect.width, rect.height,
				&memory, rect.x, rect.y, wxCOPY, false)) {
				error = "subtitle grid bitmap blit failed";
				return false;
			}
		}
		memory.SelectObject(wxNullBitmap);
#endif
		trace.present_ms = ElapsedMilliseconds(present_started);
		return true;
	}
};

SkiaGridRenderer::SkiaGridRenderer()
: impl_(std::make_unique<Impl>()) {
}

SkiaGridRenderer::~SkiaGridRenderer() = default;

bool SkiaGridRenderer::Paint(
	wxDC& target,
	wxSize const& size,
	wxRegion const& update_region,
	SkiaGridPresentationContext const& presentation,
	wxFont const& font,
	std::function<void(GridColumnPainter&)> const& paint,
	double plan_ms,
	std::string& error) {
	auto const frame_started = std::chrono::steady_clock::now();
	SkiaGridFrameTrace trace;
	trace.plan_ms = std::max(0.0, plan_ms);
	++impl_->stats.frame_attempts;
	if (!impl_->EnsureTarget(size, error) || !impl_->EnsureText(error)) {
		++impl_->stats.fallback_count;
		trace.total_ms = trace.plan_ms + ElapsedMilliseconds(frame_started);
		impl_->stats.last_frame = trace;
		return false;
	}
	SkiaGridTextRenderTarget text_target {
#ifdef _WIN32
		impl_->bitmap_target.get(),
#else
		nullptr,
#endif
		presentation.monitor_token,
		presentation.opaque_target,
		presentation.one_to_one_present,
		presentation.clear_type_requested,
	};
	if (!impl_->text->SetRenderTarget(text_target, error)) {
		++impl_->stats.fallback_count;
		trace.total_ms = trace.plan_ms + ElapsedMilliseconds(frame_started);
		impl_->stats.last_frame = trace;
		return false;
	}
	auto const text_before = impl_->text->Stats();

	SkRegion clip;
	for (wxRegionIterator it(update_region); it; ++it) {
		auto const rect = ToSkRect(it.GetRect(), impl_->width, impl_->height);
		if (!rect.isEmpty()) {
			clip.op(rect, SkRegion::kUnion_Op);
		}
	}
	if (clip.isEmpty()) {
		trace.valid = true;
		trace.text_cache = text_before;
		trace.total_ms = trace.plan_ms + ElapsedMilliseconds(frame_started);
		impl_->stats.last_frame = trace;
		return true;
	}

	auto *canvas = impl_->surface->getCanvas();
	canvas->save();
	canvas->clipRegion(clip);
	auto const paint_started = std::chrono::steady_clock::now();
	try {
#ifdef _WIN32
		// All native glyph runs in a frame share the same dirty region. Apply it
		// once here instead of allocating and selecting a GDI region per text run.
		NativeDcClip native_clip(impl_->bitmap_target.get(), clip, error);
		if (!native_clip.valid())
			throw std::runtime_error(error.empty()
				? "DirectWrite grid clip setup failed" : error);
#endif
		SkiaGridColumnPainter painter(impl_->surface.get(), &clip, *impl_->text);
		painter.SetFont(font);
		paint(painter);
	}
	catch (std::exception const& e) {
		canvas->restore();
		error = e.what();
		++impl_->stats.fallback_count;
		trace.paint_ms = ElapsedMilliseconds(paint_started);
		trace.total_ms = trace.plan_ms + ElapsedMilliseconds(frame_started);
		impl_->stats.last_frame = trace;
		return false;
	}
	catch (...) {
		canvas->restore();
		error = "unknown subtitle grid renderer failure";
		++impl_->stats.fallback_count;
		trace.paint_ms = ElapsedMilliseconds(paint_started);
		trace.total_ms = trace.plan_ms + ElapsedMilliseconds(frame_started);
		impl_->stats.last_frame = trace;
		return false;
	}
	canvas->restore();
	trace.paint_ms = ElapsedMilliseconds(paint_started);
	auto const text_after = impl_->text->Stats();
	trace.text_layout_ms = std::max(0.0, text_after.layout_ms - text_before.layout_ms);
	trace.text_raster_ms = std::max(0.0, text_after.raster_ms - text_before.raster_ms);
	trace.geometry_raster_ms = std::max(
		0.0, trace.paint_ms - trace.text_layout_ms - trace.text_raster_ms);
	trace.text_cache = text_after;

	if (!impl_->CopyAndPresent(target, update_region, trace, error)) {
		++impl_->stats.fallback_count;
		trace.total_ms = trace.plan_ms + ElapsedMilliseconds(frame_started);
		impl_->stats.last_frame = trace;
		return false;
	}
	++impl_->stats.frames_presented;
	trace.valid = true;
	trace.total_ms = trace.plan_ms + ElapsedMilliseconds(frame_started);
	impl_->stats.last_frame = trace;
	return true;
}

std::unique_ptr<GridColumnPainter> SkiaGridRenderer::CreateMeasurementPainter(
	wxFont const& font,
	std::string& error) {
	if (!impl_->EnsureText(error))
		return {};
	auto painter = std::make_unique<SkiaGridColumnPainter>(nullptr, nullptr, *impl_->text);
	try {
		painter->SetFont(font);
	}
	catch (std::exception const& e) {
		error = e.what();
		return {};
	}
	return painter;
}

void SkiaGridRenderer::InvalidateSystemFonts() noexcept {
	if (impl_->text)
		impl_->text->InvalidateSystemFonts();
}

void SkiaGridRenderer::InvalidateRasterPolicy() noexcept {
	if (impl_->text)
		impl_->text->InvalidateRasterPolicy();
}

SkiaGridRendererStats SkiaGridRenderer::Stats() const noexcept {
	return impl_->stats;
}

} // namespace aegisub::grid
