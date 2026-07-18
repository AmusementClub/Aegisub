// Copyright (c) 2026, MIRIMIRIM

#pragma once

#include <string>
#include <vector>

enum class FontCollectorMatcher {
	Platform = 0,
	Libass = 1,
};

struct FontProviderOptions {
	std::vector<std::string> additional_font_files;
	bool include_system_fonts = true;
	bool collect_match_candidates = false;
};
