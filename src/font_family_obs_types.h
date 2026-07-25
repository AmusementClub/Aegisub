// In-memory facts collected while building the Windows GDI family catalog.
//
// These are deliberately not FontFamilyRecord objects: records are a derived
// presentation of the complete observation set. Keeping the live-selection
// facts here lets later cache/replay work reuse them without making GDI calls.
#pragma once

#include "font_family_catalog.h"

#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <variant>
#include <vector>

/// Binary layout version. Incompatible changes discard the on-disk snapshot.
inline constexpr std::uint32_t kFontFamilyObsStorageSchemaVersion = 1;
/// Shape of collected probe/alias facts. Changes force a full re-observe.
/// v2: alias probes may carry FaceIdentity (face_ref) like seeds.
inline constexpr std::uint32_t kFontFamilyObsObservationContractVersion = 2;
/// Pure Derive rules. May change without re-observing when the store matches.
/// v2: snapshot tokens come from PhysicalVariantKey interning, not raw process
/// tokens. storage_schema_version is unchanged; token fields remain diagnostic.
inline constexpr std::uint32_t kFontFamilyObsDerivationContractVersion = 2;
/// Manifest field set and comparison rules.
inline constexpr std::uint32_t kFontFamilyObsManifestContractVersion = 1;
/// Little-endian magic "AFCO".
inline constexpr std::uint32_t kFontFamilyObsMagic = 0x4F434641u;

/// Hard limits applied on read and write. Anything above is a miss / refuse.
inline constexpr std::uint32_t kFontFamilyObsMaxSeeds = 100000;
inline constexpr std::uint32_t kFontFamilyObsMaxFaces = 100000;
inline constexpr std::uint32_t kFontFamilyObsMaxAliases = 200000;
inline constexpr std::uint32_t kFontFamilyObsMaxNamesPerList = 4096;
inline constexpr std::uint32_t kFontFamilyObsMaxStringPoolBytes = 64u * 1024u * 1024u;
/// Cap on string_count before pool.reserve. Each string costs at least a 4-byte
/// length prefix, so this is kFontFamilyObsMaxStringPoolBytes / 4.
inline constexpr std::uint32_t kFontFamilyObsMaxStrings =
	kFontFamilyObsMaxStringPoolBytes / 4u;
inline constexpr std::uint64_t kFontFamilyObsMaxFileBytes = 128ull * 1024u * 1024u;

/// Durable, path-free identity used by the Phase 2 observation store. A zero
/// volume/file pair is unknown and must never be accepted as a reusable face.
struct FontFamilyFaceIdentity {
	std::uint32_t volume_serial = 0;
	std::uint64_t file_index = 0;
	std::int32_t face_index = -1;
	std::uint64_t size = 0;
	std::uint64_t mtime_utc_100ns = 0;
	std::uint64_t usn = 0;
	std::uint64_t content_hash = 0;
	std::uint32_t flags = 0;

	bool operator==(FontFamilyFaceIdentity const&) const = default;

	bool IsKnown() const noexcept {
		return volume_serial != 0 && file_index != 0;
	}
};

inline constexpr std::uint32_t kFontFamilyInvalidFaceRef =
	std::numeric_limits<std::uint32_t>::max();

struct FontFamilyProbeObservation {
	FontVariantOutcome outcome;
	bool matches_requested_family = false;
	bool success = false;
	std::uint32_t face_ref = kFontFamilyInvalidFaceRef;
	/// Full/PostScript metadata read from this exact selected physical face.
	std::vector<FontFamilyName> informational_names;
};

/// One GDI-enumerated family and its four canonical Regular/Bold/Italic/
/// Bold-Italic selections. Win32 family names are read only from the Regular
/// selected face, matching the pre-observation catalog builder semantics.
struct FontFamilySeedObservation {
	std::string seed_family_name;
	std::array<FontFamilyProbeObservation, 4> rbiz;
	bool profile_matches_requested_family = false;
	std::vector<FontFamilyName> win32_family_names;
};

/// A four-way observation of a candidate Win32 alias. This is the recorded
/// replacement for the former Derive-time live maps_to_profile probe.
struct FontFamilyAliasObservation {
	std::string candidate_name;
	std::array<FontFamilyProbeObservation, 4> rbiz;
};

struct FontFamilyCatalogObservations {
	std::vector<FontFamilyFaceIdentity> faces;
	std::vector<FontFamilySeedObservation> seeds;
	std::vector<FontFamilyAliasObservation> aliases;
	/// False means enumeration was interrupted (for example during shutdown),
	/// and callers must not publish or persist a partial snapshot.
	bool complete = false;
};

/// M0 discovery facts. Matching M0 alone never proves trust; M1 face stamps do.
struct FontFamilyInputManifest {
	std::uint32_t manifest_contract_version = kFontFamilyObsManifestContractVersion;
	std::uint64_t provider_fingerprint = 0;
	std::uint64_t os_build_fingerprint = 0;
	std::uint64_t substitute_registry_fingerprint = 0;
	/// Sorted unique GDI-enumerated family names captured with the snapshot.
	std::vector<std::string> gdi_family_names;
};

/// Durable physical variant identity for Derive interning. requested_weight /
/// requested_italic are intentionally excluded: Bold-only fonts may map both
/// Regular and Bold requests onto the same face and must share a token.
struct DurableVariantKey {
	FontFamilyFaceIdentity identity;
	int realized_weight = 0;
	bool realized_italic = false;

	bool operator==(DurableVariantKey const&) const = default;
};

/// Process-local fallback when no FaceIdentity is available. Only valid for
/// cold builds in the current process; never a cross-process trust fact.
struct LiveVariantKey {
	std::uint64_t process_token = 0;

	bool operator==(LiveVariantKey const&) const = default;
};

using PhysicalVariantKey = std::variant<DurableVariantKey, LiveVariantKey>;

/// Coverage of successful probes by durable FaceIdentity. Pure function of an
/// observation snapshot; repository does not trust any persisted flag.
///
/// Successful probes with entity_token == 0 (typical device/bitmap fonts with
/// no path or fingerprint) cannot carry a FaceIdentity and are tracked as
/// no_entity_probes; they do not block strong coverage. Successful probes with
/// a non-zero process token but missing/unknown face_ref do block strong
/// coverage — those had physical evidence that failed durable capture.
struct ObservationCoverage {
	bool strong = false;
	std::size_t successful_probes = 0;
	std::size_t covered_probes = 0;
	/// Successful probes with no process entity and no face_ref (bitmap/device).
	std::size_t no_entity_probes = 0;
	std::size_t missing_face_refs = 0;
	std::size_t invalid_face_refs = 0;
	std::size_t unknown_faces = 0;
	/// First failure reason for logs; empty when strong.
	std::string reason;
};
