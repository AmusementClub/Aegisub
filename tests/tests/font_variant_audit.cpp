#include "../../src/font_variant_audit.h"

#include "../../src/ass_dialogue.h"
#include "../../src/ass_file.h"
#include "../../src/ass_font_state.h"
#include "../../src/ass_style.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

FontVariantOutcome outcome(
	FontVariantRole role,
	std::uint64_t entity,
	FontVariantStatus status = FontVariantStatus::Canonical,
	int weight = 400,
	bool italic = false) {
	FontVariantOutcome result;
	result.realized_weight = weight;
	result.realized_italic = italic;
	result.role = role;
	result.status = status;
	result.entity_token = entity;
	return result;
}

FontFamilyVariantProfile profile(
	FontVariantOutcome regular,
	FontVariantOutcome bold = {},
	FontVariantOutcome italic = {},
	FontVariantOutcome bold_italic = {}) {
	return BuildFontFamilyVariantProfile({
		std::move(regular),
		std::move(bold),
		std::move(italic),
		std::move(bold_italic),
	}, FontVariantBackend::VsFilterGdi,
	   FontSelectionEvidence::Observed, true);
}

FontFamilyRecord family(
	FontFamilyId id,
	std::string name,
	FontFamilyVariantProfile variant_profile) {
	FontFamilyRecord result;
	result.id = id;
	result.localized_family_name = name;
	result.english_win32_family_name = std::move(name);
	result.variant_profile = std::move(variant_profile);
	return result;
}

FontFamilyVariantProfile regular_profile(std::uint64_t base = 100) {
	return profile(
		outcome(FontVariantRole::Regular, base),
		outcome(FontVariantRole::Bold, base + 1, FontVariantStatus::Canonical, 700),
		outcome(FontVariantRole::Italic, base + 2, FontVariantStatus::Canonical, 400, true),
		outcome(FontVariantRole::BoldItalic, base + 3, FontVariantStatus::Canonical, 700, true));
}

FontFamilyVariantProfile bold_only_profile(std::uint64_t entity = 10) {
	return profile(
		outcome(FontVariantRole::Bold, entity, FontVariantStatus::Canonical, 700),
		outcome(FontVariantRole::Bold, entity, FontVariantStatus::Canonical, 700),
		outcome(FontVariantRole::Unknown, 0, FontVariantStatus::Synthetic),
		outcome(FontVariantRole::Unknown, 0, FontVariantStatus::Synthetic));
}

FontFamilyVariantProfile italic_only_profile(std::uint64_t entity = 20) {
	return profile(
		outcome(FontVariantRole::Italic, entity, FontVariantStatus::Canonical, 400, true),
		outcome(FontVariantRole::Unknown, 0, FontVariantStatus::Synthetic),
		outcome(FontVariantRole::Italic, entity, FontVariantStatus::Canonical, 400, true),
		outcome(FontVariantRole::Unknown, 0, FontVariantStatus::Synthetic));
}

AssStyle& add_style(
	AssFile& file,
	std::string name,
	std::string font,
	bool bold = false,
	bool italic = false) {
	auto* style = new AssStyle;
	style->name = std::move(name);
	style->font = std::move(font);
	style->bold = bold;
	style->italic = italic;
	style->UpdateData();
	file.Styles.push_back(*style);
	return *style;
}

AssDialogue& add_event(
	AssFile& file,
	std::string text,
	int row = 0,
	std::string style = "Default") {
	auto* event = new AssDialogue;
	event->Text = std::move(text);
	event->Style = std::move(style);
	event->Row = row;
	file.Events.push_back(*event);
	return *event;
}

} // namespace

