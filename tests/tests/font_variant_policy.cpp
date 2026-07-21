#include "../../src/font_variant_policy.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <utility>

namespace {

FontVariantOutcome outcome(
	FontVariantRole role,
	std::uint64_t entity,
	FontVariantStatus status = FontVariantStatus::Canonical,
	int realized_weight = 400,
	bool realized_italic = false) {
	FontVariantOutcome result;
	result.realized_weight = realized_weight;
	result.realized_italic = realized_italic;
	result.role = role;
	result.status = status;
	result.entity_token = entity;
	return result;
}

FontFamilyVariantProfile profile_with(
	FontVariantOutcome regular,
	FontVariantOutcome bold = {},
	FontVariantOutcome italic = {},
	FontVariantOutcome bold_italic = {}) {
	FontFamilyVariantProfile profile;
	profile.outcomes = {
		std::move(regular),
		std::move(bold),
		std::move(italic),
		std::move(bold_italic),
	};
	profile.backend = FontVariantBackend::VsFilterGdi;
	profile.evidence = FontSelectionEvidence::Observed;
	profile.automatic_pinning_reliable = true;
	return profile;
}

FontVariantSelectionAction family_change(bool allow_replace_explicit = false) {
	return {true, allow_replace_explicit};
}

} // namespace

TEST(font_variant_policy, canonicalizes_from_physical_role_not_numeric_weight) {
	auto black_regular = CanonicalizeFontVariant(
		outcome(FontVariantRole::Regular, 1, FontVariantStatus::Canonical, 900));
	ASSERT_TRUE(black_regular);
	EXPECT_EQ(FontVariantRole::Regular, black_regular->role);
	EXPECT_EQ(400, black_regular->weight);
	EXPECT_FALSE(black_regular->italic);

	auto semibold_marked_bold = CanonicalizeFontVariant(
		outcome(FontVariantRole::Bold, 2, FontVariantStatus::Canonical, 600));
	ASSERT_TRUE(semibold_marked_bold);
	EXPECT_EQ(FontVariantRole::Bold, semibold_marked_bold->role);
	EXPECT_EQ(700, semibold_marked_bold->weight);
	EXPECT_FALSE(semibold_marked_bold->italic);
}

TEST(font_variant_policy, rejects_unknown_noncanonical_and_synthetic_outcomes) {
	EXPECT_FALSE(CanonicalizeFontVariant(
		outcome(FontVariantRole::Unknown, 1, FontVariantStatus::Canonical)));
	EXPECT_FALSE(CanonicalizeFontVariant(
		outcome(FontVariantRole::Bold, 0, FontVariantStatus::Canonical, 700)));
	EXPECT_FALSE(CanonicalizeFontVariant(
		outcome(FontVariantRole::Bold, 1, FontVariantStatus::NonCanonical, 700)));
	EXPECT_FALSE(CanonicalizeFontVariant(
		outcome(FontVariantRole::Bold, 1, FontVariantStatus::Synthetic, 700)));
}

TEST(font_variant_policy, builds_deduplicated_choices_in_rbiz_order) {
	auto profile = profile_with(
		outcome(FontVariantRole::Regular, 10),
		outcome(FontVariantRole::Bold, 20, FontVariantStatus::Canonical, 700),
		outcome(FontVariantRole::Italic, 30, FontVariantStatus::Canonical, 400, true),
		outcome(FontVariantRole::BoldItalic, 40, FontVariantStatus::Canonical, 700, true));

	auto choices = BuildVariantChoices(profile);
	ASSERT_EQ(4u, choices.size());
	EXPECT_EQ(FontVariantRole::Regular, choices[0].role);
	EXPECT_EQ(FontVariantRole::Bold, choices[1].role);
	EXPECT_EQ(FontVariantRole::Italic, choices[2].role);
	EXPECT_EQ(FontVariantRole::BoldItalic, choices[3].role);
	EXPECT_EQ(400, choices[0].weight);
	EXPECT_EQ(700, choices[1].weight);
	EXPECT_TRUE(choices[2].italic);
	EXPECT_TRUE(choices[3].italic);
}

TEST(font_variant_policy, one_physical_face_is_one_choice) {
	auto profile = profile_with(
		outcome(FontVariantRole::Bold, 55, FontVariantStatus::Canonical, 700),
		outcome(FontVariantRole::Bold, 55, FontVariantStatus::Canonical, 700),
		outcome(FontVariantRole::Unknown, 0),
		outcome(FontVariantRole::Bold, 55, FontVariantStatus::Canonical, 700));

	auto choices = BuildVariantChoices(profile);
	ASSERT_EQ(1u, choices.size());
	EXPECT_EQ(FontVariantRole::Bold, choices.front().role);
	EXPECT_EQ(55u, choices.front().entity_token);
}

