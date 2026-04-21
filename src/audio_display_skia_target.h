// Copyright (c) 2026
// All rights reserved.

#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <wx/bitmap.h>
#include <wx/gdicmn.h>

class wxDC;
class SkCanvas;

class AudioDisplaySkiaTarget {
public:
	virtual ~AudioDisplaySkiaTarget() = default;

	AudioDisplaySkiaTarget(AudioDisplaySkiaTarget const&) = delete;
	AudioDisplaySkiaTarget& operator=(AudioDisplaySkiaTarget const&) = delete;

protected:
	AudioDisplaySkiaTarget() = default;

public:
	virtual bool IsValid() const = 0;
	virtual wxRect const& GetRect() const = 0;
	virtual SkCanvas *GetCanvas() const = 0;
	virtual bool Finalize() = 0;
};

class AudioDisplaySkiaPresentTarget : public AudioDisplaySkiaTarget {
public:
	virtual bool PresentTo(wxDC &dc, bool use_mask) = 0;
};

std::unique_ptr<AudioDisplaySkiaTarget> CreateAudioDisplaySkiaBitmapTarget(wxBitmap &bitmap, wxRect const& rect);
std::unique_ptr<AudioDisplaySkiaPresentTarget> CreateAudioDisplaySkiaBitmapPresentTarget(wxRect const& rect);

bool CopyBgraPixelsToBitmap(
	std::vector<uint32_t> const& pixels,
	int width,
	int height,
	wxBitmap &bitmap);
