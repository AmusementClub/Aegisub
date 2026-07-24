#pragma once

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
};

/// Show the native system font selector when prefer-localized is on, otherwise
/// always Aegisub's custom selector (catalog English names, or enumerator
/// fallback if the catalog is empty). The displayed face name is also the
/// exact face name returned for ASS writing.
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