TEST(font_variant_audit, reports_and_applies_safe_style_fallbacks) {
	FontFamilyCatalog catalog({
		family(1, "Bold Only", bold_only_profile()),
		family(2, "Italic Only", italic_only_profile()),
	});
	AssFile file;
	auto& bold = add_style(file, "BoldStyle", "Bold Only");
	auto& italic = add_style(file, "ItalicStyle", "Italic Only");

	std::array<int, 2> source_lines{7, 8};
	auto plan = BuildFontVariantAuditPlan(file, catalog, {}, source_lines);
	ASSERT_EQ(2u, plan.findings.size());
	EXPECT_EQ(2u, plan.scanned_style_count);
	EXPECT_EQ(FontVariantAuditClassification::ImplicitBoldFallback,
	          plan.findings[0].classification);
	EXPECT_EQ(FontVariantAuditClassification::ImplicitItalicFallback,
	          plan.findings[1].classification);
	EXPECT_EQ(7, plan.findings[0].source.line);
	EXPECT_TRUE(plan.findings[0].safe_to_apply);
	EXPECT_TRUE(plan.findings[1].safe_to_apply);

	std::array<std::size_t, 2> selected{0, 1};
	auto applied = ApplyFontVariantAuditChanges(file, plan, selected);
	EXPECT_TRUE(applied.success) << applied.error;
	EXPECT_TRUE(applied.styles_changed);
	EXPECT_FALSE(applied.dialogue_text_changed);
	EXPECT_EQ(2u, applied.applied_change_count);
	EXPECT_TRUE(bold.bold);
	EXPECT_FALSE(bold.italic);
	EXPECT_FALSE(italic.bold);
	EXPECT_TRUE(italic.italic);
}

TEST(font_variant_audit, ordinary_rbiz_family_does_not_produce_a_finding) {
	FontFamilyCatalog catalog({family(1, "Complete", regular_profile())});
	AssFile file;
	add_style(file, "Default", "Complete");

	auto plan = BuildFontVariantAuditPlan(file, catalog);
	EXPECT_EQ(1u, plan.scanned_style_count);
	EXPECT_TRUE(plan.findings.empty());
}

TEST(font_variant_audit, classifies_noncanonical_synthetic_and_unknown_requests) {
	FontFamilyCatalog catalog({
		family(1, "Noncanonical", profile(outcome(
			FontVariantRole::Unknown, 11, FontVariantStatus::NonCanonical))),
		family(2, "Synthetic", profile(outcome(
			FontVariantRole::Unknown, 12, FontVariantStatus::Synthetic))),
		family(3, "No Profile", profile(outcome(
			FontVariantRole::Unknown, 0, FontVariantStatus::Unknown))),
	});
	AssFile file;
	add_style(file, "A", "Noncanonical");
	add_style(file, "B", "Synthetic");
	add_style(file, "C", "No Profile");
	add_style(file, "D", "Missing Family");

	auto plan = BuildFontVariantAuditPlan(file, catalog);
	ASSERT_EQ(4u, plan.findings.size());
	EXPECT_EQ(FontVariantAuditClassification::NonCanonical,
	          plan.findings[0].classification);
	EXPECT_EQ(FontVariantAuditClassification::Synthetic,
	          plan.findings[1].classification);
	EXPECT_EQ(FontVariantAuditClassification::Unknown,
	          plan.findings[2].classification);
	EXPECT_EQ(FontVariantAuditClassification::Unknown,
	          plan.findings[3].classification);
	for (auto const& finding : plan.findings)
		EXPECT_FALSE(finding.safe_to_apply);
}

TEST(font_variant_audit, applies_an_explicit_family_fix_at_the_text_span) {
	FontFamilyCatalog catalog({
		family(1, "Base", regular_profile()),
		family(2, "Bold Only", bold_only_profile()),
	});
	AssFile file;
	add_style(file, "Default", "Base");
	auto& event = add_event(file, "{\\fnBold Only}text", 9);

	auto plan = BuildFontVariantAuditPlan(file, catalog);
	ASSERT_EQ(1u, plan.findings.size());
	auto const& finding = plan.findings.front();
	EXPECT_EQ(FontVariantAuditSourceKind::OverrideSpan, finding.source.kind);
	EXPECT_EQ(FontVariantAuditClassification::ImplicitBoldFallback,
	          finding.classification);
	EXPECT_EQ(10, finding.source.line);
	EXPECT_TRUE(finding.safe_to_apply);
	EXPECT_FALSE(finding.has_explicit_bold);

	std::array<std::size_t, 1> selected{0};
	auto applied = ApplyFontVariantAuditChanges(file, plan, selected);
	EXPECT_TRUE(applied.success) << applied.error;
	EXPECT_TRUE(applied.dialogue_text_changed);
	EXPECT_EQ("{\\fnBold Only}{\\b1}text", event.Text.get());

	auto after = BuildFontVariantAuditPlan(file, catalog);
	EXPECT_TRUE(after.findings.empty());
}

