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

#include "font_matching_common.h"

#include <cstdlib>
#include <utility>

int NormalizeLibassAssWeight(int bold) noexcept {
	return (bold == 1 || bold == -1) ? 700 : bold <= 0 ? 400 : bold;
}

FontMatchRequest NormalizeLibassFontRequest(std::string facename, int bold, bool italic) {
	if (!facename.empty() && facename[0] == '@')
		facename.erase(0, 1);

	FontMatchRequest request;
	request.facename = std::move(facename);
	request.requested_weight = NormalizeLibassAssWeight(bold);
	request.requested_italic_value = italic ? 100 : 0;
	request.requested_italic = italic;
	return request;
}

int FontAttributesSimilarity(FontMatchFaceAttributes const& face, FontMatchRequest const& request) {
	int score = 0;

	if (request.requested_italic && !face.italic)
		score += 1;
	else if (!request.requested_italic && face.italic)
		score += 4;

	int effective_weight = face.weight;
	if (request.requested_weight > face.weight + 150 && !face.bold)
		effective_weight += 120;

	score += 73 * std::abs(effective_weight - request.requested_weight) / 256;
	return score;
}

FontSyntheticStyle DetectSyntheticStyle(FontMatchFaceAttributes const& face, FontMatchRequest const& request) {
	FontSyntheticStyle style;
	style.fake_italic = request.requested_italic_value > 55 && !face.italic;
	style.fake_bold = request.requested_weight > face.weight + 150 && !face.bold;
	return style;
}