TEST(font_variant_policy, bold_only_family_pins_an_inherited_regular_request) {
	auto profile = profile_with(
		outcome(FontVariantRole::Bold, 7, FontVariantStatus::Canonical, 700),
		outcome(FontVariantRole::Bold, 7, FontVariantStatus::Canonical, 700));
	FontVariantSelection current;

	auto adjusted = AdjustFamilySelection(current, profile, family_change());
	EXPECT_TRUE(adjusted.applied_implicit_selection);
	EXPECT_TRUE(adjusted.HasChanges());
	EXPECT_TRUE(adjusted.changed_weight);
	EXPECT_FALSE(adjusted.changed_italic);
	EXPECT_EQ(700, adjusted.selection.weight);
	EXPECT_FALSE(adjusted.selection.italic);
}

TEST(font_variant_policy, unknown_backend_is_report_only_by_default) {
	auto profile = BuildFontFamilyVariantProfile({
		outcome(FontVariantRole::Bold, 7, FontVariantStatus::Canonical, 700),
		outcome(FontVariantRole::Bold, 7, FontVariantStatus::Canonical, 700),
		FontVariantOutcome{},
		FontVariantOutcome{},
	});

	EXPECT_FALSE(profile.automatic_pinning_reliable);
	EXPECT_FALSE(FindImplicitVariantSelection(profile));
	auto adjusted = AdjustFamilySelection(
		FontVariantSelection{}, profile, family_change());
	EXPECT_FALSE(adjusted.applied_implicit_selection);
}

TEST(font_variant_policy, algorithmic_evidence_never_gets_an_implicit_pin) {
	auto profile = BuildFontFamilyVariantProfile({
		outcome(FontVariantRole::Bold, 7, FontVariantStatus::Canonical, 700),
		outcome(FontVariantRole::Bold, 7, FontVariantStatus::Canonical, 700),
		FontVariantOutcome{},
		FontVariantOutcome{},
	}, FontVariantBackend::LibassScoring,
	   FontSelectionEvidence::Algorithmic, true);

	EXPECT_FALSE(FindImplicitVariantSelection(profile));
}

TEST(font_variant_policy, observed_evidence_is_not_tied_to_backend_name) {
	auto profile = BuildFontFamilyVariantProfile({
		outcome(FontVariantRole::Bold, 7, FontVariantStatus::Canonical, 700),
		outcome(FontVariantRole::Bold, 7, FontVariantStatus::Canonical, 700),
		FontVariantOutcome{},
		FontVariantOutcome{},
	}, FontVariantBackend::CoreText, FontSelectionEvidence::Observed, true);

	EXPECT_TRUE(FindImplicitVariantSelection(profile).has_value());
}

TEST(font_variant_policy, implicit_pin_must_preserve_the_physical_entity) {
	auto profile = BuildFontFamilyVariantProfile({
		outcome(FontVariantRole::Bold, 7, FontVariantStatus::Canonical, 700),
		outcome(FontVariantRole::Bold, 8, FontVariantStatus::Canonical, 700),
		FontVariantOutcome{},
		FontVariantOutcome{},
	}, FontVariantBackend::VsFilterGdi,
	   FontSelectionEvidence::Observed, true);
	EXPECT_FALSE(FindImplicitVariantSelection(profile).has_value());

	auto adjusted = AdjustFamilySelection(
		FontVariantSelection{}, profile, family_change());
	EXPECT_FALSE(adjusted.applied_implicit_selection);
	EXPECT_FALSE(adjusted.HasChanges());
}

TEST(font_variant_policy, regular_and_bold_family_has_no_implicit_variant) {
	auto profile = profile_with(
		outcome(FontVariantRole::Regular, 1),
		outcome(FontVariantRole::Bold, 2, FontVariantStatus::Canonical, 700));
	FontVariantSelection current{400, false, false, false};

	auto adjusted = AdjustFamilySelection(current, profile, family_change());
	EXPECT_FALSE(adjusted.applied_implicit_selection);
	EXPECT_FALSE(adjusted.HasChanges());
	EXPECT_EQ(400, adjusted.selection.weight);
}

TEST(font_variant_policy, merely_opening_the_dialog_does_not_pin_a_variant) {
	auto profile = profile_with(
		outcome(FontVariantRole::Bold, 7, FontVariantStatus::Canonical, 700),
		outcome(FontVariantRole::Bold, 7, FontVariantStatus::Canonical, 700));
	FontVariantSelection current;

	auto adjusted = AdjustFamilySelection(current, profile, {});
	EXPECT_FALSE(adjusted.applied_implicit_selection);
	EXPECT_FALSE(adjusted.HasChanges());
	EXPECT_EQ(400, adjusted.selection.weight);
}

