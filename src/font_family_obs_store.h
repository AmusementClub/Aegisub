// Binary observation snapshot: header/CRC codec and atomic whole-file replace.
// Pure serialization — no GDI, no manifest policy, no Derive.
#pragma once

#include "font_family_obs_types.h"

#include <libaegisub/fs_fwd.h>

#include <cstdint>
#include <string>
#include <string_view>

enum class FontFamilyObsStoreStatus : std::uint8_t {
	Ok = 0,
	NotFound,
	IoError,
	BadMagic,
	BadSchema,
	BadCrc,
	BadCounts,
	TooLarge,
	Incomplete,
	Truncated,
	ContractMismatch,
	RefuseWrite,
};

struct FontFamilyObsStorePayload {
	FontFamilyInputManifest manifest;
	FontFamilyCatalogObservations observations;
	std::uint32_t observation_contract_version =
		kFontFamilyObsObservationContractVersion;
	std::uint32_t derivation_contract_version =
		kFontFamilyObsDerivationContractVersion;
	std::uint64_t created_utc_unix = 0;
	/// Diagnostic only; never participates in invalidation.
	std::uint64_t app_build_id = 0;
};

char const* FontFamilyObsStoreStatusName(FontFamilyObsStoreStatus status) noexcept;

/// Decode a complete payload from bytes. Any structural failure returns a miss
/// status without throwing and without partially-trusted observations.
FontFamilyObsStoreStatus DecodeFontFamilyObsStore(
	std::string_view bytes,
	FontFamilyObsStorePayload& out);

/// Encode a complete payload. Incomplete observations are refused.
FontFamilyObsStoreStatus EncodeFontFamilyObsStore(
	FontFamilyObsStorePayload const& payload,
	std::string& out_bytes);

/// Load from disk. Missing files return NotFound; corruption returns a miss.
FontFamilyObsStoreStatus TryLoadFontFamilyObsStore(
	agi::fs::path const& path,
	FontFamilyObsStorePayload& out);

/// Atomically replace the formal cache file (tmp → flush → rename). Incomplete
/// payloads and oversize encodings refuse to write so a prior good file remains.
FontFamilyObsStoreStatus SaveFontFamilyObsStoreAtomic(
	agi::fs::path const& path,
	FontFamilyObsStorePayload const& payload);
