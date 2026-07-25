#include <gtest/gtest.h>

#include "../../src/font_family_derive_win.h"
#include "../../src/font_family_obs_repository_win.h"
#include "../../src/font_family_obs_store.h"
#include "../../src/gdi_font_resolver.h"
#include "../../src/options.h"

#include <libaegisub/fs.h>
#include <libaegisub/io.h>
#include <libaegisub/path.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

class ScopedLocalPath {
	std::unique_ptr<agi::Path> path_;
	agi::Path* previous_ = nullptr;

public:
	explicit ScopedLocalPath(agi::fs::path const& local_root) {
		previous_ = config::path;
		path_ = std::make_unique<agi::Path>();
		path_->SetToken("?local", local_root);
		// Path constructor fills platform defaults; override ?local after.
		path_->SetToken("?local", local_root);
		config::path = path_.get();
	}
	~ScopedLocalPath() { config::path = previous_; }
};

void WriteLe32(std::string& bytes, std::size_t offset, std::uint32_t value) {
	ASSERT_GE(bytes.size(), offset + 4u);
	bytes[offset + 0] = static_cast<char>(value & 0xFFu);
	bytes[offset + 1] = static_cast<char>((value >> 8) & 0xFFu);
	bytes[offset + 2] = static_cast<char>((value >> 16) & 0xFFu);
	bytes[offset + 3] = static_cast<char>((value >> 24) & 0xFFu);
}

std::string ReadFileBytes(agi::fs::path const& path) {
	auto stream = agi::io::Open(path, true);
	auto const size = agi::fs::Size(path);
	std::string bytes(static_cast<std::size_t>(size), '\0');
	if (size > 0)
		stream->read(bytes.data(), static_cast<std::streamsize>(size));
	return bytes;
}

void WriteFileBytes(agi::fs::path const& path, std::string const& bytes) {
	agi::io::Save save(path, true);
	save.Get().write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
	save.Close();
}

void ExpectSameCatalogRecords(
	FontFamilyCatalog const& left,
	FontFamilyCatalog const& right) {
	ASSERT_EQ(left.size(), right.size());
	for (std::size_t i = 0; i < left.records().size(); ++i) {
		auto const& a = left.records()[i];
		auto const& b = right.records()[i];
		EXPECT_EQ(a.id, b.id) << "record " << i;
		EXPECT_EQ(a.localized_family_name, b.localized_family_name) << "record " << i;
		EXPECT_EQ(a.english_win32_family_name, b.english_win32_family_name)
			<< "record " << i;
		ASSERT_EQ(a.names.size(), b.names.size()) << "record " << i;
		for (std::size_t n = 0; n < a.names.size(); ++n) {
			EXPECT_EQ(a.names[n].value, b.names[n].value) << "record " << i;
			EXPECT_EQ(a.names[n].locale, b.names[n].locale) << "record " << i;
			EXPECT_EQ(a.names[n].kind, b.names[n].kind) << "record " << i;
			EXPECT_EQ(a.names[n].entity_token, b.names[n].entity_token) << "record " << i;
			EXPECT_EQ(a.names[n].variant_role, b.names[n].variant_role) << "record " << i;
		}
		EXPECT_EQ(a.variant_profile.backend, b.variant_profile.backend) << "record " << i;
		EXPECT_EQ(a.variant_profile.evidence, b.variant_profile.evidence) << "record " << i;
		EXPECT_EQ(
			a.variant_profile.automatic_pinning_reliable,
			b.variant_profile.automatic_pinning_reliable)
			<< "record " << i;
		for (std::size_t o = 0; o < a.variant_profile.outcomes.size(); ++o) {
			auto const& lo = a.variant_profile.outcomes[o];
			auto const& ro = b.variant_profile.outcomes[o];
			EXPECT_EQ(lo.requested_weight, ro.requested_weight) << "record " << i;
			EXPECT_EQ(lo.requested_italic, ro.requested_italic) << "record " << i;
			EXPECT_EQ(lo.realized_weight, ro.realized_weight) << "record " << i;
			EXPECT_EQ(lo.realized_italic, ro.realized_italic) << "record " << i;
			EXPECT_EQ(lo.role, ro.role) << "record " << i;
			EXPECT_EQ(lo.status, ro.status) << "record " << i;
			EXPECT_EQ(lo.entity_token, ro.entity_token) << "record " << i;
		}
	}
}