TEST(font_variant_audit, coalesces_repairs_across_unrelated_override_blocks) {
	FontFamilyCatalog catalog({
		family(1, "Base", regular_profile()),
		family(2, "Bold Only", bold_only_profile()),
	});
	AssFile file;
	add_style(file, "Default", "Base");
	auto& event = add_event(file, "{\\fnBold Only}one{\\c&HFFFFFF&}two");

	auto plan = BuildFontVariantAuditPlan(file, catalog);
	ASSERT_EQ(2u, plan.findings.size());
	EXPECT_EQ(plan.findings[0].source.font_state_revision,
	          plan.findings[1].source.font_state_revision);

	std::array<std::size_t, 2> selected{0, 1};
	auto applied = ApplyFontVariantAuditChanges(file, plan, selected);
	ASSERT_TRUE(applied.success) << applied.error;
	EXPECT_EQ("{\\fnBold Only}{\\b1}one{\\c&HFFFFFF&}two", event.Text.get());
	EXPECT_TRUE(BuildFontVariantAuditPlan(file, catalog).findings.empty());
}

TEST(font_variant_audit, explicit_variant_override_is_report_only_without_opt_in) {
	FontFamilyCatalog catalog({
		family(1, "Base", regular_profile()),
		family(2, "Bold Only", bold_only_profile()),
	});
	AssFile file;
	add_style(file, "Default", "Base");
	auto& event = add_event(file, "{\\fnBold Only\\b0}text");

	auto plan = BuildFontVariantAuditPlan(file, catalog);
	ASSERT_EQ(1u, plan.findings.size());
	EXPECT_EQ(FontVariantAuditClassification::ImplicitBoldFallback,
	          plan.findings[0].classification);
	EXPECT_TRUE(plan.findings[0].requires_explicit_replacement);
	EXPECT_FALSE(plan.findings[0].safe_to_apply);
	EXPECT_EQ("explicit_variant_override_preserved", plan.findings[0].reason_code);

	std::array<std::size_t, 1> selected{0};
	auto rejected = ApplyFontVariantAuditChanges(file, plan, selected);
	EXPECT_FALSE(rejected.success);
	EXPECT_EQ("{\\fnBold Only\\b0}text", event.Text.get());

	auto applied = ApplyFontVariantAuditChanges(file, plan, selected, {true});
	EXPECT_TRUE(applied.success) << applied.error;
	EXPECT_EQ("{\\fnBold Only\\b1}text", event.Text.get());
}

TEST(font_variant_audit, build_option_marks_explicit_fix_safe) {
	FontFamilyCatalog catalog({
		family(1, "Base", regular_profile()),
		family(2, "Italic Only", italic_only_profile()),
	});
	AssFile file;
	add_style(file, "Default", "Base");
	add_event(file, "{\\fnItalic Only\\i0}text");

	auto plan = BuildFontVariantAuditPlan(file, catalog, {true});
	ASSERT_EQ(1u, plan.findings.size());
	EXPECT_TRUE(plan.findings[0].requires_explicit_replacement);
	EXPECT_TRUE(plan.findings[0].safe_to_apply);
	EXPECT_TRUE(plan.findings[0].pin_italic);
}

TEST(font_variant_audit, replaces_explicit_variant_written_before_family) {
	FontFamilyCatalog catalog({
		family(1, "Base", regular_profile()),
		family(2, "Bold Only", bold_only_profile()),
	});
	AssFile file;
	add_style(file, "Default", "Base");
	auto& event = add_event(file, "{\\b0\\fnBold Only}text");

	auto plan = BuildFontVariantAuditPlan(file, catalog, {true});
	ASSERT_EQ(1u, plan.findings.size());
	ASSERT_TRUE(plan.findings[0].safe_to_apply);
	ASSERT_TRUE(plan.findings[0].requires_explicit_replacement);
	std::array<std::size_t, 1> selected{0};

	auto applied = ApplyFontVariantAuditChanges(file, plan, selected, {true});
	ASSERT_TRUE(applied.success) << applied.error;
	EXPECT_EQ("{\\b0\\fnBold Only}{\\b1}text", event.Text.get());
	EXPECT_TRUE(BuildFontVariantAuditPlan(file, catalog).findings.empty());
}

