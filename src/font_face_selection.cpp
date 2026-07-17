#include "font_face_selection.h"

bool ShouldWriteFontFace(
	std::string_view stored_name,
	std::string_view displayed_name,
	bool has_explicit_override,
	std::string_view selected_name)
{
	if (displayed_name != selected_name)
		return true;
	return has_explicit_override && stored_name != selected_name;
}
