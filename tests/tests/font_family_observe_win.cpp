#include <gtest/gtest.h>

#include "../../src/font_family_observe_win.h"
#include "../../src/gdi_font_resolver.h"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace {

FontFamilyFaceIdentity const* FaceAt(
	FontFamilyCatalogObservations const& obs,
	std::uint32_t ref) {
	if (ref == kFontFamilyInvalidFaceRef || ref >= obs.faces.size())
		return nullptr;
	return &obs.faces[ref];
}

void ExpectProbeSemanticEqual(
	FontFamilyCatalogObservations const& left_obs,
	FontFamilyProbeObservation const& left,
	FontFamilyCatalogObservations const& right_obs,
	FontFamilyProbeObservation const& right) {
	EXPECT_EQ(left.success, right.success);
	EXPECT_EQ(left.matches_requested_family, right.matches_requested_family);
	EXPECT_EQ(left.outcome.status, right.outcome.status);
	EXPECT_EQ(left.outcome.role, right.outcome.role);
	EXPECT_EQ(left.outcome.requested_weight, right.outcome.requested_weight);
	EXPECT_EQ(left.outcome.requested_italic, right.outcome.requested_italic);
	EXPECT_EQ(left.outcome.realized_weight, right.outcome.realized_weight);
	EXPECT_EQ(left.outcome.realized_italic, right.outcome.realized_italic);
	EXPECT_EQ(left.outcome.entity_token, right.outcome.entity_token);

	auto const* left_face = FaceAt(left_obs, left.face_ref);
	auto const* right_face = FaceAt(right_obs, right.face_ref);
	ASSERT_EQ(left_face == nullptr, right_face == nullptr);
	if (left_face && right_face)
		EXPECT_EQ(*left_face, *right_face);
}

void ExpectObservationsSemanticEqual(
	FontFamilyCatalogObservations const& left,
	FontFamilyCatalogObservations const& right) {
	ASSERT_EQ(left.complete, right.complete);
	ASSERT_EQ(left.seeds.size(), right.seeds.size());
	for (std::size_t i = 0; i < left.seeds.size(); ++i) {
		EXPECT_EQ(left.seeds[i].seed_family_name, right.seeds[i].seed_family_name);
		EXPECT_EQ(
			left.seeds[i].profile_matches_requested_family,
			right.seeds[i].profile_matches_requested_family);
		ASSERT_EQ(left.seeds[i].rbiz.size(), right.seeds[i].rbiz.size());
		for (std::size_t r = 0; r < left.seeds[i].rbiz.size(); ++r)
			ExpectProbeSemanticEqual(
				left, left.seeds[i].rbiz[r], right, right.seeds[i].rbiz[r]);
	}

	ASSERT_EQ(left.aliases.size(), right.aliases.size());
	std::map<std::string, std::size_t> right_alias_index;
	for (std::size_t i = 0; i < right.aliases.size(); ++i)
		right_alias_index.emplace(right.aliases[i].candidate_name, i);
	for (auto const& left_alias : left.aliases) {
		auto const it = right_alias_index.find(left_alias.candidate_name);
		ASSERT_NE(it, right_alias_index.end()) << left_alias.candidate_name;
		auto const& right_alias = right.aliases[it->second];
		ASSERT_EQ(left_alias.rbiz.size(), right_alias.rbiz.size());
		for (std::size_t r = 0; r < left_alias.rbiz.size(); ++r)
			ExpectProbeSemanticEqual(
				left, left_alias.rbiz[r], right, right_alias.rbiz[r]);
	}
}

} // namespace