// Header: observation_contract_version at offset 8, derivation at 12, storage at 4.
constexpr std::size_t kHdrStorageSchema = 4;
constexpr std::size_t kHdrObservationContract = 8;
constexpr std::size_t kHdrDerivationContract = 12;

} // namespace

TEST(font_family_obs_repository_win, m1_rejects_unknown_face_identity) {
	FontFamilyFaceIdentity bogus;
	bogus.volume_serial = 0x11111111u;
	bogus.file_index = 0x2222222233333333ull;
	bogus.face_index = 0;
	bogus.size = 1;
	bogus.mtime_utc_100ns = 1;
	EXPECT_FALSE(ValidateWindowsFontFamilyFaceStamps({bogus}));
	// Vacuous success: repository treats faces.empty() as weak_m1, not strong M1.
	EXPECT_TRUE(ValidateWindowsFontFamilyFaceStamps({}));
}

TEST(font_family_obs_repository_win, m0_includes_font_registry_fingerprint) {
	FontFamilyInputManifest left;
	left.provider_fingerprint = 1;
	left.os_build_fingerprint = 2;
	left.font_registry_fingerprint = 3;
	left.gdi_family_names = {"Example"};
	auto right = left;

	EXPECT_TRUE(WindowsFontFamilyManifestsMatchM0(left, right));
	right.font_registry_fingerprint = 4;
	EXPECT_FALSE(WindowsFontFamilyManifestsMatchM0(left, right));
	right = left;
	right.gdi_family_names = {"example"};
	EXPECT_TRUE(WindowsFontFamilyManifestsMatchM0(left, right));
}

TEST(font_family_obs_repository_win, observe_stability_requires_full_m0_and_seed_match) {
	FontFamilyInputManifest before;
	before.provider_fingerprint = 1;
	before.os_build_fingerprint = 2;
	before.font_registry_fingerprint = 3;
	before.gdi_family_names = {"Example"};
	auto after = before;

	EXPECT_TRUE(WindowsFontFamilyObservationM0Stable(
		before, after, {"example"}));
	after.font_registry_fingerprint = 4;
	EXPECT_FALSE(WindowsFontFamilyObservationM0Stable(
		before, after, {"Example"}));
	after = before;
	EXPECT_FALSE(WindowsFontFamilyObservationM0Stable(
		before, after, {"Different"}));
}

TEST(font_family_obs_repository_win, default_cache_path_uses_fixed_afco_name) {
	auto const root = (std::filesystem::current_path() /
		"build-dir" / "font-family-obs-path-test").lexically_normal();
	agi::fs::CreateDirectory(root);
	ScopedLocalPath scoped(root);
	auto const path = DefaultWindowsFontFamilyObsCachePath();

	EXPECT_EQ("windows-font-observations.afco", path.filename().string());
	EXPECT_EQ("font_family_catalog", path.parent_path().filename().string());
}

TEST(font_family_obs_repository_win, m1_dedupes_ttc_faces_on_same_file_identity) {
	// Two face_index rows on an unknown file must still fail once, without
	// requiring a second OpenFileById success path to exist on this host.
	FontFamilyFaceIdentity face_a;
	face_a.volume_serial = 0xABCDEF01u;
	face_a.file_index = 0x1020304050607080ull;
	face_a.face_index = 0;
	face_a.size = 100;
	face_a.mtime_utc_100ns = 200;
	auto face_b = face_a;
	face_b.face_index = 1;
	EXPECT_FALSE(ValidateWindowsFontFamilyFaceStamps({face_a, face_b}));
}

