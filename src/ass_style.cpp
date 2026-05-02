// Copyright (c) 2005, Rodrigo Braz Monteiro
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the name of the Aegisub Group nor the names of its contributors
//     may be used to endorse or promote products derived from this software
//     without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// Aegisub Project http://www.aegisub.org/

/// @file ass_style.cpp
/// @brief Class for style definitions in subtitles
/// @ingroup subs_storage
///

#include "ass_style.h"

#include "ass_compat.h"
#include "ass_parse_error.h"

#include <libaegisub/split.h>
#include <libaegisub/string_utils.h>
#include <libaegisub/util.h>

#include <algorithm>

AssStyle::AssStyle() {
	std::fill(Margin.begin(), Margin.end(), 10);

	UpdateData();
}

AssEntryGroup AssStyle::Group() const { return AssEntryGroup::STYLE; }

namespace {
double non_negative(double value) {
	return std::max(value, 0.0);
}

void append_style_field(std::string& line, std::string const& value, bool& first) {
	if (!first)
		line.push_back(',');
	line.append(value);
	first = false;
}

class parser {
	agi::split_iterator<agi::StringRange::const_iterator> pos;

	std::string next_tok() {
		if (pos.eof())
			throw SubtitleFormatParseError("Malformed style: not enough fields");
		return agi::util::strings::trim_copy(agi::str(*pos++));
	}

public:
	parser(std::string const& str) {
		auto colon = find(str.begin(), str.end(), ':');
		if (colon != str.end())
			pos = agi::Split(agi::StringRange(colon + 1, str.end()), ',');
	}

	void check_done() const {
		if (!pos.eof())
			throw SubtitleFormatParseError("Malformed style: too many fields");
	}

	std::string next_str() { return next_tok(); }
	agi::Color next_color() {
		agi::Color color;
		if (!AssCompat::ParseStyleColor(next_tok(), color))
			throw SubtitleFormatParseError("Malformed style: bad color field");
		return color;
	}

	int next_int() {
		int value = 0;
		if (!AssCompat::ParseInteger(next_tok(), value))
			throw SubtitleFormatParseError("Malformed style: bad int field");
		return value;
	}

	double next_double() {
		double value = 0.0;
		if (!AssCompat::ParseFloat(next_tok(), value))
			throw SubtitleFormatParseError("Malformed style: bad double field");
		return value;
	}

	void skip_token() {
		if (!pos.eof())
			++pos;
	}
};
}

AssStyle::AssStyle(std::string const& str, int version) {
	parser p(str);

	name = p.next_str();
	font = p.next_str();
	fontsize = p.next_double();

	if (version != 0) {
		primary = p.next_color();
		secondary = p.next_color();
		outline = p.next_color();
		shadow = p.next_color();
	}
	else {
		primary = p.next_color();
		secondary = p.next_color();

		// Skip tertiary color
		p.skip_token();

		// Read shadow/outline color
		outline = p.next_color();
		shadow = outline;
	}

	bold = !!p.next_int();
	italic = !!p.next_int();

	if (version != 0) {
		underline = !!p.next_int();
		strikeout = !!p.next_int();

		scalex = p.next_double();
		scaley = p.next_double();
		spacing = p.next_double();
		angle = p.next_double();
	}
	else {
		// SSA defaults
		underline = false;
		strikeout = false;

		scalex = 100;
		scaley = 100;
		spacing = 0;
		angle = 0.0;
	}

	borderstyle = p.next_int();
	outline_w = p.next_double();
	shadow_w = p.next_double();
	alignment = p.next_int();

	if (version == 0)
		alignment = SsaToAss(alignment);

	Margin[0] = agi::util::mid(-9999, p.next_int(), 99999);
	Margin[1] = agi::util::mid(-9999, p.next_int(), 99999);
	Margin[2] = agi::util::mid(-9999, p.next_int(), 99999);

	// Skip alpha level
	if (version == 0)
		p.skip_token();

	encoding = p.next_int();

	p.check_done();

	UpdateData();
}

void AssStyle::UpdateData() {
	replace(name.begin(), name.end(), ',', ';');
	replace(font.begin(), font.end(), ',', ';');

	data = "Style: ";
	bool first = true;
	auto append = [&](std::string const& value) { append_style_field(data, value, first); };

	append(name);
	append(font);
	append(AssCompat::FormatFloat(fontsize));
	append(AssCompat::FormatStyleColor(primary));
	append(AssCompat::FormatStyleColor(secondary));
	append(AssCompat::FormatStyleColor(outline));
	append(AssCompat::FormatStyleColor(shadow));
	append(AssCompat::FormatInteger(bold ? -1 : 0));
	append(AssCompat::FormatInteger(italic ? -1 : 0));
	append(AssCompat::FormatInteger(underline ? -1 : 0));
	append(AssCompat::FormatInteger(strikeout ? -1 : 0));
	append(AssCompat::FormatFloat(non_negative(scalex)));
	append(AssCompat::FormatFloat(non_negative(scaley)));
	append(AssCompat::FormatFloat(non_negative(spacing)));
	append(AssCompat::FormatFloat(angle));
	append(AssCompat::FormatInteger(borderstyle));
	append(AssCompat::FormatFloat(non_negative(outline_w)));
	append(AssCompat::FormatFloat(non_negative(shadow_w)));
	append(AssCompat::FormatInteger(alignment));
	append(AssCompat::FormatInteger(Margin[0]));
	append(AssCompat::FormatInteger(Margin[1]));
	append(AssCompat::FormatInteger(Margin[2]));
	append(AssCompat::FormatInteger(encoding));
}

int AssStyle::AssToSsa(int ass_align) {
	switch (ass_align) {
		case 1:  return 1;
		case 2:  return 2;
		case 3:  return 3;
		case 4:  return 9;
		case 5:  return 10;
		case 6:  return 11;
		case 7:  return 5;
		case 8:  return 6;
		case 9:  return 7;
		default: return 2;
	}
}

int AssStyle::SsaToAss(int ssa_align) {
	switch(ssa_align) {
		case 1:  return 1;
		case 2:  return 2;
		case 3:  return 3;
		case 5:  return 7;
		case 6:  return 8;
		case 7:  return 9;
		case 9:  return 4;
		case 10: return 5;
		case 11: return 6;
		default: return 2;
	}
}