TEST(font_variant_audit, numeric_weight_remains_report_only_even_with_opt_in) {
	FontFamilyCatalog catalog({
		family(1, "Base", regular_profile()),
		family(2, "Bold Only", bold_only_profile()),
	});
	AssFile file;
	add_style(file, "Default", "Base");
	auto& event = add_event(file, "{\\fnBold Only\\b600}text");

	auto plan = BuildFontVariantAuditPlan(file, catalog, {true});
	ASSERT_EQ(1u, plan.findings.size());
	EXPECT_EQ(FontVariantAuditClassification::NonCanonical,
	          plan.findings[0].classification);
	EXPECT_EQ("numeric_weight_report_only", plan.findings[0].reason_code);
	EXPECT_EQ("600", plan.findings[0].raw_bold_tag);
	EXPECT_FALSE(plan.findings[0].safe_to_apply);

	std::array<std::size_t, 1> selected{0};
	auto applied = ApplyFontVariantAuditChanges(file, plan, selected, {true});
	EXPECT_FALSE(applied.success);
	EXPECT_EQ("{\\fnBold Only\\b600}text", event.Text.get());
}

TEST(font_variant_audit, transform_derived_variant_state_is_report_only) {
	FontFamilyCatalog catalog({
		family(1, "Base", regular_profile()),
		family(2, "Bold Only", bold_only_profile()),
	});
	AssFile file;
	add_style(file, "Default", "Base");
	add_event(file, "{\\t(0,100,\\fnBold Only)}text");

	auto plan = BuildFontVariantAuditPlan(file, catalog, {true});
	ASSERT_EQ(1u, plan.findings.size());
	EXPECT_TRUE(plan.findings[0].source.in_transform);
	EXPECT_EQ("transform_variant_report_only", plan.findings[0].reason_code);
	EXPECT_FALSE(plan.findings[0].safe_to_apply);
}

TEST(font_variant_audit, stale_event_rejects_every_selected_fix_before_style_mutation) {
	FontFamilyCatalog catalog({
		family(1, "Base", regular_profile()),
		family(2, "Bold Only", bold_only_profile()),
	});
	AssFile file;
	auto& style = add_style(file, "Default", "Bold Only");
	auto& event = add_event(file, "{\\fnBold Only}text");

	auto plan = BuildFontVariantAuditPlan(file, catalog);
	ASSERT_EQ(2u, plan.findings.size());
	event.Text = "{\\fnBold Only}changed";
	std::array<std::size_t, 2> selected{0, 1};

	auto applied = ApplyFontVariantAuditChanges(file, plan, selected);
	EXPECT_FALSE(applied.success);
	EXPECT_FALSE(style.bold);
	EXPECT_EQ("{\\fnBold Only}changed", event.Text.get());
}

TEST(font_variant_audit, stale_style_state_rejects_event_fix_without_mutation) {
	FontFamilyCatalog catalog({
		family(1, "Base", regular_profile()),
		family(2, "Bold Only", bold_only_profile()),
	});
	AssFile file;
	auto& style = add_style(file, "Default", "Bold Only");
	auto& event = add_event(file, "{\\fnBold Only}text");
	auto plan = BuildFontVariantAuditPlan(file, catalog);
	ASSERT_EQ(2u, plan.findings.size());
	style.font = "Base";
	std::array<std::size_t, 2> selected{0, 1};

	auto applied = ApplyFontVariantAuditChanges(file, plan, selected);
	EXPECT_FALSE(applied.success);
	EXPECT_FALSE(style.bold);
	EXPECT_EQ("{\\fnBold Only}text", event.Text.get());
}

