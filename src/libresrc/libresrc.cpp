// Copyright (c) 2009, Amar Takhar <verm@aegisub.org>
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

#include "libresrc.h"

#include <wx/bitmap.h>
#include <wx/bmpbndl.h>
#include <wx/icon.h>
#include <wx/image.h>
#include <wx/intl.h>
#include <wx/mstream.h>

namespace {
wxBitmap libresrc_getimage_directional(const unsigned char *buff, size_t size, int dir) {
	wxMemoryInputStream mem(buff, size);
	if (dir != wxLayout_RightToLeft)
		return wxBitmap(wxImage(mem));
	return wxBitmap(wxImage(mem).Mirror());
}
}

wxBitmap libresrc_getimage(const unsigned char *buff, size_t size) {
	return libresrc_getimage_directional(buff, size, wxLayout_LeftToRight);
}

wxBitmap libresrc_getimage_resized(const unsigned char* buff, size_t size, int dir, int resize) {
	wxMemoryInputStream mem(buff, size);
	if (dir != wxLayout_RightToLeft)
		return wxBitmap(wxImage(mem).Scale(resize, resize, wxIMAGE_QUALITY_HIGH));
	return wxBitmap(wxImage(mem).Scale(resize, resize, wxIMAGE_QUALITY_HIGH).Mirror());
}

wxBitmapBundle libresrc_getimage_bundle(
	const unsigned char *image16, size_t size16,
	const unsigned char *image24, size_t size24,
	const unsigned char *image32, size_t size32,
	const unsigned char *image48, size_t size48,
	const unsigned char *image64, size_t size64,
	int dir)
{
	wxVector<wxBitmap> bitmaps;
	bitmaps.push_back(libresrc_getimage_directional(image16, size16, dir));
	bitmaps.push_back(libresrc_getimage_directional(image24, size24, dir));
	bitmaps.push_back(libresrc_getimage_directional(image32, size32, dir));
	bitmaps.push_back(libresrc_getimage_directional(image48, size48, dir));
	bitmaps.push_back(libresrc_getimage_directional(image64, size64, dir));
	return wxBitmapBundle::FromBitmaps(bitmaps);
}

wxIcon libresrc_geticon(const unsigned char *buff, size_t size) {
	wxMemoryInputStream mem(buff, size);
	wxIcon icon;
	icon.CopyFromBitmap(wxBitmap(wxImage(mem)));
	return icon;
}

std::pair<const char *, size_t> libresrc_getconfig(const unsigned char *data, size_t size) {
	return {reinterpret_cast<const char *>(data), size};
}
