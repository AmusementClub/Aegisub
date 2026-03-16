#pragma once

#include <libaegisub/string_utils.h>

inline bool TryParseMicroDVDFrames(agi::util::strings::view first, agi::util::strings::view second, int& start_frame, int& end_frame) {
	return agi::util::strings::parse_integer(first, start_frame)
		&& agi::util::strings::parse_integer(second, end_frame);
}