TEST(font_family_observe_win, parallel_matches_serial_on_shared_seed_subset) {
	GdiFontResolver resolver;
	ASSERT_TRUE(resolver.available());
	auto all = resolver.EnumerateFamilies();
	ASSERT_FALSE(all.empty());

	// Bound wall time: enough seeds to exercise multi-worker merge/remap.
	constexpr std::size_t kMaxSeeds = 48;
	if (all.size() > kMaxSeeds)
		all.resize(kMaxSeeds);

	auto serial = ObserveWindowsFontFamilies(
		resolver, [] { return false; }, {.worker_count = 1, .seeds = &all});
	ASSERT_TRUE(serial.observations.complete);
	ASSERT_EQ(all.size(), serial.observations.seeds.size());

	auto parallel = ObserveWindowsFontFamilies(
		resolver, [] { return false; }, {.worker_count = 4, .seeds = &all});
	ASSERT_TRUE(parallel.observations.complete);
	ASSERT_EQ(all.size(), parallel.observations.seeds.size());

	ExpectObservationsSemanticEqual(serial.observations, parallel.observations);

	// Probe totals should be in the same ballpark (alias pass is sequential on
	// both paths; seed probes scale with seed count × RBIZ).
	EXPECT_GT(serial.physical_probe_count, 0u);
	EXPECT_GT(parallel.physical_probe_count, 0u);
}

TEST(font_family_observe_win, empty_seed_list_is_complete_without_probes) {
	GdiFontResolver resolver;
	ASSERT_TRUE(resolver.available());
	std::vector<std::string> empty;
	auto result = ObserveWindowsFontFamilies(
		resolver, [] { return false; }, {.worker_count = 4, .seeds = &empty});
	EXPECT_TRUE(result.observations.complete);
	EXPECT_TRUE(result.observations.seeds.empty());
	EXPECT_TRUE(result.observations.aliases.empty());
	EXPECT_EQ(0u, result.physical_probe_count);
}

TEST(font_family_observe_win, incremental_reuses_unchanged_seeds) {
	GdiFontResolver resolver;
	ASSERT_TRUE(resolver.available());
	auto all = resolver.EnumerateFamilies();
	ASSERT_FALSE(all.empty());
	constexpr std::size_t kMaxSeeds = 40;
	if (all.size() > kMaxSeeds)
		all.resize(kMaxSeeds);

	auto full = ObserveWindowsFontFamilies(
		resolver, [] { return false; }, {.worker_count = 2, .seeds = &all});
	ASSERT_TRUE(full.observations.complete);
	ASSERT_EQ(all.size(), full.observations.seeds.size());

	auto again = ObserveWindowsFontFamilies(
		resolver, [] { return false; },
		{.worker_count = 2, .seeds = &all, .previous = &full.observations});
	ASSERT_TRUE(again.observations.complete);
	EXPECT_TRUE(again.used_incremental);
	EXPECT_EQ(all.size(), static_cast<std::size_t>(again.reused_seed_count));
	EXPECT_EQ(0u, again.probed_seed_count);
	// Seed RBIZ reused → no seed probes; alias pass is copy-only for owned names.
	EXPECT_LT(again.physical_probe_count, full.physical_probe_count);
	ExpectObservationsSemanticEqual(full.observations, again.observations);
}

TEST(font_family_observe_win, incremental_probes_only_missing_seed) {
	GdiFontResolver resolver;
	ASSERT_TRUE(resolver.available());
	auto all = resolver.EnumerateFamilies();
	ASSERT_GE(all.size(), 3u);
	constexpr std::size_t kMaxSeeds = 40;
	if (all.size() > kMaxSeeds)
		all.resize(kMaxSeeds);

	auto full = ObserveWindowsFontFamilies(
		resolver, [] { return false; }, {.worker_count = 1, .seeds = &all});
	ASSERT_TRUE(full.observations.complete);

	// Simulate a prior snapshot that is missing the last enumerated seed.
	auto previous = full.observations;
	ASSERT_FALSE(previous.seeds.empty());
	previous.seeds.pop_back();

	auto inc = ObserveWindowsFontFamilies(
		resolver, [] { return false; },
		{.worker_count = 1, .seeds = &all, .previous = &previous});
	ASSERT_TRUE(inc.observations.complete);
	EXPECT_TRUE(inc.used_incremental);
	EXPECT_EQ(1u, inc.probed_seed_count);
	EXPECT_EQ(all.size() - 1, static_cast<std::size_t>(inc.reused_seed_count));
	EXPECT_LT(inc.physical_probe_count, full.physical_probe_count);
	ExpectObservationsSemanticEqual(full.observations, inc.observations);
}