TEST(font_variant_audit, request_profile_preserves_charset_and_source_font_size) {
	FontFamilyCatalog catalog({family(1, "Dynamic", regular_profile())});
	AssFile file;
	auto& style = add_style(file, "Default", "Dynamic");
	style.encoding = 128;
	style.fontsize = 42.5;

	std::vector<aegisub::ass::AssFontRequest> requests;
	FontVariantAuditProfileProvider provider = [&](auto const& request)
		-> std::optional<FontFamilyVariantProfile> {
		requests.push_back(request);
		return bold_only_profile(500);
	};
	auto plan = BuildFontVariantAuditPlan(file, catalog, {}, {}, provider);

	ASSERT_EQ(1u, requests.size());
	EXPECT_EQ("Dynamic", requests.front().family);
	EXPECT_EQ(128, requests.front().charset);
	EXPECT_DOUBLE_EQ(42.5, requests.front().height);
	ASSERT_EQ(1u, plan.findings.size());
	EXPECT_DOUBLE_EQ(42.5, plan.findings.front().height);
	EXPECT_EQ(FontSelectionEvidence::Observed,
	          plan.findings.front().profile_evidence);
	ASSERT_EQ(1u, plan.snapshots.size());
	EXPECT_DOUBLE_EQ(42.5, plan.snapshots.front().height);
	EXPECT_EQ(FontVariantAuditClassification::ImplicitBoldFallback,
	          plan.findings.front().classification);

	std::array<std::size_t, 1> selected{0};
	auto applied = ApplyFontVariantAuditChanges(file, plan, selected, {}, provider);
	ASSERT_TRUE(applied.success) << applied.error;
	EXPECT_TRUE(style.bold);
	ASSERT_EQ(2u, requests.size());
	EXPECT_EQ(128, requests.back().charset);
	EXPECT_DOUBLE_EQ(42.5, requests.back().height);
}

TEST(font_variant_audit, override_request_preserves_charset_and_source_font_size) {
	FontFamilyCatalog catalog({
		family(1, "Base", regular_profile()),
		family(2, "Dynamic", regular_profile()),
	});
	AssFile file;
	add_style(file, "Default", "Base");
	add_event(file, "{\\fnDynamic\\fe128\\fs37}text");

	std::optional<aegisub::ass::AssFontRequest> dynamic_request;
	FontVariantAuditProfileProvider provider = [&](auto const& request)
		-> std::optional<FontFamilyVariantProfile> {
		if (request.family == "Dynamic") {
			dynamic_request = request;
			return bold_only_profile(600);
		}
		return regular_profile();
	};
	auto plan = BuildFontVariantAuditPlan(file, catalog, {}, {}, provider);

	ASSERT_TRUE(dynamic_request.has_value());
	EXPECT_EQ(128, dynamic_request->charset);
	EXPECT_DOUBLE_EQ(37.0, dynamic_request->height);
	ASSERT_EQ(1u, plan.findings.size());
	EXPECT_EQ(128, plan.findings.front().charset);
	EXPECT_DOUBLE_EQ(37.0, plan.findings.front().height);
}

TEST(font_variant_audit, stale_style_font_size_rejects_fix) {
	FontFamilyCatalog catalog({family(1, "Bold Only", bold_only_profile())});
	AssFile file;
	auto& style = add_style(file, "Default", "Bold Only");
	style.fontsize = 24.0;
	auto plan = BuildFontVariantAuditPlan(file, catalog);
	ASSERT_EQ(1u, plan.findings.size());

	style.fontsize = 36.0;
	std::array<std::size_t, 1> selected{0};
	auto applied = ApplyFontVariantAuditChanges(file, plan, selected);
	EXPECT_FALSE(applied.success);
	EXPECT_FALSE(style.bold);
}

TEST(font_variant_audit, changed_inherited_font_size_rejects_override_fix) {
	FontFamilyCatalog catalog({
		family(1, "Base", regular_profile()),
		family(2, "Bold Only", bold_only_profile()),
	});
	AssFile file;
	auto& style = add_style(file, "Default", "Base");
	style.fontsize = 24.0;
	auto& event = add_event(file, "{\\fnBold Only}text");
	auto plan = BuildFontVariantAuditPlan(file, catalog);
	ASSERT_EQ(1u, plan.findings.size());
	ASSERT_EQ(FontVariantAuditSourceKind::OverrideSpan,
	          plan.findings.front().source.kind);

	style.fontsize = 36.0;
	std::array<std::size_t, 1> selected{0};
	auto applied = ApplyFontVariantAuditChanges(file, plan, selected);
	EXPECT_FALSE(applied.success);
	EXPECT_EQ("{\\fnBold Only}text", event.Text.get());
}

