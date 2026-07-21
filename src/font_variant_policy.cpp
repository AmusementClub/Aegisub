#include "font_variant_policy.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace {

bool is_known_role(FontVariantRole role) {
	return role == FontVariantRole::Regular ||
	       role == FontVariantRole::Bold ||
	       role == FontVariantRole::Italic ||
	       role == FontVariantRole::BoldItalic;
}

int role_order(FontVariantRole role) {
	switch (role) {
		case FontVariantRole::Regular: return 0;
		case FontVariantRole::Bold: return 1;
		case FontVariantRole::Italic: return 2;
		case FontVariantRole::BoldItalic: return 3;
		case FontVariantRole::Unknown: break;
	}
	return 4;
}

FontVariantChoice choice_for_role(FontVariantRole role, std::uint64_t entity_token) {
	FontVariantChoice choice;
	choice.role = role;
	choice.weight = role == FontVariantRole::Bold || role == FontVariantRole::BoldItalic
		? 700
		: 400;
	choice.italic = role == FontVariantRole::Italic || role == FontVariantRole::BoldItalic;
	choice.status = FontVariantStatus::Canonical;
	choice.entity_token = entity_token;
	return choice;
}

bool same_entity_and_role(FontVariantChoice const& left, FontVariantChoice const& right) {
	return left.entity_token == right.entity_token && left.role == right.role;
}

bool target_request_preserves_choice(
	FontFamilyVariantProfile const& profile,
	FontVariantChoice const& choice) {
	auto target = CanonicalizeFontVariant(
		profile.For(choice.weight == 700, choice.italic));
	return target && same_entity_and_role(*target, choice);
}

} // namespace

std::optional<FontVariantChoice> CanonicalizeFontVariant(
	FontVariantOutcome const& outcome) {
	if (outcome.status != FontVariantStatus::Canonical ||
	    outcome.entity_token == 0 || !is_known_role(outcome.role))
		return std::nullopt;
	return choice_for_role(outcome.role, outcome.entity_token);
}

std::vector<FontVariantChoice> BuildVariantChoices(
	FontFamilyVariantProfile const& profile) {
	std::vector<FontVariantChoice> choices;
	choices.reserve(profile.outcomes.size());

	auto append = [&](std::optional<FontVariantChoice> candidate) {
		if (!candidate)
			return;
		if (std::find_if(choices.begin(), choices.end(), [&](auto const& existing) {
			return existing.entity_token == candidate->entity_token;
		}) == choices.end())
			choices.push_back(*candidate);
	};

	for (auto const& outcome : profile.outcomes)
		append(CanonicalizeFontVariant(outcome));

	std::stable_sort(choices.begin(), choices.end(), [](auto const& left, auto const& right) {
		return role_order(left.role) < role_order(right.role);
	});
	return choices;
}

std::optional<FontVariantChoice> FindImplicitVariantSelection(
	FontFamilyVariantProfile const& profile) {
	// Automatic ASS pins require both physical selection evidence and a
	// backend declaration that the profile is reliable for this operation.
	// Do not key this rule to a backend name: future observed providers should
	// get the same behavior, while algorithmic providers remain report-only.
	if (profile.evidence != FontSelectionEvidence::Observed ||
	    !profile.automatic_pinning_reliable)
		return std::nullopt;

	// A normal request which lands on a reliable non-Regular face, while no
	// upright Regular outcome exists, is the only implicit fallback that can be
	// safely pinned with ASS's B/I controls.
	auto ordinary = CanonicalizeFontVariant(profile.outcomes[0]);
	if (!ordinary || ordinary->role == FontVariantRole::Regular)
		return std::nullopt;

	for (std::size_t index : {std::size_t(0), std::size_t(1)}) {
		auto choice = CanonicalizeFontVariant(profile.outcomes[index]);
		if (choice && choice->role == FontVariantRole::Regular)
			return std::nullopt;
	}
	if (!target_request_preserves_choice(profile, *ordinary))
		return std::nullopt;

	auto const choices = BuildVariantChoices(profile);
	auto const it = std::find_if(choices.begin(), choices.end(), [&](auto const& choice) {
		return same_entity_and_role(choice, *ordinary);
	});
	return it == choices.end() ? std::nullopt : std::optional<FontVariantChoice>(*it);
}

FontVariantAdjustment AdjustFamilySelection(
	FontVariantSelection const& current,
	FontFamilyVariantProfile const& profile,
	FontVariantSelectionAction action) {
	FontVariantAdjustment result;
	result.selection = current;
	if (!action.family_changed)
		return result;

	auto implicit = FindImplicitVariantSelection(profile);
	if (!implicit)
		return result;

	bool const weight_diff = current.weight != implicit->weight;
	bool const italic_diff = current.italic != implicit->italic;
	bool const weight_blocked = weight_diff && current.has_explicit_bold &&
		!action.allow_replace_explicit;
	bool const italic_blocked = italic_diff && current.has_explicit_italic &&
		!action.allow_replace_explicit;
	if (weight_blocked || italic_blocked) {
		result.blocked_by_explicit = true;
		return result;
	}

	if (weight_diff) {
		result.selection.weight = implicit->weight;
		result.changed_weight = true;
	}
	if (italic_diff) {
		result.selection.italic = implicit->italic;
		result.changed_italic = true;
	}
	result.applied_implicit_selection = true;
	return result;
}
