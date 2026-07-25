#include <gtest/gtest.h>

#include "../../src/font_family_derive_win.h"

#include <array>
#include <string>
#include <utility>

namespace {

FontFamilyProbeObservation probe(
	std::uint64_t token,
	int requested_weight,
	bool requested_italic,
	FontVariantRole role) {
	FontFamilyProbeObservation result;
	result.success = true;
	result.matches_requested_family = true;
	result.outcome = {
		requested_weight, requested_italic, requested_weight, requested_italic,
		role, FontVariantStatus::Canonical, token};
	return result;
}

std::array<FontFamilyProbeObservation, 4> profile(std::uint64_t token) {
	return {
		probe(token, 400, false, FontVariantRole::Regular),
		probe(token + 1, 700, false, FontVariantRole::Bold),
		probe(token + 2, 400, true, FontVariantRole::Italic),
		probe(token + 3, 700, true, FontVariantRole::BoldItalic),
	};
}

FontFamilySeedObservation seed(
	std::string name,
	std::uint64_t token,
	std::vector<FontFamilyName> names = {}) {
	FontFamilySeedObservation result;
	result.seed_family_name = std::move(name);
	result.rbiz = profile(token);
	result.profile_matches_requested_family = true;
	result.win32_family_names = std::move(names);
	return result;
}

FontFamilyAliasObservation alias(std::string name, std::uint64_t token) {
	FontFamilyAliasObservation result;
	result.candidate_name = std::move(name);
	result.rbiz = profile(token);
	return result;
}

} // namespace

TEST(font_family_derive_win, merges_physical_seeds_and_uses_recorded_english_alias) {
	FontFamilyCatalogObservations observations;
	observations.complete = true;
	observations.seeds = {
		seed("Localized A", 10, {{"English Alias", "en-US", FontFamilyNameKind::Win32Family}}),
		seed("Localized B", 10),
	};
	observations.aliases = {
		alias("Localized A", 10),
		alias("Localized B", 10),
		alias("English Alias", 10),
	};

	auto const catalog = DeriveWindowsFontFamilyCatalog(observations, "en-US");
	ASSERT_EQ(1u, catalog.size());
	auto const& record = catalog.records().front();
	EXPECT_EQ(1u, record.id);
	EXPECT_EQ(1u, catalog.records().front().id);
	EXPECT_EQ("English Alias", record.english_win32_family_name);
	EXPECT_EQ(FontFamilyMatchKind::Exact, catalog.Resolve("Localized B").match);
}

TEST(font_family_derive_win, never_accepts_an_alias_without_a_matching_observation) {
	FontFamilyCatalogObservations observations;
	observations.complete = true;
	observations.seeds = {seed(
		"Localized", 20,
		{{"Unverified English", "en-US", FontFamilyNameKind::Win32Family}})};
	observations.aliases = {alias("Localized", 20)};

	auto const catalog = DeriveWindowsFontFamilyCatalog(observations, "en-US");
	ASSERT_EQ(1u, catalog.size());
	EXPECT_TRUE(catalog.records().front().english_win32_family_name.empty());
}

TEST(font_family_derive_win, replays_locale_selection_without_live_gdi) {
	FontFamilyCatalogObservations observations;
	observations.complete = true;
	observations.seeds = {seed(
		"Fallback", 30,
		{{"Localized Name", "zh-CN", FontFamilyNameKind::Win32Family},
		 {"English Name", "en-US", FontFamilyNameKind::Win32Family}})};
	observations.aliases = {
		alias("Fallback", 30),
		alias("Localized Name", 30),
		alias("English Name", 30),
	};

	auto const catalog = DeriveWindowsFontFamilyCatalog(observations, "zh-Hans-CN");
	ASSERT_EQ(1u, catalog.size());
	EXPECT_EQ("Localized Name", catalog.records().front().localized_family_name);
	EXPECT_EQ("English Name", catalog.records().front().english_win32_family_name);
}

TEST(font_family_derive_win, rejects_partial_observations) {
	FontFamilyCatalogObservations observations;
	observations.seeds = {seed("Partial", 40)};
	EXPECT_TRUE(DeriveWindowsFontFamilyCatalog(observations, "en-US").empty());
}

