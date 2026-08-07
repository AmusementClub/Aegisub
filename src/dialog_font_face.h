#pragma once

#include "ass_style.h"

#include <libaegisub/color.h>

#include <functional>
#include <cstdint>
#include <optional>
#include <string>

struct FontFamilySelectionModel;
class wxWindow;
namespace agi { struct Context; }

struct FontFaceDialogSelection {
	std::string face_name;
	/// Catalog identity for an explicitly selected list item. This disambiguates
	/// duplicate display aliases without serializing an implementation token.
	std::optional<std::uint32_t> selected_family_id;
	int point_size = 10;
	int charset = 1;
	/// Effective ASS/GDI weight. Values other than 400/700 are retained until
	/// the user explicitly changes the variant controls.
	int effective_weight = 400;
	bool bold = false;
	bool italic = false;
	bool underline = false;
	bool has_explicit_weight = false;
	bool has_explicit_italic = false;
	bool variant_modified = false;
	bool implicit_variant_pinned = false;
	bool allow_replace_explicit = false;
	bool from_native_dialog = false;

	/// Preview appearance carried from the active line's style + overrides, so
	/// the preview reflects the line's border/shadow/colors instead of AssStyle
	/// defaults. These are display-only and never written back.
	double outline_w = AssStyle::DefaultOutlineWidth;
	double shadow_w = AssStyle::DefaultShadowWidth;
	/// \borderstyle has no override tag, so only the style value is carried.
	int borderstyle = AssStyle::DefaultBorderStyle;
	agi::Color primary{ 255, 255, 255 };
	agi::Color outline{ 0, 0, 0 };
	agi::Color shadow{ 0, 0, 0 };
};

/// Show Aegisub's custom selector using localized or English catalog names as
/// configured. If the catalog is empty, the selector uses enumerated fallback
/// names. The displayed face name is also the exact face name returned for ASS
/// writing.
///
/// The callback is called when the dialog's Apply button is pressed. Apply
/// leaves the dialog open so callers can inspect the result and continue
/// editing the selection.
std::optional<FontFaceDialogSelection> ShowFontFaceDialog(
	wxWindow *parent,
	agi::Context *context,
	FontFaceDialogSelection const& initial,
	FontFamilySelectionModel const& font_model,
	std::function<void(FontFaceDialogSelection const&)> on_apply = {});