TEST(font_family_obs_repository_win, second_load_hits_without_physical_probes) {
	// agi::Path::SetToken requires an absolute token value; relative roots are
	// intentionally cleared rather than resolved against the process cwd.
	auto const root = (std::filesystem::current_path() /
		"build-dir" / "font-family-obs-repository-test").lexically_normal();
	agi::fs::CreateDirectory(root);
	ScopedLocalPath scoped(root);
	auto const cache_path = DefaultWindowsFontFamilyObsCachePath();
	ASSERT_FALSE(cache_path.empty());
	ASSERT_TRUE(cache_path.is_absolute()) << agi::fs::PathToString(cache_path);
	try {
		agi::fs::Remove(cache_path);
	}
	catch (...) {
	}

	GdiFontResolver first_resolver;
	auto const first = LoadOrRebuildWindowsFontFamilyCatalog(
		first_resolver, "en-US", [] { return false; }, cache_path);
	ASSERT_NE(FontFamilyObsHitKind::RebuildAborted, first.hit);
	ASSERT_FALSE(first.catalog.empty()) << "system should expose at least one GDI family";
	EXPECT_EQ(FontFamilyObsHitKind::Rebuilt, first.hit);
	EXPECT_GT(first.physical_probe_count, 0u);
	ASSERT_TRUE(first.wrote_cache) << "first cold build must publish a complete cache";

	// Cold resolver, warm cache: M0+M1 must admit a zero-probe hit.
	GdiFontResolver second_resolver;
	auto const second = LoadOrRebuildWindowsFontFamilyCatalog(
		second_resolver, "en-US", [] { return false; }, cache_path);
	EXPECT_EQ(FontFamilyObsHitKind::Hit, second.hit)
		<< "store=" << FontFamilyObsStoreStatusName(second.store_status);
	EXPECT_EQ(0u, second.physical_probe_count);
	EXPECT_EQ(first.catalog.size(), second.catalog.size());
	EXPECT_EQ(first.face_count, second.face_count);
	// Real systems with DWrite paths should publish durable faces; empty is
	// allowed but must be surfaced as weak_m1 rather than silent strong trust.
	if (second.face_count == 0)
		EXPECT_TRUE(second.weak_m1);
	else
		EXPECT_FALSE(second.weak_m1);
	// Cold rebuild that wrote a strong cache should report strong coverage.
	if (first.wrote_cache)
		EXPECT_TRUE(first.coverage.strong);
	if (second.hit == FontFamilyObsHitKind::Hit && !second.weak_m1)
		EXPECT_TRUE(second.coverage.strong);
	// Deep field compare: size/face_count alone is not enough (#9).
	ExpectSameCatalogRecords(first.catalog, second.catalog);
}

TEST(font_family_obs_repository_win, observation_contract_bump_forces_rebuild) {
	auto const root = (std::filesystem::current_path() /
		"build-dir" / "font-family-obs-contract-obs").lexically_normal();
	agi::fs::CreateDirectory(root);
	ScopedLocalPath scoped(root);
	auto const cache_path = DefaultWindowsFontFamilyObsCachePath();
	ASSERT_FALSE(cache_path.empty());
	try {
		agi::fs::Remove(cache_path);
	}
	catch (...) {
	}

	GdiFontResolver builder;
	auto const first = LoadOrRebuildWindowsFontFamilyCatalog(
		builder, "en-US", [] { return false; }, cache_path);
	ASSERT_EQ(FontFamilyObsHitKind::Rebuilt, first.hit);
	ASSERT_TRUE(first.wrote_cache);

	auto bytes = ReadFileBytes(cache_path);
	WriteLe32(bytes, kHdrObservationContract, kFontFamilyObsObservationContractVersion + 1);
	WriteFileBytes(cache_path, bytes);

	GdiFontResolver second_resolver;
	auto const second = LoadOrRebuildWindowsFontFamilyCatalog(
		second_resolver, "en-US", [] { return false; }, cache_path);
	// Contract mismatch must rebuild (physical probes), not admit a hit.
	EXPECT_EQ(FontFamilyObsHitKind::Rebuilt, second.hit);
	EXPECT_GT(second.physical_probe_count, 0u);
}

TEST(font_family_obs_repository_win, derivation_contract_bump_rederives_without_probes) {
	auto const root = (std::filesystem::current_path() /
		"build-dir" / "font-family-obs-contract-derive").lexically_normal();
	agi::fs::CreateDirectory(root);
	ScopedLocalPath scoped(root);
	auto const cache_path = DefaultWindowsFontFamilyObsCachePath();
	ASSERT_FALSE(cache_path.empty());
	try {
		agi::fs::Remove(cache_path);
	}
	catch (...) {
	}

	GdiFontResolver builder;
	auto const first = LoadOrRebuildWindowsFontFamilyCatalog(
		builder, "en-US", [] { return false; }, cache_path);
	ASSERT_EQ(FontFamilyObsHitKind::Rebuilt, first.hit);
	ASSERT_TRUE(first.wrote_cache);

	auto bytes = ReadFileBytes(cache_path);
	// Older derivation contract, same observation contract and payload CRC.
	WriteLe32(bytes, kHdrDerivationContract, kFontFamilyObsDerivationContractVersion + 99);
	WriteFileBytes(cache_path, bytes);

	GdiFontResolver second_resolver;
	auto const second = LoadOrRebuildWindowsFontFamilyCatalog(
		second_resolver, "en-US", [] { return false; }, cache_path);
	EXPECT_EQ(FontFamilyObsHitKind::Hit, second.hit)
		<< FontFamilyObsHitKindName(second.hit);
	EXPECT_EQ(0u, second.physical_probe_count);
	ExpectSameCatalogRecords(first.catalog, second.catalog);
}

