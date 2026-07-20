#pragma once

#include <functional>
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
///
/// The callback is called when the dialog's Apply button is pressed. Apply
/// leaves the dialog open so callers can inspect the result and continue
/// editing the selection.
std::optional<FontFaceDialogSelection> ShowFontFaceDialog(
	wxWindow *parent,
	agi::Context *context,
	FontFaceDialogSelection const& initial,
	FontFamilyCatalogUiModel const& font_model,
	std::function<void(FontFaceDialogSelection const&)> on_apply = {});
