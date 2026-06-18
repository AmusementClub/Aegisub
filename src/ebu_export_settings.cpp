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

namespace {
int GetOptionInt(std::string const& name, int default_value) {
	return config::GetIntOptionOrDefault(name, default_value);
}

bool GetOptionBool(std::string const& name, bool default_value) {
	return config::GetBoolOptionOrDefault(name, default_value);
}

void SetOptionIntIfPresent(std::string const& name, int value) {
	if (!config::opt)
		return;
	try {
		OPT_SET(name)->SetInt(value);
	}
	catch (...) {
	}
}

void SetOptionBoolIfPresent(std::string const& name, bool value) {
	if (!config::opt)
		return;
	try {
		OPT_SET(name)->SetBool(value);
	}
	catch (...) {
	}
}
}

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
, tv_standard((TvStandard)GetOptionInt(prefix + "/TV Standard", 0))
, text_encoding((TextEncoding)GetOptionInt(prefix + "/Text Encoding", 0))
, max_line_length(GetOptionInt(prefix + "/Max Line Length", 42))
, line_wrapping_mode((LineWrappingMode)GetOptionInt(prefix + "/Line Wrapping Mode", 1))
, translate_alignments(GetOptionBool(prefix + "/Translate Alignments", true))
, inclusive_end_times(GetOptionBool(prefix + "/Inclusive End Times", true))
, display_standard((DisplayStandard)GetOptionInt(prefix + "/Display Standard", 0))
{
	timecode_offset.h = GetOptionInt(prefix + "/Timecode Offset/H", 0);
	timecode_offset.m = GetOptionInt(prefix + "/Timecode Offset/M", 0);
	timecode_offset.s = GetOptionInt(prefix + "/Timecode Offset/S", 0);
	timecode_offset.f = GetOptionInt(prefix + "/Timecode Offset/F", 0);
}

void EbuExportSettings::Save() const {
	if (!config::opt)
		return;

	SetOptionIntIfPresent(prefix + "/TV Standard", tv_standard);
	SetOptionIntIfPresent(prefix + "/Text Encoding", text_encoding);
	SetOptionIntIfPresent(prefix + "/Max Line Length", max_line_length);
	SetOptionIntIfPresent(prefix + "/Line Wrapping Mode", line_wrapping_mode);
	SetOptionBoolIfPresent(prefix + "/Translate Alignments", translate_alignments);
	SetOptionBoolIfPresent(prefix + "/Inclusive End Times", inclusive_end_times);
	SetOptionIntIfPresent(prefix + "/Display Standard", display_standard);
	SetOptionIntIfPresent(prefix + "/Timecode Offset/H", timecode_offset.h);
	SetOptionIntIfPresent(prefix + "/Timecode Offset/M", timecode_offset.m);
	SetOptionIntIfPresent(prefix + "/Timecode Offset/S", timecode_offset.s);
	SetOptionIntIfPresent(prefix + "/Timecode Offset/F", timecode_offset.f);
}