TEST(font_family_obs_repository_win, storage_schema_bump_is_load_miss_then_rebuild) {
	auto const root = (std::filesystem::current_path() /
		"build-dir" / "font-family-obs-contract-schema").lexically_normal();
	agi::fs::CreateDirectory(root);
	ScopedLocalPath scoped(root);
	auto const cache_path = DefaultWindowsFontFamilyObsCachePath();
	ASSERT_FALSE(cache_path.empty());
	try {
		agi::fs::Remove(cache_path);
	}
	catch (...) {
	}

	GdiFontResolver builder;
	auto const first = LoadOrRebuildWindowsFontFamilyCatalog(
		builder, "en-US", [] { return false; }, cache_path);
	ASSERT_TRUE(first.wrote_cache);

	auto bytes = ReadFileBytes(cache_path);
	WriteLe32(bytes, kHdrStorageSchema, kFontFamilyObsStorageSchemaVersion + 1);
	WriteFileBytes(cache_path, bytes);

	// Direct store path: BadSchema.
	FontFamilyObsStorePayload loaded;
	EXPECT_EQ(FontFamilyObsStoreStatus::BadSchema, TryLoadFontFamilyObsStore(cache_path, loaded));

	GdiFontResolver second_resolver;
	auto const second = LoadOrRebuildWindowsFontFamilyCatalog(
		second_resolver, "en-US", [] { return false; }, cache_path);
	EXPECT_EQ(FontFamilyObsHitKind::Rebuilt, second.hit);
	EXPECT_EQ(FontFamilyObsStoreStatus::BadSchema, second.store_status);
	EXPECT_GT(second.physical_probe_count, 0u);
}

TEST(font_family_obs_repository_win, coverage_strong_when_successful_probes_covered) {
	FontFamilyFaceIdentity face;
	face.volume_serial = 1;
	face.file_index = 2;
	face.face_index = 0;
	face.size = 10;
	face.mtime_utc_100ns = 20;

	FontFamilyCatalogObservations observations;
	observations.faces = {face};
	FontFamilySeedObservation seed;
	seed.seed_family_name = "Covered";
	for (auto& probe : seed.rbiz) {
		probe.success = true;
		probe.matches_requested_family = true;
		probe.face_ref = 0;
		probe.outcome.entity_token = 1;
	}
	observations.seeds = {std::move(seed)};
	observations.complete = true;

	auto const coverage = AssessObservationCoverage(observations);
	EXPECT_TRUE(coverage.strong);
	EXPECT_EQ(4u, coverage.successful_probes);
	EXPECT_EQ(4u, coverage.covered_probes);
	EXPECT_EQ(0u, coverage.missing_face_refs);
	EXPECT_TRUE(coverage.reason.empty());
}

TEST(font_family_obs_repository_win, coverage_weak_when_successful_probe_missing_face_ref) {
	// Non-zero process token means some physical entity was observed; missing
	// face_ref then blocks strong coverage.
	FontFamilyCatalogObservations observations;
	FontFamilySeedObservation seed;
	seed.seed_family_name = "Missing";
	for (auto& probe : seed.rbiz) {
		probe.success = true;
		probe.face_ref = kFontFamilyInvalidFaceRef;
		probe.outcome.entity_token = 1;
	}
	observations.seeds = {std::move(seed)};
	observations.complete = true;

	auto const coverage = AssessObservationCoverage(observations);
	EXPECT_FALSE(coverage.strong);
	EXPECT_EQ(4u, coverage.successful_probes);
	EXPECT_EQ(0u, coverage.covered_probes);
	EXPECT_EQ(4u, coverage.missing_face_refs);
	EXPECT_FALSE(coverage.reason.empty());
}

