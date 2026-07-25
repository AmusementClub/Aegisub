#include <gtest/gtest.h>

#include "../../src/font_family_obs_store.h"

#include <libaegisub/fs.h>

#include <cstring>
#include <string>
#include <vector>

namespace {

FontFamilyProbeObservation make_probe(std::uint64_t token, std::uint32_t face_ref) {
	FontFamilyProbeObservation probe;
	probe.success = true;
	probe.matches_requested_family = true;
	probe.face_ref = face_ref;
	probe.outcome = {
		400, false, 400, false, FontVariantRole::Regular, FontVariantStatus::Canonical,
		token};
	probe.informational_names.push_back(
		{"Full", "en-US", FontFamilyNameKind::FullName, token, FontVariantRole::Regular});
	return probe;
}

FontFamilyObsStorePayload sample_payload() {
	FontFamilyObsStorePayload payload;
	payload.created_utc_unix = 1'700'000'000;
	payload.app_build_id = 42;
	payload.manifest.provider_fingerprint = 0x1111;
	payload.manifest.os_build_fingerprint = 0x2222;
	payload.manifest.font_registry_fingerprint = 0x3333;
	payload.manifest.gdi_family_names = {"Arial", "Segoe UI"};

	FontFamilyFaceIdentity face;
	face.volume_serial = 0xABCDEF01u;
	face.file_index = 0x1020304050607080ull;
	face.face_index = 0;
	face.size = 12345;
	face.mtime_utc_100ns = 0x200000000ull;
	payload.observations.faces.push_back(face);

	FontFamilySeedObservation seed;
	seed.seed_family_name = "Arial";
	seed.profile_matches_requested_family = true;
	seed.rbiz = {
		make_probe(10, 0), make_probe(11, 0), make_probe(12, 0), make_probe(13, 0)};
	seed.win32_family_names.push_back(
		{"Arial", "en-US", FontFamilyNameKind::Win32Family});
	payload.observations.seeds.push_back(std::move(seed));

	FontFamilyAliasObservation alias;
	alias.candidate_name = "Arial";
	alias.rbiz = {
		make_probe(10, kFontFamilyInvalidFaceRef),
		make_probe(11, kFontFamilyInvalidFaceRef),
		make_probe(12, kFontFamilyInvalidFaceRef),
		make_probe(13, kFontFamilyInvalidFaceRef)};
	payload.observations.aliases.push_back(std::move(alias));
	payload.observations.complete = true;
	return payload;
}

void ExpectSameName(FontFamilyName const& left, FontFamilyName const& right) {
	EXPECT_EQ(left.value, right.value);
	EXPECT_EQ(left.locale, right.locale);
	EXPECT_EQ(left.kind, right.kind);
	EXPECT_EQ(left.entity_token, right.entity_token);
	EXPECT_EQ(left.variant_role, right.variant_role);
}

void ExpectSameProbe(
	FontFamilyProbeObservation const& left,
	FontFamilyProbeObservation const& right) {
	EXPECT_EQ(left.success, right.success);
	EXPECT_EQ(left.matches_requested_family, right.matches_requested_family);
	EXPECT_EQ(left.face_ref, right.face_ref);
	EXPECT_EQ(left.outcome.requested_weight, right.outcome.requested_weight);
	EXPECT_EQ(left.outcome.requested_italic, right.outcome.requested_italic);
	EXPECT_EQ(left.outcome.realized_weight, right.outcome.realized_weight);
	EXPECT_EQ(left.outcome.realized_italic, right.outcome.realized_italic);
	EXPECT_EQ(left.outcome.role, right.outcome.role);
	EXPECT_EQ(left.outcome.status, right.outcome.status);
	EXPECT_EQ(left.outcome.entity_token, right.outcome.entity_token);
	ASSERT_EQ(left.informational_names.size(), right.informational_names.size());
	for (std::size_t i = 0; i < left.informational_names.size(); ++i)
		ExpectSameName(left.informational_names[i], right.informational_names[i]);
}

void ExpectSameObservations(
	FontFamilyCatalogObservations const& left,
	FontFamilyCatalogObservations const& right) {
	EXPECT_EQ(left.complete, right.complete);
	ASSERT_EQ(left.faces.size(), right.faces.size());
	for (std::size_t i = 0; i < left.faces.size(); ++i)
		EXPECT_EQ(left.faces[i], right.faces[i]);
	ASSERT_EQ(left.seeds.size(), right.seeds.size());
	for (std::size_t i = 0; i < left.seeds.size(); ++i) {
		EXPECT_EQ(left.seeds[i].seed_family_name, right.seeds[i].seed_family_name);
		EXPECT_EQ(
			left.seeds[i].profile_matches_requested_family,
			right.seeds[i].profile_matches_requested_family);
		ASSERT_EQ(left.seeds[i].rbiz.size(), right.seeds[i].rbiz.size());
		for (std::size_t p = 0; p < left.seeds[i].rbiz.size(); ++p)
			ExpectSameProbe(left.seeds[i].rbiz[p], right.seeds[i].rbiz[p]);
		ASSERT_EQ(
			left.seeds[i].win32_family_names.size(),
			right.seeds[i].win32_family_names.size());
		for (std::size_t n = 0; n < left.seeds[i].win32_family_names.size(); ++n)
			ExpectSameName(
				left.seeds[i].win32_family_names[n],
				right.seeds[i].win32_family_names[n]);
	}
	ASSERT_EQ(left.aliases.size(), right.aliases.size());
	for (std::size_t i = 0; i < left.aliases.size(); ++i) {
		EXPECT_EQ(left.aliases[i].candidate_name, right.aliases[i].candidate_name);
		ASSERT_EQ(left.aliases[i].rbiz.size(), right.aliases[i].rbiz.size());
		for (std::size_t p = 0; p < left.aliases[i].rbiz.size(); ++p)
			ExpectSameProbe(left.aliases[i].rbiz[p], right.aliases[i].rbiz[p]);
	}
}

void WriteLe32(std::string& bytes, std::size_t offset, std::uint32_t value) {
	ASSERT_GE(bytes.size(), offset + 4u);
	bytes[offset + 0] = static_cast<char>(value & 0xFFu);
	bytes[offset + 1] = static_cast<char>((value >> 8) & 0xFFu);
	bytes[offset + 2] = static_cast<char>((value >> 16) & 0xFFu);
	bytes[offset + 3] = static_cast<char>((value >> 24) & 0xFFu);
}

std::uint32_t ReadLe32(std::string const& bytes, std::size_t offset) {
	return static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset])) |
		(static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 1])) << 8) |
		(static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 2])) << 16) |
		(static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 3])) << 24);
}

