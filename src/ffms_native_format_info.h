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

struct FFMSNativeFormatIds {
	int nv12 = -1;
	int p010le = -1;
	int yuv420p = -1;
	int yuv420p10le = -1;
	int yuv422p = -1;
	int yuv422p10le = -1;
	int yuv444p = -1;
	int yuv444p10le = -1;
};

inline bool TryGetFFMSNativeSourceFrameFormatInfo(
	int pixfmt,
	FFMSNativeFormatIds const& ids,
	SourceFrameFormatInfo& info) {
	if (pixfmt < 0)
		return false;

	if (pixfmt == ids.nv12) {
		info = MakeSemiplanar420SourceFrameFormatInfo(8, 1, 2);
		return true;
	}
	if (pixfmt == ids.p010le) {
		info = MakeSemiplanar420SourceFrameFormatInfo(10, 2, 4);
		return true;
	}
	if (pixfmt == ids.yuv420p) {
		info = MakePlanarYCbCrSourceFrameFormatInfo(2, 2, 8, 1);
		return true;
	}
	if (pixfmt == ids.yuv420p10le) {
		info = MakePlanarYCbCrSourceFrameFormatInfo(2, 2, 10, 2);
		return true;
	}
	if (pixfmt == ids.yuv422p) {
		info = MakePlanarYCbCrSourceFrameFormatInfo(2, 1, 8, 1);
		return true;
	}
	if (pixfmt == ids.yuv422p10le) {
		info = MakePlanarYCbCrSourceFrameFormatInfo(2, 1, 10, 2);
		return true;
	}
	if (pixfmt == ids.yuv444p) {
		info = MakePlanarYCbCrSourceFrameFormatInfo(1, 1, 8, 1);
		return true;
	}
	if (pixfmt == ids.yuv444p10le) {
		info = MakePlanarYCbCrSourceFrameFormatInfo(1, 1, 10, 2);
		return true;
	}

	return false;
}