TEST(font_family_obs_repository_win, coverage_allows_no_entity_bitmap_style_probes) {
	// success + token 0 + no face_ref: device/bitmap fonts. Do not block strong.
	FontFamilyCatalogObservations observations;
	FontFamilySeedObservation seed;
	seed.seed_family_name = "Fixedsys";
	for (auto& probe : seed.rbiz) {
		probe.success = true;
		probe.face_ref = kFontFamilyInvalidFaceRef;
		probe.outcome.entity_token = 0;
	}
	observations.seeds = {std::move(seed)};
	observations.complete = true;

	auto const coverage = AssessObservationCoverage(observations);
	EXPECT_TRUE(coverage.strong);
	EXPECT_EQ(4u, coverage.successful_probes);
	EXPECT_EQ(4u, coverage.covered_probes);
	EXPECT_EQ(4u, coverage.no_entity_probes);
	EXPECT_EQ(0u, coverage.missing_face_refs);
}

TEST(font_family_obs_repository_win, coverage_weak_when_face_ref_out_of_range) {
	FontFamilyFaceIdentity face;
	face.volume_serial = 1;
	face.file_index = 2;
	face.face_index = 0;
	face.size = 10;
	face.mtime_utc_100ns = 20;

	FontFamilyCatalogObservations observations;
	observations.faces = {face}; // size 1
	FontFamilySeedObservation seed;
	seed.seed_family_name = "OutOfRange";
	for (auto& probe : seed.rbiz) {
		probe.success = true;
		probe.matches_requested_family = true;
		probe.face_ref = 5; // out of range
		probe.outcome.entity_token = 1;
	}
	observations.seeds = {std::move(seed)};
	observations.complete = true;

	auto const coverage = AssessObservationCoverage(observations);
	EXPECT_FALSE(coverage.strong);
	EXPECT_EQ(4u, coverage.successful_probes);
	EXPECT_EQ(0u, coverage.covered_probes);
	EXPECT_EQ(4u, coverage.invalid_face_refs);
	EXPECT_FALSE(coverage.reason.empty());
}

TEST(font_family_obs_repository_win, coverage_weak_when_face_identity_unknown) {
	FontFamilyFaceIdentity face; // default: volume_serial=0 → !IsKnown()
	FontFamilyCatalogObservations observations;
	observations.faces = {face};
	FontFamilySeedObservation seed;
	seed.seed_family_name = "UnknownFace";
	for (auto& probe : seed.rbiz) {
		probe.success = true;
		probe.matches_requested_family = true;
		probe.face_ref = 0;
		probe.outcome.entity_token = 1;
	}
	observations.seeds = {std::move(seed)};
	observations.complete = true;

	auto const coverage = AssessObservationCoverage(observations);
	EXPECT_FALSE(coverage.strong);
	EXPECT_EQ(4u, coverage.successful_probes);
	EXPECT_EQ(0u, coverage.covered_probes);
	EXPECT_EQ(4u, coverage.unknown_faces);
	EXPECT_FALSE(coverage.reason.empty());
}

TEST(font_family_obs_repository_win, empty_faces_with_persisted_tokens_forces_rebuild) {
	// faces.empty() historically short-circuited to weak_m1 before coverage.
	// Loaded observations with success probes carrying nonzero process tokens
	// but no durable identity must force a rebuild, not a weak hit (N2a / T1-3).
	auto const root = (std::filesystem::current_path() /
		"build-dir" / "font-family-obs-empty-faces-coverage").lexically_normal();
	agi::fs::CreateDirectory(root);
	ScopedLocalPath scoped(root);
	auto const cache_path = DefaultWindowsFontFamilyObsCachePath();
	ASSERT_FALSE(cache_path.empty());
	try {
		agi::fs::Remove(cache_path);
	}
	catch (...) {
	}

	GdiFontResolver manifest_resolver;
	auto manifest = CollectWindowsFontFamilyManifest(manifest_resolver);
	ASSERT_FALSE(manifest.gdi_family_names.empty());

	FontFamilyObsStorePayload payload;
	payload.manifest = manifest;
	payload.observation_contract_version = kFontFamilyObsObservationContractVersion;
	payload.derivation_contract_version = kFontFamilyObsDerivationContractVersion;
	payload.created_utc_unix = 1'700'000'000;
	// faces.empty(), but every probe has a nonzero persisted process token.
	FontFamilySeedObservation seed;
	seed.seed_family_name = manifest.gdi_family_names.front();
	seed.profile_matches_requested_family = true;
	for (auto& probe : seed.rbiz) {
		probe.success = true;
		probe.matches_requested_family = true;
		probe.face_ref = kFontFamilyInvalidFaceRef;
		probe.outcome.entity_token = 0xDEADBEEFull;
	}
	payload.observations.seeds = {std::move(seed)};
	payload.observations.complete = true;
	// Seed name matches the live M0 family set so load reaches coverage (not
	// MissM0). Coverage is weak, so repository must rebuild rather than hit.
	ASSERT_EQ(
		FontFamilyObsStoreStatus::Ok, SaveFontFamilyObsStoreAtomic(cache_path, payload));

	// Precondition: coverage of the crafted snapshot is weak.
	EXPECT_FALSE(AssessObservationCoverage(payload.observations).strong);

	GdiFontResolver resolver;
	auto const result = LoadOrRebuildWindowsFontFamilyCatalog(
		resolver, "en-US", [] { return false; }, cache_path);
	// Final hit kind is Rebuilt (rebuild overwrites MissCoverage), not Hit/weak_m1.
	EXPECT_EQ(FontFamilyObsHitKind::Rebuilt, result.hit)
		<< FontFamilyObsHitKindName(result.hit);
	EXPECT_GT(result.physical_probe_count, 0u);
	EXPECT_FALSE(result.weak_m1);
}