// Header layout: magic..flags (24) + created/app (16) = 40;
// face/seed/alias/string_count/string_pool_bytes at 40,44,48,52,56.
constexpr std::size_t kHdrStringCount = 52;
constexpr std::size_t kHdrStringPoolBytes = 56;
constexpr std::size_t kHdrStorageSchema = 4;

} // namespace

TEST(font_family_obs_store, round_trip_preserves_observations) {
	auto const original = sample_payload();
	std::string bytes;
	ASSERT_EQ(FontFamilyObsStoreStatus::Ok, EncodeFontFamilyObsStore(original, bytes));
	ASSERT_FALSE(bytes.empty());

	FontFamilyObsStorePayload decoded;
	ASSERT_EQ(FontFamilyObsStoreStatus::Ok, DecodeFontFamilyObsStore(bytes, decoded));
	EXPECT_EQ(original.created_utc_unix, decoded.created_utc_unix);
	EXPECT_EQ(original.app_build_id, decoded.app_build_id);
	EXPECT_EQ(original.observation_contract_version, decoded.observation_contract_version);
	EXPECT_EQ(original.derivation_contract_version, decoded.derivation_contract_version);
	EXPECT_EQ(original.manifest.provider_fingerprint, decoded.manifest.provider_fingerprint);
	EXPECT_EQ(original.manifest.os_build_fingerprint, decoded.manifest.os_build_fingerprint);
	EXPECT_EQ(
		original.manifest.font_registry_fingerprint,
		decoded.manifest.font_registry_fingerprint);
	ASSERT_EQ(original.manifest.gdi_family_names, decoded.manifest.gdi_family_names);
	ExpectSameObservations(original.observations, decoded.observations);
}

TEST(font_family_obs_store, refuses_incomplete_write) {
	auto payload = sample_payload();
	payload.observations.complete = false;
	std::string bytes;
	EXPECT_EQ(FontFamilyObsStoreStatus::Incomplete, EncodeFontFamilyObsStore(payload, bytes));
	EXPECT_TRUE(bytes.empty());
}

TEST(font_family_obs_store, crc_mismatch_is_a_miss) {
	auto const original = sample_payload();
	std::string bytes;
	ASSERT_EQ(FontFamilyObsStoreStatus::Ok, EncodeFontFamilyObsStore(original, bytes));
	ASSERT_GT(bytes.size(), 72u);
	bytes.back() = static_cast<char>(static_cast<unsigned char>(bytes.back()) ^ 0xFFu);

	FontFamilyObsStorePayload decoded;
	EXPECT_EQ(FontFamilyObsStoreStatus::BadCrc, DecodeFontFamilyObsStore(bytes, decoded));
	EXPECT_TRUE(decoded.observations.seeds.empty());
}