TEST(font_variant_audit, apply_rejects_when_live_target_no_longer_matches_entity) {
	FontFamilyCatalog catalog({family(1, "Bold Only", regular_profile())});
	AssFile file;
	auto& style = add_style(file, "Default", "Bold Only");
	FontVariantAuditProfileProvider initial = [](auto const&)
		-> std::optional<FontFamilyVariantProfile> {
		return bold_only_profile(700);
	};
	auto plan = BuildFontVariantAuditPlan(file, catalog, {}, {}, initial);
	ASSERT_EQ(1u, plan.findings.size());
	ASSERT_TRUE(plan.findings.front().safe_to_apply);
	std::array<std::size_t, 1> selected{0};

	FontVariantAuditProfileProvider replaced_entity = [](auto const&)
		-> std::optional<FontFamilyVariantProfile> {
		return bold_only_profile(702);
	};
	auto replaced = ApplyFontVariantAuditChanges(
		file, plan, selected, {}, replaced_entity);
	EXPECT_FALSE(replaced.success);
	EXPECT_FALSE(style.bold);

	FontVariantAuditProfileProvider changed_target = [](auto const&)
		-> std::optional<FontFamilyVariantProfile> {
		return profile(
			outcome(FontVariantRole::Bold, 700, FontVariantStatus::Canonical, 700),
			outcome(FontVariantRole::Bold, 701, FontVariantStatus::Canonical, 700));
	};
	auto applied = ApplyFontVariantAuditChanges(
		file, plan, selected, {}, changed_target);
	EXPECT_FALSE(applied.success);
	EXPECT_FALSE(style.bold);
	EXPECT_EQ(
		"font variant audit live profile changed after the plan was built",
		applied.error);
}

TEST(font_variant_audit, apply_rejects_changed_backend_or_evidence) {
	FontFamilyCatalog catalog({family(1, "Bold Only", regular_profile())});
	AssFile file;
	auto& style = add_style(file, "Default", "Bold Only");
	FontVariantAuditProfileProvider initial = [](auto const&)
		-> std::optional<FontFamilyVariantProfile> {
		return bold_only_profile(800);
	};
	auto plan = BuildFontVariantAuditPlan(file, catalog, {}, {}, initial);
	ASSERT_EQ(1u, plan.findings.size());
	ASSERT_TRUE(plan.findings.front().safe_to_apply);
	EXPECT_EQ(FontVariantBackend::VsFilterGdi,
	          plan.findings.front().profile_backend);
	EXPECT_EQ(FontSelectionEvidence::Observed,
	          plan.findings.front().profile_evidence);
	std::array<std::size_t, 1> selected{0};

	FontVariantAuditProfileProvider different_backend = [](auto const&)
		-> std::optional<FontFamilyVariantProfile> {
		auto profile = bold_only_profile(800);
		profile.backend = FontVariantBackend::CoreText;
		return profile;
	};
	auto different = ApplyFontVariantAuditChanges(
		file, plan, selected, {}, different_backend);
	EXPECT_FALSE(different.success);
	EXPECT_FALSE(style.bold);

	FontVariantAuditProfileProvider algorithmic = [](auto const&)
		-> std::optional<FontFamilyVariantProfile> {
		auto profile = bold_only_profile(800);
		profile.evidence = FontSelectionEvidence::Algorithmic;
		profile.automatic_pinning_reliable = true;
		return profile;
	};
	auto applied = ApplyFontVariantAuditChanges(
		file, plan, selected, {}, algorithmic);
	EXPECT_FALSE(applied.success);
	EXPECT_FALSE(style.bold);
	EXPECT_EQ(
		"font variant audit live profile changed after the plan was built",
		applied.error);
}