TEST(font_family_derive_win, clears_ambiguous_english_names_after_full_replay) {
	FontFamilyCatalogObservations observations;
	observations.complete = true;
	auto first = seed(
		"First", 50,
		{{"Shared English", "en-US", FontFamilyNameKind::Win32Family}});
	auto second = seed(
		"Second", 50,
		{{"Shared English", "en-US", FontFamilyNameKind::Win32Family}});
	// Records with untrusted profiles must not merge, but an English alias is
	// still only publishable after the full-record ambiguity pass.
	first.profile_matches_requested_family = false;
	second.profile_matches_requested_family = false;
	observations.seeds = {std::move(first), std::move(second)};
	observations.aliases = {
		alias("First", 50), alias("Second", 50), alias("Shared English", 50)};

	auto const catalog = DeriveWindowsFontFamilyCatalog(observations, "en-US");
	ASSERT_EQ(2u, catalog.size());
	EXPECT_EQ(1u, catalog.records()[0].id);
	EXPECT_EQ(2u, catalog.records()[1].id);
	EXPECT_TRUE(catalog.records()[0].english_win32_family_name.empty());
	EXPECT_TRUE(catalog.records()[1].english_win32_family_name.empty());
}

TEST(font_family_derive_win, never_merges_records_with_an_unknown_regular_entity) {
	FontFamilyCatalogObservations observations;
	observations.complete = true;
	auto first = seed("Unknown A", 60);
	auto second = seed("Unknown B", 60);
	first.rbiz[0].outcome.entity_token = 0;
	second.rbiz[0].outcome.entity_token = 0;
	first.profile_matches_requested_family = false;
	second.profile_matches_requested_family = false;
	observations.seeds = {std::move(first), std::move(second)};
	observations.aliases = {alias("Unknown A", 60), alias("Unknown B", 60)};

	auto const catalog = DeriveWindowsFontFamilyCatalog(observations, "en-US");
	ASSERT_EQ(2u, catalog.size());
	EXPECT_EQ(1u, catalog.records()[0].id);
	EXPECT_EQ(2u, catalog.records()[1].id);
}

TEST(font_family_derive_win, interns_live_tokens_to_dense_snapshot_ids) {
	// Without FaceIdentity, LiveVariantKey maps equal process tokens to the
	// same dense snapshot token (not the raw process value).
	FontFamilyCatalogObservations observations;
	observations.complete = true;
	observations.seeds = {seed("Live", 900)};
	observations.aliases = {alias("Live", 900)};

	auto const catalog = DeriveWindowsFontFamilyCatalog(observations, "en-US");
	ASSERT_EQ(1u, catalog.size());
	auto const& outcomes = catalog.records().front().variant_profile.outcomes;
	// Four distinct process tokens → four dense ids starting at 1.
	EXPECT_EQ(1u, outcomes[0].entity_token);
	EXPECT_EQ(2u, outcomes[1].entity_token);
	EXPECT_EQ(3u, outcomes[2].entity_token);
	EXPECT_EQ(4u, outcomes[3].entity_token);
}