TEST(font_family_obs_repository_win, sanitize_loaded_transient_tokens_clears_all_token_fields) {
	// Locks SanitizeLoadedTransientTokens itself: every process-token field on
	// seeds/aliases (outcomes, informational names, win32 names) must become 0.
	// Does not exercise the LoadOrRebuild call site; that is a separate contract.
	FontFamilyCatalogObservations observations;
	observations.complete = true;

	FontFamilySeedObservation seed;
	seed.seed_family_name = "SanitizeSeed";
	seed.win32_family_names.push_back(
		{"SanitizeSeed", "en-US", FontFamilyNameKind::Win32Family, 0xABCDull,
			FontVariantRole::Regular});
	for (std::size_t i = 0; i < 4; ++i) {
		seed.rbiz[i].success = true;
		seed.rbiz[i].outcome.entity_token = 1000 + i;
		seed.rbiz[i].informational_names.push_back({
			"Full", "en-US", FontFamilyNameKind::FullName, 2000 + i,
			FontVariantRole::Regular});
	}
	observations.seeds = {std::move(seed)};

	FontFamilyAliasObservation alias;
	alias.candidate_name = "SanitizeSeed";
	for (std::size_t i = 0; i < 4; ++i) {
		alias.rbiz[i].success = true;
		alias.rbiz[i].outcome.entity_token = 3000 + i;
		alias.rbiz[i].informational_names.push_back({
			"AliasFull", "en-US", FontFamilyNameKind::FullName, 4000 + i,
			FontVariantRole::Regular});
	}
	observations.aliases = {std::move(alias)};

	SanitizeLoadedTransientTokens(observations);

	for (auto const& s : observations.seeds) {
		for (auto const& probe : s.rbiz) {
			EXPECT_EQ(0u, probe.outcome.entity_token);
			for (auto const& name : probe.informational_names)
				EXPECT_EQ(0u, name.entity_token);
		}
		for (auto const& name : s.win32_family_names)
			EXPECT_EQ(0u, name.entity_token);
	}
	for (auto const& a : observations.aliases) {
		for (auto const& probe : a.rbiz) {
			EXPECT_EQ(0u, probe.outcome.entity_token);
			for (auto const& name : probe.informational_names)
				EXPECT_EQ(0u, name.entity_token);
		}
	}
}

