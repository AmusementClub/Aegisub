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

#include <cstdlib>
#include <utility>

#include <wx/bmpbndl.h>
#include <wx/defs.h>

#include "bitmap.h"
#include "default_config.h"

class wxBitmap;
class wxIcon;

wxBitmap libresrc_getimage(const unsigned char *image, size_t size);
wxBitmap libresrc_getimage_resized(const unsigned char* image, size_t size, int dir, int resize);
wxBitmapBundle libresrc_getimage_bundle(
    const unsigned char *image16, size_t size16,
    const unsigned char *image24, size_t size24,
    const unsigned char *image32, size_t size32,
    const unsigned char *image48, size_t size48,
    const unsigned char *image64, size_t size64,
    int dir);
wxIcon libresrc_geticon(const unsigned char *image, size_t size);
std::pair<const char *, size_t> libresrc_getconfig(const unsigned char *data, size_t size);
#define GETIMAGE(a) libresrc_getimage(a, sizeof(a))
#define GETIMAGEDIR(a, d, s) libresrc_getimage_resized(a, sizeof(a), d, s)
#define GETIMAGEBUNDLE(a, d) libresrc_getimage_bundle(a##_16, sizeof(a##_16), a##_24, sizeof(a##_24), a##_32, sizeof(a##_32), a##_48, sizeof(a##_48), a##_64, sizeof(a##_64), d)
#if defined(__WXMSW__) && wxCHECK_VERSION(3, 1, 5)
#define AEGI_BITMAP_ICON_SIZE(window, size) (size)
#elif defined(__WXMSW__)
#define AEGI_BITMAP_ICON_SIZE(window, size) ((window)->FromDIP(size))
#else
#define AEGI_BITMAP_ICON_SIZE(window, size) (size)
#endif
#define CMD_ICON_GET(icon, dir, size) ( \
    (size) <= 16 ? GETIMAGEDIR(icon##_16, (dir), (size)) : \
    (size) <= 24 ? GETIMAGEDIR(icon##_24, (dir), (size)) : \
    (size) <= 32 ? GETIMAGEDIR(icon##_32, (dir), (size)) : \
    (size) <= 48 ? GETIMAGEDIR(icon##_48, (dir), (size)) : GETIMAGEDIR(icon##_64, (dir), (size)) )
#define CMD_ICON_BUNDLE_GET(icon, dir) GETIMAGEBUNDLE(icon, dir)
#define GETICON(a) libresrc_geticon(a, sizeof(a))
