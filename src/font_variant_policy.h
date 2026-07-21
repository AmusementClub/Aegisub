#pragma once

#include "font_family_catalog.h"

#include <optional>
#include <vector>

/// The logical ASS font state exposed by the font selectors. `weight` is the
/// effective ASS \b request (400/700 for the ordinary states, or an exact
/// numeric weight for an existing inline override). The explicit flags are
/// provenance only: when set, the corresponding source override is preserved
/// unless the caller explicitly permits replacement.
struct FontVariantSelection {
	int weight = 400;
	bool italic = false;
	bool has_explicit_bold = false;
	bool has_explicit_italic = false;
};

/// A family change is an explicit user action. Opening a dialog or changing
/// an unrelated property must leave the current ASS state untouched.
struct FontVariantSelectionAction {
	bool family_changed = false;
	bool allow_replace_explicit = false;
};

struct FontVariantAdjustment {
	FontVariantSelection selection;
	bool changed_weight = false;
	bool changed_italic = false;
	bool applied_implicit_selection = false;
	bool blocked_by_explicit = false;

	bool HasChanges() const noexcept {
		return changed_weight || changed_italic;
	}
};

/// Convert one reliable backend result into an ASS-representable RBIZ choice.
/// Numeric realized weight alone is deliberately insufficient: a 600/900 face
/// can still have a Regular physical role. Unknown, synthetic, and
/// non-canonical results are not safe automatic choices.
std::optional<FontVariantChoice> CanonicalizeFontVariant(
	FontVariantOutcome const& outcome);

/// Return the canonical, de-duplicated choices in stable RBIZ order.
std::vector<FontVariantChoice> BuildVariantChoices(
	FontFamilyVariantProfile const& profile);

/// Return the sole non-Regular face selected by an ordinary request when the
/// corresponding ASS B/I request preserves the same physical entity.
std::optional<FontVariantChoice> FindImplicitVariantSelection(
	FontFamilyVariantProfile const& profile);

/// Apply a family profile's safe implicit choice to an ASS selection. Explicit
/// \b/\i overrides, including numeric weights, are protected by default.
/// `allow_replace_explicit` is reserved for an explicit user action such as a
/// dedicated "replace variant" command.
FontVariantAdjustment AdjustFamilySelection(
	FontVariantSelection const& current,
	FontFamilyVariantProfile const& profile,
	FontVariantSelectionAction action = {});