TEST(font_family_observe_win, incremental_works_after_token_sanitize) {
	// Production previous is disk-loaded then SanitizeLoadedTransientTokens.
	GdiFontResolver resolver;
	ASSERT_TRUE(resolver.available());
	auto all = resolver.EnumerateFamilies();
	ASSERT_FALSE(all.empty());
	constexpr std::size_t kMaxSeeds = 24;
	if (all.size() > kMaxSeeds)
		all.resize(kMaxSeeds);

	auto full = ObserveWindowsFontFamilies(
		resolver, [] { return false; }, {.worker_count = 1, .seeds = &all});
	ASSERT_TRUE(full.observations.complete);

	auto previous = full.observations;
	for (auto& seed : previous.seeds) {
		for (auto& probe : seed.rbiz)
			probe.outcome.entity_token = 0;
		for (auto& name : seed.win32_family_names)
			name.entity_token = 0;
	}
	for (auto& alias : previous.aliases)
		for (auto& probe : alias.rbiz)
			probe.outcome.entity_token = 0;

	auto inc = ObserveWindowsFontFamilies(
		resolver, [] { return false; },
		{.worker_count = 1, .seeds = &all, .previous = &previous});
	ASSERT_TRUE(inc.observations.complete);
	EXPECT_TRUE(inc.used_incremental);
	EXPECT_EQ(0u, inc.probed_seed_count);
	EXPECT_EQ(all.size(), static_cast<std::size_t>(inc.reused_seed_count));
	// Compare against a sanitized full snapshot: production previous has tokens
	// cleared; durable identity is face_ref / FaceIdentity, not process tokens.
	auto full_sanitized = full.observations;
	for (auto& seed : full_sanitized.seeds) {
		for (auto& probe : seed.rbiz)
			probe.outcome.entity_token = 0;
		for (auto& name : seed.win32_family_names)
			name.entity_token = 0;
	}
	for (auto& alias : full_sanitized.aliases)
		for (auto& probe : alias.rbiz)
			probe.outcome.entity_token = 0;
	ExpectObservationsSemanticEqual(full_sanitized, inc.observations);
}

TEST(font_family_observe_win, weak_previous_forces_full_observe) {
	// success + entity_token + no face_ref is not durable; must not reuse.
	GdiFontResolver resolver;
	ASSERT_TRUE(resolver.available());
	auto all = resolver.EnumerateFamilies();
	ASSERT_FALSE(all.empty());
	if (all.size() > 8)
		all.resize(8);

	FontFamilyCatalogObservations weak;
	weak.complete = true;
	for (auto const& name : all) {
		FontFamilySeedObservation seed;
		seed.seed_family_name = name;
		seed.profile_matches_requested_family = true;
		for (auto& probe : seed.rbiz) {
			probe.success = true;
			probe.matches_requested_family = true;
			probe.face_ref = kFontFamilyInvalidFaceRef;
			probe.outcome.entity_token = 0xBEEF;
		}
		weak.seeds.push_back(std::move(seed));
	}

	auto result = ObserveWindowsFontFamilies(
		resolver, [] { return false; },
		{.worker_count = 1, .seeds = &all, .previous = &weak});
	ASSERT_TRUE(result.observations.complete);
	EXPECT_FALSE(result.used_incremental);
	EXPECT_EQ(0u, result.reused_seed_count);
	EXPECT_EQ(all.size(), static_cast<std::size_t>(result.probed_seed_count));
	EXPECT_GT(result.physical_probe_count, 0u);
}
