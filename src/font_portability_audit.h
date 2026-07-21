#pragma once

#include "font_name_normalization.h"
#include "font_variant_audit.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

class AssFile;

struct FontPortabilityAuditOptions {
	FontNameNormalizationTarget name_target = FontNameNormalizationTarget::Localized;
	FontVariantAuditOptions variant;

	bool operator==(FontPortabilityAuditOptions const&) const = default;
};

struct FontPortabilityAuditPlan {
	FontPortabilityAuditOptions options;
	FontNameNormalizationPlan names;
	FontVariantAuditPlan variants;
};

enum class FontPortabilityAuditFilter {
	All,
	Names,
	Variants
};

class FontPortabilityAuditSelection {
	std::vector<std::uint8_t> names;
	std::vector<std::uint8_t> variants;

public:
	void Reset(
		FontPortabilityAuditPlan const& plan,
		FontPortabilityAuditFilter initial_filter);
	void ResetNames(FontNameNormalizationPlan const& plan, bool select_safe);
	void ResetVariants(FontVariantAuditPlan const& plan, bool select_safe);
	void SelectSafe(
		FontPortabilityAuditPlan const& plan,
		FontPortabilityAuditFilter filter,
		bool select);

	bool IsNameSelected(std::size_t index) const;
	bool IsVariantSelected(std::size_t index) const;
	bool SetNameSelected(
		FontNameNormalizationPlan const& plan,
		std::size_t index,
		bool selected);
	bool SetVariantSelected(
		FontVariantAuditPlan const& plan,
		std::size_t index,
		bool selected);

	std::size_t SelectedNameCount() const;
	std::size_t SelectedVariantCount() const;
	bool HasSelection() const;
	std::vector<std::size_t> SelectedNameIndices() const;
	std::vector<std::size_t> SelectedVariantIndices() const;
};

struct FontPortabilityAuditApplyResult {
	bool success = false;
	bool styles_changed = false;
	bool dialogue_text_changed = false;
	std::size_t applied_name_change_count = 0;
	std::size_t applied_variant_change_count = 0;
	std::string error;
};

/// Build the two independent audit plans from one immutable subtitle state.
FontPortabilityAuditPlan BuildFontPortabilityAuditPlan(
	AssFile const& file,
	FontFamilyCatalog const& catalog,
	FontPortabilityAuditOptions options,
	std::span<int const> style_source_lines = {},
	FontVariantAuditProfileProvider const& profile_provider = {});

/// Validate and prepare both plans on a private AssFile copy. Variant fixes are
/// prepared before family-name changes so their original ASS snapshots remain
/// valid. The original file is updated only after both operations succeed.
FontPortabilityAuditApplyResult ApplyFontPortabilityAuditChanges(
	AssFile& file,
	FontPortabilityAuditPlan const& plan,
	std::span<std::size_t const> selected_name_change_indices,
	std::span<std::size_t const> selected_variant_finding_indices,
	FontPortabilityAuditOptions options,
	FontVariantAuditProfileProvider const& profile_provider = {});
