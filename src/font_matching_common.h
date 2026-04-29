// Copyright (c) 2026, MIRIMIRIM
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// Font matching formulas derived from libass (ISC License):
//   https://github.com/libass/libass/blob/bbb3c7f
//   libass/ass_fontselect.c  — font_attributes_similarity
//   libass/ass_font.c        — synthetic italic/bold detection
//   libass/ass_parse.c       — ASS bold/italic normalization

#pragma once

#include <string>

struct FontMatchRequest {
	std::string facename;
	int requested_weight = 400;
	int requested_italic_value = 0;
	bool requested_italic = false;
};

struct FontMatchFaceAttributes {
	int weight = 400;
	bool bold = false;
	bool italic = false;
};

struct FontSyntheticStyle {
	bool fake_bold = false;
	bool fake_italic = false;
};

FontMatchRequest NormalizeAssFontRequest(std::string facename, int bold, bool italic);
int FontAttributesSimilarity(FontMatchFaceAttributes const& face, FontMatchRequest const& request);
FontSyntheticStyle DetectSyntheticStyle(FontMatchFaceAttributes const& face, FontMatchRequest const& request);