TEST(font_family_derive_win, durable_identity_merges_regardless_of_process_token) {
	// Two seeds share FaceIdentity + realized style but different process
	// tokens; DurableVariantKey must still unify them.
	FontFamilyFaceIdentity face;
	face.volume_serial = 0xAABBCCDDu;
	face.file_index = 0x1122334455667788ull;
	face.face_index = 0;
	face.size = 4096;
	face.mtime_utc_100ns = 1000;

	FontFamilyCatalogObservations observations;
	observations.complete = true;
	observations.faces = {face};

	auto make_seed = [&](std::string name, std::uint64_t process_token) {
		FontFamilySeedObservation s;
		s.seed_family_name = std::move(name);
		s.profile_matches_requested_family = true;
		for (std::size_t i = 0; i < 4; ++i) {
			bool const italic = i >= 2;
			bool const bold = (i & 1u) != 0;
			int const weight = bold ? 700 : 400;
			s.rbiz[i].success = true;
			s.rbiz[i].matches_requested_family = true;
			s.rbiz[i].face_ref = 0;
			s.rbiz[i].outcome = {
				weight, italic, weight, italic,
				bold ? (italic ? FontVariantRole::BoldItalic : FontVariantRole::Bold)
				     : (italic ? FontVariantRole::Italic : FontVariantRole::Regular),
				FontVariantStatus::Canonical,
				process_token + i};
		}
		return s;
	};

	observations.seeds = {make_seed("A", 1000), make_seed("B", 2000)};
	// Aliases use the same durable face so english/locale mapping still works.
	auto make_alias = [&](std::string name, std::uint64_t process_token) {
		FontFamilyAliasObservation a;
		a.candidate_name = std::move(name);
		for (std::size_t i = 0; i < 4; ++i) {
			bool const italic = i >= 2;
			bool const bold = (i & 1u) != 0;
			int const weight = bold ? 700 : 400;
			a.rbiz[i].success = true;
			a.rbiz[i].matches_requested_family = true;
			a.rbiz[i].face_ref = 0;
			a.rbiz[i].outcome = {
				weight, italic, weight, italic,
				bold ? (italic ? FontVariantRole::BoldItalic : FontVariantRole::Bold)
				     : (italic ? FontVariantRole::Italic : FontVariantRole::Regular),
				FontVariantStatus::Canonical,
				process_token + i};
		}
		return a;
	};
	observations.aliases = {make_alias("A", 1000), make_alias("B", 2000)};

	auto const catalog = DeriveWindowsFontFamilyCatalog(observations, "en-US");
	ASSERT_EQ(1u, catalog.size());
	EXPECT_EQ(FontFamilyMatchKind::Exact, catalog.Resolve("B").match);
}

TEST(font_family_derive_win, reassigns_informational_name_tokens_from_probe) {
	FontFamilyCatalogObservations observations;
	observations.complete = true;
	auto s = seed("Info", 70);
	// Stale serialized token on informational name must be overwritten.
	s.rbiz[0].informational_names.push_back(
		{"Info Full", "en-US", FontFamilyNameKind::FullName, 99999,
			FontVariantRole::Regular});
	observations.seeds = {std::move(s)};
	observations.aliases = {alias("Info", 70)};

	auto const catalog = DeriveWindowsFontFamilyCatalog(observations, "en-US");
	ASSERT_EQ(1u, catalog.size());
	auto const regular_token =
		catalog.records().front().variant_profile.outcomes[0].entity_token;
	ASSERT_NE(0u, regular_token);
	bool found = false;
	for (auto const& name : catalog.records().front().names) {
		if (name.kind == FontFamilyNameKind::FullName && name.value == "Info Full") {
			EXPECT_EQ(regular_token, name.entity_token);
			found = true;
		}
	}
	EXPECT_TRUE(found);
}

TEST(font_family_derive_win, mixed_durable_and_live_keys_do_not_collide) {
	FontFamilyFaceIdentity face;
	face.volume_serial = 1;
	face.file_index = 2;
	face.face_index = 0;
	face.size = 3;
	face.mtime_utc_100ns = 4;

	FontFamilyCatalogObservations observations;
	observations.complete = true;
	observations.faces = {face};

	auto durable = seed("Durable", 10);
	for (auto& p : durable.rbiz)
		p.face_ref = 0;
	// Live seed uses the same process tokens but no face_ref: LiveVariantKey
	// space must not collide with DurableVariantKey.
	auto live = seed("Live", 10);
	for (auto& p : live.rbiz)
		p.face_ref = kFontFamilyInvalidFaceRef;
	live.profile_matches_requested_family = false;
	durable.profile_matches_requested_family = false;

	observations.seeds = {std::move(durable), std::move(live)};
	observations.aliases = {alias("Durable", 10), alias("Live", 10)};

	auto const catalog = DeriveWindowsFontFamilyCatalog(observations, "en-US");
	ASSERT_EQ(2u, catalog.size());
	// Distinct key spaces → distinct snapshot tokens for Regular.
	EXPECT_NE(
		catalog.records()[0].variant_profile.outcomes[0].entity_token,
		catalog.records()[1].variant_profile.outcomes[0].entity_token);
}