TEST(font_family_obs_repository_win, derive_groups_by_face_ref_when_tokens_sanitized) {
	// Derive-layer contract: after process tokens are cleared, two seeds that
	// share FaceIdentity + realized style still merge via DurableVariantKey.
	// This is not a LoadOrRebuild integration test (name is intentional).
	FontFamilyFaceIdentity face;
	face.volume_serial = 0xAABBCCDDu;
	face.file_index = 0x1122334455667788ull;
	face.face_index = 0;
	face.size = 4096;
	face.mtime_utc_100ns = 1000;

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
				process_token + static_cast<std::uint64_t>(i)};
			s.rbiz[i].informational_names.push_back({
				s.seed_family_name + " Full", "en-US", FontFamilyNameKind::FullName,
				process_token + 99, FontVariantRole::Regular});
		}
		return s;
	};

	FontFamilyCatalogObservations observations;
	observations.complete = true;
	observations.faces = {face};
	observations.seeds = {make_seed("A", 1000), make_seed("B", 9000)};
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
				process_token + static_cast<std::uint64_t>(i)};
		}
		return a;
	};
	observations.aliases = {make_alias("A", 1000), make_alias("B", 9000)};

	// Use the real sanitize API (not a hand-rolled zero loop) before Derive.
	SanitizeLoadedTransientTokens(observations);
	EXPECT_TRUE(AssessObservationCoverage(observations).strong);

	auto const catalog = DeriveWindowsFontFamilyCatalog(observations, "en-US");
	ASSERT_EQ(1u, catalog.size());
	EXPECT_EQ(FontFamilyMatchKind::Exact, catalog.Resolve("B").match);
	// Snapshot tokens come from DurableVariantKey, not the cleared process values.
	EXPECT_NE(0u, catalog.records().front().variant_profile.outcomes[0].entity_token);
}

TEST(font_family_obs_repository_win, coverage_ignores_failed_probes) {
	FontFamilyCatalogObservations observations;
	FontFamilySeedObservation seed;
	seed.seed_family_name = "Failed";
	for (auto& probe : seed.rbiz) {
		probe.success = false;
		probe.face_ref = kFontFamilyInvalidFaceRef;
		probe.outcome.entity_token = 0;
	}
	observations.seeds = {std::move(seed)};
	observations.complete = true;

	auto const coverage = AssessObservationCoverage(observations);
	EXPECT_TRUE(coverage.strong);
	EXPECT_EQ(0u, coverage.successful_probes);
	EXPECT_EQ(0u, coverage.covered_probes);
}

TEST(font_family_obs_repository_win, rebuild_lock_timeout_returns_miss_without_observe) {
	// Hold the rebuild mutex on another thread (same-thread Wait is recursive on
	// Windows and would not exercise timeout). Force a 1ms wait and prove
	// MissLockTimeout without starting Observe.
	auto const root = (std::filesystem::current_path() /
		"build-dir" / "font-family-obs-lock-timeout").lexically_normal();
	agi::fs::CreateDirectory(root);
	ScopedLocalPath scoped(root);
	auto const cache_path = DefaultWindowsFontFamilyObsCachePath();
	ASSERT_FALSE(cache_path.empty());
	try {
		agi::fs::Remove(cache_path);
	}
	catch (...) {
	}

	auto const lock_name = FontFamilyObsRebuildLockNameForPath(cache_path);
	std::atomic<bool> holder_ready{false};
	std::atomic<bool> release_holder{false};
	std::thread holder([&] {
		HANDLE mutex = CreateMutexW(nullptr, FALSE, lock_name.c_str());
		if (!mutex)
			return;
		if (WaitForSingleObject(mutex, 5000) != WAIT_OBJECT_0) {
			CloseHandle(mutex);
			return;
		}
		holder_ready.store(true);
		while (!release_holder.load())
			Sleep(1);
		ReleaseMutex(mutex);
		CloseHandle(mutex);
	});

	for (int i = 0; i < 5000 && !holder_ready.load(); ++i)
		Sleep(1);
	ASSERT_TRUE(holder_ready.load()) << "background thread failed to own rebuild lock";

	struct RestoreWait {
		~RestoreWait() { SetFontFamilyObsRebuildLockWaitMsForTest(std::nullopt); }
	} restore_wait;
	SetFontFamilyObsRebuildLockWaitMsForTest(1);

	GdiFontResolver resolver;
	auto const probes_before = resolver.stats().physical_probe_count;
	auto const result = LoadOrRebuildWindowsFontFamilyCatalog(
		resolver, "en-US", [] { return false; }, cache_path);

	EXPECT_EQ(FontFamilyObsHitKind::MissLockTimeout, result.hit)
		<< FontFamilyObsHitKindName(result.hit);
	EXPECT_TRUE(result.catalog.empty());
	EXPECT_EQ(0u, result.face_count);
	// Contract: timeout must not start a concurrent full Observe.
	EXPECT_EQ(probes_before, result.physical_probe_count);
	EXPECT_EQ(probes_before, resolver.stats().physical_probe_count);

	release_holder.store(true);
	holder.join();
}
