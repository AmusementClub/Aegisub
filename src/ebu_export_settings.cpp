// Copyright (c) 2011 Niels Martin Hansen <nielsm@aegisub.org>
// Copyright (c) 2012 Thomas Goyne <plorkyeran@aegisub.org>
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
//
// Aegisub Project http://www.aegisub.org/

/// @file ebu_export_settings.cpp
/// @see ebu_export_settings.h
/// @ingroup subtitle_io export

#include "ebu_export_settings.h"

#include "options.h"

#include <libaegisub/charset_conv.h>
#include <libaegisub/make_unique.h>

agi::vfr::Framerate EbuExportSettings::GetFramerate() const {
	switch (tv_standard) {
		case STL24:     return agi::vfr::Framerate(24, 1);
		case STL25:     return agi::vfr::Framerate(25, 1);
		case STL30:     return agi::vfr::Framerate(30, 1);
		case STL23:     return agi::vfr::Framerate(24000, 1001, false);
		case STL29:     return agi::vfr::Framerate(30000, 1001, false);
		case STL29drop: return agi::vfr::Framerate(30000, 1001);
		default:        return agi::vfr::Framerate(25, 1);
	}
}

std::unique_ptr<agi::charset::IconvWrapper> EbuExportSettings::GetTextEncoder() const {
	using namespace agi;
	switch (text_encoding) {
		case iso6937_2: return make_unique<charset::IconvWrapper>("utf-8", "ISO-6937-2");
		case iso8859_5: return make_unique<charset::IconvWrapper>("utf-8", "ISO-8859-5");
		case iso8859_6: return make_unique<charset::IconvWrapper>("utf-8", "ISO-8859-6");
		case iso8859_7: return make_unique<charset::IconvWrapper>("utf-8", "ISO-8859-7");
		case iso8859_8: return make_unique<charset::IconvWrapper>("utf-8", "ISO-8859-8");
		case utf8:      return make_unique<charset::IconvWrapper>("utf-8", "utf-8");
		default:        return make_unique<charset::IconvWrapper>("utf-8", "ISO-8859-1");
	}
}

EbuExportSettings::EbuExportSettings(std::string const& prefix)
: prefix(prefix)
, tv_standard((TvStandard)OPT_GET(prefix + "/TV Standard")->GetInt())
, text_encoding((TextEncoding)OPT_GET(prefix + "/Text Encoding")->GetInt())
, max_line_length(OPT_GET(prefix + "/Max Line Length")->GetInt())
, line_wrapping_mode((LineWrappingMode)OPT_GET(prefix + "/Line Wrapping Mode")->GetInt())
, translate_alignments(OPT_GET(prefix + "/Translate Alignments")->GetBool())
, inclusive_end_times(OPT_GET(prefix + "/Inclusive End Times")->GetBool())
, display_standard((DisplayStandard)OPT_GET(prefix + "/Display Standard")->GetInt())
{
	timecode_offset.h = OPT_GET(prefix + "/Timecode Offset/H")->GetInt();
	timecode_offset.m = OPT_GET(prefix + "/Timecode Offset/M")->GetInt();
	timecode_offset.s = OPT_GET(prefix + "/Timecode Offset/S")->GetInt();
	timecode_offset.f = OPT_GET(prefix + "/Timecode Offset/F")->GetInt();
}

void EbuExportSettings::Save() const {
	OPT_SET(prefix + "/TV Standard")->SetInt(tv_standard);
	OPT_SET(prefix + "/Text Encoding")->SetInt(text_encoding);
	OPT_SET(prefix + "/Max Line Length")->SetInt(max_line_length);
	OPT_SET(prefix + "/Line Wrapping Mode")->SetInt(line_wrapping_mode);
	OPT_SET(prefix + "/Translate Alignments")->SetBool(translate_alignments);
	OPT_SET(prefix + "/Inclusive End Times")->SetBool(inclusive_end_times);
	OPT_SET(prefix + "/Display Standard")->SetInt(display_standard);
	OPT_SET(prefix + "/Timecode Offset/H")->SetInt(timecode_offset.h);
	OPT_SET(prefix + "/Timecode Offset/M")->SetInt(timecode_offset.m);
	OPT_SET(prefix + "/Timecode Offset/S")->SetInt(timecode_offset.s);
	OPT_SET(prefix + "/Timecode Offset/F")->SetInt(timecode_offset.f);
}
