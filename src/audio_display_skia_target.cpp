// Copyright (c) 2026
// All rights reserved.

#include "audio_display_skia_target.h"

#include <include/core/SkCanvas.h>
#include <include/core/SkImageInfo.h>
#include <include/core/SkSurface.h>

#include <wx/dc.h>
#include <wx/rawbmp.h>

#include <algorithm>
#include <vector>

namespace {
class AudioDisplaySkiaBitmapTarget final : public AudioDisplaySkiaPresentTarget {
	struct Impl {
		std::vector<uint32_t> pixels;
		sk_sp<SkSurface> surface;
	};

	wxRect rect;
	wxBitmap owned_bitmap;
	wxBitmap *bitmap = nullptr;
	std::unique_ptr<Impl> impl;

	void Initialize();

public:
	explicit AudioDisplaySkiaBitmapTarget(wxRect const& rect);
	AudioDisplaySkiaBitmapTarget(wxBitmap &bitmap, wxRect const& rect);
	~AudioDisplaySkiaBitmapTarget() override;

	bool IsValid() const override;
	wxRect const& GetRect() const override { return rect; }
	SkCanvas *GetCanvas() const override;
	bool Finalize() override;
	bool PresentTo(wxDC &dc, bool use_mask) override;
};
}

bool CopyBgraPixelsToBitmap(
	std::vector<uint32_t> const& pixels,
	int width,
	int height,
	wxBitmap &bitmap) {
	if (width <= 0 || height <= 0 || !bitmap.IsOk())
		return false;
	if (static_cast<int>(bitmap.GetWidth()) != width || static_cast<int>(bitmap.GetHeight()) != height)
		return false;
	if (pixels.size() < static_cast<size_t>(width) * static_cast<size_t>(height))
		return false;

	wxAlphaPixelData pixel_data(bitmap);
	if (!pixel_data)
		return false;

	auto row = pixel_data.GetPixels();
	auto const* src_bytes = reinterpret_cast<unsigned char const*>(pixels.data());
	size_t const row_bytes = static_cast<size_t>(width) * 4;
	for (int y = 0; y < height; ++y) {
		auto pixel = row;
		auto const* src = src_bytes + static_cast<size_t>(y) * row_bytes;
		for (int x = 0; x < width; ++x) {
			pixel.Blue() = src[x * 4 + 0];
			pixel.Green() = src[x * 4 + 1];
			pixel.Red() = src[x * 4 + 2];
			pixel.Alpha() = src[x * 4 + 3];
			++pixel;
		}
		row.OffsetY(pixel_data, 1);
	}

	return true;
}

AudioDisplaySkiaBitmapTarget::AudioDisplaySkiaBitmapTarget(wxRect const& rect)
: rect(rect)
, owned_bitmap(rect.width, rect.height, 32)
, bitmap(&owned_bitmap)
, impl(std::make_unique<Impl>()) {
	Initialize();
}

AudioDisplaySkiaBitmapTarget::AudioDisplaySkiaBitmapTarget(wxBitmap &bitmap, wxRect const& rect)
: rect(rect)
, bitmap(&bitmap)
, impl(std::make_unique<Impl>()) {
	Initialize();
}

AudioDisplaySkiaBitmapTarget::~AudioDisplaySkiaBitmapTarget() = default;

void AudioDisplaySkiaBitmapTarget::Initialize() {
	if (!bitmap || !bitmap->IsOk())
		return;
	if (bitmap->GetWidth() != rect.width || bitmap->GetHeight() != rect.height)
		return;
	if (rect.width <= 0 || rect.height <= 0)
		return;

	impl->pixels.assign(static_cast<size_t>(rect.width) * static_cast<size_t>(rect.height), 0);
	auto const image_info = SkImageInfo::Make(
		rect.width,
		rect.height,
		kBGRA_8888_SkColorType,
		kPremul_SkAlphaType);
	impl->surface = SkSurfaces::WrapPixels(
		image_info,
		impl->pixels.data(),
		static_cast<size_t>(rect.width) * 4);
}

bool AudioDisplaySkiaBitmapTarget::IsValid() const {
	return bitmap
		&& bitmap->IsOk()
		&& impl
		&& impl->surface;
}

SkCanvas *AudioDisplaySkiaBitmapTarget::GetCanvas() const {
	return IsValid() ? impl->surface->getCanvas() : nullptr;
}

bool AudioDisplaySkiaBitmapTarget::Finalize() {
	if (!IsValid())
		return false;
	return CopyBgraPixelsToBitmap(impl->pixels, rect.width, rect.height, *bitmap);
}

bool AudioDisplaySkiaBitmapTarget::PresentTo(wxDC &dc, bool use_mask) {
	if (!Finalize())
		return false;
	dc.DrawBitmap(*bitmap, rect.x, rect.y, use_mask);
	return true;
}

std::unique_ptr<AudioDisplaySkiaTarget> CreateAudioDisplaySkiaBitmapTarget(wxBitmap &bitmap, wxRect const& rect) {
	return std::make_unique<AudioDisplaySkiaBitmapTarget>(bitmap, rect);
}

std::unique_ptr<AudioDisplaySkiaPresentTarget> CreateAudioDisplaySkiaBitmapPresentTarget(wxRect const& rect) {
	return std::make_unique<AudioDisplaySkiaBitmapTarget>(rect);
}