TEST(font_variant_policy, explicit_numeric_weight_is_preserved_by_default) {
	auto profile = profile_with(
		outcome(FontVariantRole::Bold, 7, FontVariantStatus::Canonical, 700),
		outcome(FontVariantRole::Bold, 7, FontVariantStatus::Canonical, 700));
	FontVariantSelection current{600, false, true, false};

	auto adjusted = AdjustFamilySelection(current, profile, family_change());
	EXPECT_TRUE(adjusted.blocked_by_explicit);
	EXPECT_FALSE(adjusted.applied_implicit_selection);
	EXPECT_FALSE(adjusted.HasChanges());
	EXPECT_EQ(600, adjusted.selection.weight);
}

TEST(font_variant_policy, explicit_boolean_override_is_also_preserved_by_default) {
	auto profile = profile_with(
		outcome(FontVariantRole::Bold, 7, FontVariantStatus::Canonical, 700),
		outcome(FontVariantRole::Bold, 7, FontVariantStatus::Canonical, 700));
	FontVariantSelection current{400, false, true, false};

	auto adjusted = AdjustFamilySelection(current, profile, family_change());
	EXPECT_TRUE(adjusted.blocked_by_explicit);
	EXPECT_FALSE(adjusted.HasChanges());
	EXPECT_EQ(400, adjusted.selection.weight);
}

TEST(font_variant_policy, explicit_user_action_can_replace_a_protected_override) {
	auto profile = profile_with(
		outcome(FontVariantRole::Bold, 7, FontVariantStatus::Canonical, 700),
		outcome(FontVariantRole::Bold, 7, FontVariantStatus::Canonical, 700));
	FontVariantSelection current{600, false, true, false};

	auto adjusted = AdjustFamilySelection(current, profile, family_change(true));
	EXPECT_FALSE(adjusted.blocked_by_explicit);
	EXPECT_TRUE(adjusted.applied_implicit_selection);
	EXPECT_TRUE(adjusted.changed_weight);
	EXPECT_EQ(700, adjusted.selection.weight);
}

TEST(font_variant_policy, matching_explicit_dimension_is_preserved_while_free_dimension_pins) {
	auto profile = profile_with(
		outcome(FontVariantRole::BoldItalic, 9, FontVariantStatus::Canonical, 700, true),
		outcome(FontVariantRole::BoldItalic, 9, FontVariantStatus::Canonical, 700, true),
		outcome(FontVariantRole::BoldItalic, 9, FontVariantStatus::Canonical, 700, true),
		outcome(FontVariantRole::BoldItalic, 9, FontVariantStatus::Canonical, 700, true));
	FontVariantSelection current{700, false, true, false};

	auto adjusted = AdjustFamilySelection(current, profile, family_change());
	EXPECT_TRUE(adjusted.applied_implicit_selection);
	EXPECT_FALSE(adjusted.changed_weight);
	EXPECT_TRUE(adjusted.changed_italic);
	EXPECT_EQ(700, adjusted.selection.weight);
	EXPECT_TRUE(adjusted.selection.italic);
}

TEST(font_variant_policy, conflicting_explicit_dimension_prevents_partial_pin) {
	auto profile = profile_with(
		outcome(FontVariantRole::BoldItalic, 9, FontVariantStatus::Canonical, 700, true),
		outcome(FontVariantRole::BoldItalic, 9, FontVariantStatus::Canonical, 700, true),
		outcome(FontVariantRole::BoldItalic, 9, FontVariantStatus::Canonical, 700, true),
		outcome(FontVariantRole::BoldItalic, 9, FontVariantStatus::Canonical, 700, true));
	FontVariantSelection current{400, false, true, false};

	auto adjusted = AdjustFamilySelection(current, profile, family_change());
	EXPECT_TRUE(adjusted.blocked_by_explicit);
	EXPECT_FALSE(adjusted.HasChanges());
	EXPECT_EQ(400, adjusted.selection.weight);
	EXPECT_FALSE(adjusted.selection.italic);
}

TEST(font_variant_policy, untrusted_implicit_choice_is_ignored) {
	auto profile = profile_with(
		outcome(FontVariantRole::Bold, 99, FontVariantStatus::NonCanonical, 700),
		outcome(FontVariantRole::Bold, 99, FontVariantStatus::NonCanonical, 700));
	FontVariantSelection current;

	EXPECT_FALSE(FindImplicitVariantSelection(profile).has_value());
	auto adjusted = AdjustFamilySelection(current, profile, family_change());
	EXPECT_FALSE(adjusted.applied_implicit_selection);
	EXPECT_FALSE(adjusted.HasChanges());
}
