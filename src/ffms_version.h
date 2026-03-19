// Copyright (c) 2026
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

#pragma once

#include "source_frame.h"

#include <ffms.h>

namespace ffms {

constexpr int MakeVersion(int major, int minor, int micro, int bump) noexcept {
	return (major << 24) | (minor << 16) | (micro << 8) | bump;
}

namespace api_version {
constexpr int kTrackIndexSettings = MakeVersion(2, 21, 0, 0);
constexpr int kFrameColorMetadata = MakeVersion(2, 21, 0, 0);
constexpr int kVideoRotation = MakeVersion(2, 24, 0, 0);
constexpr int kFrameMasteringDisplay = MakeVersion(2, 27, 0, 0);
constexpr int kVideoLastEndTime = MakeVersion(2, 30, 0, 0);
constexpr int kVideoFlip = MakeVersion(2, 31, 0, 0);
constexpr int kFrameDolbyVision = MakeVersion(3, 0, 1, 0);
constexpr int kFrameHdr10Plus = MakeVersion(3, 1, 1, 0);
constexpr int kFrameStereoViews = MakeVersion(5, 1, 0, 0);
constexpr int kVideoLastEndPts = MakeVersion(5, 1, 1, 0);
}

constexpr bool VersionAtLeast(int actual, int required) noexcept {
	return actual >= required;
}

constexpr bool VersionsSupportFeature(int header_version, int runtime_version, int required) noexcept {
	return VersionAtLeast(header_version, required) && VersionAtLeast(runtime_version, required);
}

constexpr bool HeaderVersionAtLeast(int required) noexcept {
	return VersionAtLeast(FFMS_VERSION, required);
}

#ifdef WITH_FFMS2_RUNTIME_LOADING
int GetLoadedVersionNumber() noexcept;
#else
constexpr int GetLoadedVersionNumber() noexcept {
	return FFMS_VERSION;
}
#endif

inline bool RuntimeVersionAtLeast(int required) noexcept {
	return VersionAtLeast(GetLoadedVersionNumber(), required);
}

inline bool CurrentBuildSupportsFeature(int required) noexcept {
	return VersionsSupportFeature(FFMS_VERSION, GetLoadedVersionNumber(), required);
}

inline int GetVideoRotation(FFMS_VideoProperties const* properties) noexcept {
#if FFMS_VERSION >= ((2 << 24) | (24 << 16) | (0 << 8) | 0)
	if (properties && CurrentBuildSupportsFeature(api_version::kVideoRotation))
		return properties->Rotation;
#else
	(void)properties;
#endif
	return 0;
}

inline int GetVideoFlip(FFMS_VideoProperties const* properties) noexcept {
#if FFMS_VERSION >= ((2 << 24) | (31 << 16) | (0 << 8) | 0)
	if (properties && CurrentBuildSupportsFeature(api_version::kVideoFlip))
		return properties->Flip;
#else
	(void)properties;
#endif
	return 0;
}

inline SourceFrameRect GetVideoVisibleRect(FFMS_VideoProperties const* properties, int width, int height) noexcept {
	SourceFrameRect full = { 0, 0, width, height };
	if (!properties || width <= 0 || height <= 0)
		return full;

	int left = std::clamp(properties->CropLeft, 0, width);
	int top = std::clamp(properties->CropTop, 0, height);
	int right = std::clamp(properties->CropRight, 0, width - left);
	int bottom = std::clamp(properties->CropBottom, 0, height - top);
	int visible_width = width - left - right;
	int visible_height = height - top - bottom;
	if (visible_width <= 0 || visible_height <= 0)
		return full;

	return { left, top, visible_width, visible_height };
}

inline int GetFrameColorPrimaries(FFMS_Frame const* frame, int fallback = -1) noexcept {
#if FFMS_VERSION >= ((2 << 24) | (21 << 16) | (0 << 8) | 0)
	if (frame && CurrentBuildSupportsFeature(api_version::kFrameColorMetadata))
		return frame->ColorPrimaries;
#else
	(void)frame;
#endif
	return fallback;
}

inline int GetFrameTransferCharacteristics(FFMS_Frame const* frame, int fallback = -1) noexcept {
#if FFMS_VERSION >= ((2 << 24) | (21 << 16) | (0 << 8) | 0)
	if (frame && CurrentBuildSupportsFeature(api_version::kFrameColorMetadata))
		return frame->TransferCharateristics;
#else
	(void)frame;
#endif
	return fallback;
}

inline int GetFrameChromaLocation(FFMS_Frame const* frame, int fallback = 0) noexcept {
#if FFMS_VERSION >= ((2 << 24) | (21 << 16) | (0 << 8) | 0)
	if (frame && CurrentBuildSupportsFeature(api_version::kFrameColorMetadata))
		return frame->ChromaLocation;
#else
	(void)frame;
#endif
	return fallback;
}

}
