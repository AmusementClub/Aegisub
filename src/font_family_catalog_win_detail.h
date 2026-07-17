#pragma once

#include <string>

namespace font_family_catalog_win_detail {

/// Stable identity of a face resolved through GDI and DirectWrite.
struct FontEntityKey {
	std::string path_lower;
	int face_index = -1;
	bool valid = false;
};

inline bool SameEntity(FontEntityKey const& a, FontEntityKey const& b) {
	return a.valid && b.valid
	    && a.face_index == b.face_index
	    && a.path_lower == b.path_lower;
}

} // namespace font_family_catalog_win_detail
