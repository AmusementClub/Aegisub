#pragma once

#include <optional>
#include <string>

struct FontFamilyCatalogUiModel;
class wxWindow;
namespace agi { struct Context; }

struct FontFaceDialogSelection {
	std::string face_name;
	int point_size = 10;
	bool bold = false;
	bool italic = false;
	bool underline = false;
};

/// Show the native wx selector for localized names, or Aegisub's
/// catalog-backed selector for English names. The displayed face name is also
/// the exact face name returned for ASS writing.
std::optional<FontFaceDialogSelection> ShowFontFaceDialog(
	wxWindow *parent,
	agi::Context *context,
	FontFaceDialogSelection const& initial,
	FontFamilyCatalogUiModel const& font_model);