TEST(font_family_obs_store, bad_magic_is_a_miss) {
	auto const original = sample_payload();
	std::string bytes;
	ASSERT_EQ(FontFamilyObsStoreStatus::Ok, EncodeFontFamilyObsStore(original, bytes));
	bytes[0] = 'X';
	FontFamilyObsStorePayload decoded;
	EXPECT_EQ(FontFamilyObsStoreStatus::BadMagic, DecodeFontFamilyObsStore(bytes, decoded));
}

TEST(font_family_obs_store, truncated_payload_is_a_miss) {
	auto const original = sample_payload();
	std::string bytes;
	ASSERT_EQ(FontFamilyObsStoreStatus::Ok, EncodeFontFamilyObsStore(original, bytes));
	bytes.resize(bytes.size() / 2);
	FontFamilyObsStorePayload decoded;
	auto const status = DecodeFontFamilyObsStore(bytes, decoded);
	EXPECT_TRUE(
		status == FontFamilyObsStoreStatus::Truncated ||
		status == FontFamilyObsStoreStatus::BadCrc);
}

TEST(font_family_obs_store, atomic_save_and_load_round_trip) {
	auto dir = agi::fs::PathFromString("build-dir/font-family-obs-store-test");
	agi::fs::CreateDirectory(dir);
	auto path = dir / "windows-font-observations.afco";
	auto const original = sample_payload();
	ASSERT_EQ(
		FontFamilyObsStoreStatus::Ok, SaveFontFamilyObsStoreAtomic(path, original));

	FontFamilyObsStorePayload loaded;
	ASSERT_EQ(FontFamilyObsStoreStatus::Ok, TryLoadFontFamilyObsStore(path, loaded));
	ExpectSameObservations(original.observations, loaded.observations);

	try {
		agi::fs::Remove(path);
	}
	catch (...) {
	}
}

TEST(font_family_obs_store, storage_schema_version_mismatch_is_bad_schema) {
	auto const original = sample_payload();
	std::string bytes;
	ASSERT_EQ(FontFamilyObsStoreStatus::Ok, EncodeFontFamilyObsStore(original, bytes));
	WriteLe32(bytes, kHdrStorageSchema, kFontFamilyObsStorageSchemaVersion + 1);
	FontFamilyObsStorePayload decoded;
	EXPECT_EQ(FontFamilyObsStoreStatus::BadSchema, DecodeFontFamilyObsStore(bytes, decoded));
}

TEST(font_family_obs_store, huge_string_count_is_too_large_without_giant_alloc) {
	auto const original = sample_payload();
	std::string bytes;
	ASSERT_EQ(FontFamilyObsStoreStatus::Ok, EncodeFontFamilyObsStore(original, bytes));
	// Hostile header: enormous string_count with CRC still matching payload.
	WriteLe32(bytes, kHdrStringCount, 0xFFFFFFFFu);
	FontFamilyObsStorePayload decoded;
	auto const status = DecodeFontFamilyObsStore(bytes, decoded);
	EXPECT_TRUE(
		status == FontFamilyObsStoreStatus::TooLarge ||
		status == FontFamilyObsStoreStatus::BadCounts)
		<< FontFamilyObsStoreStatusName(status);
	EXPECT_TRUE(decoded.observations.seeds.empty());
}

TEST(font_family_obs_store, string_count_inconsistent_with_pool_bytes_is_bad_counts) {
	auto const original = sample_payload();
	std::string bytes;
	ASSERT_EQ(FontFamilyObsStoreStatus::Ok, EncodeFontFamilyObsStore(original, bytes));
	auto const pool_bytes = ReadLe32(bytes, kHdrStringPoolBytes);
	// Claim more strings than 4-byte prefixes could fit in the pool region.
	auto const too_many = pool_bytes / 4u + 1u;
	if (too_many <= ReadLe32(bytes, kHdrStringCount))
		GTEST_SKIP() << "sample pool too small to craft inconsistent count";
	WriteLe32(bytes, kHdrStringCount, too_many);
	FontFamilyObsStorePayload decoded;
	EXPECT_EQ(FontFamilyObsStoreStatus::BadCounts, DecodeFontFamilyObsStore(bytes, decoded));
}

TEST(font_family_obs_store, header_string_count_tamper_caught_before_body_use) {
	// Payload CRC unchanged; only structural string_count is hostile.
	auto const original = sample_payload();
	std::string bytes;
	ASSERT_EQ(FontFamilyObsStoreStatus::Ok, EncodeFontFamilyObsStore(original, bytes));
	WriteLe32(bytes, kHdrStringCount, kFontFamilyObsMaxStrings + 1u);
	FontFamilyObsStorePayload decoded;
	EXPECT_EQ(FontFamilyObsStoreStatus::TooLarge, DecodeFontFamilyObsStore(bytes, decoded));
}
