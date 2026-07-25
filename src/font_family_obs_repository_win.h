// Windows observation repository: strict M0/M1 hit or full Observe + atomic save.
#pragma once

#ifndef _WIN32
#error "font_family_obs_repository_win.h is Windows-only"
#endif

#include "font_family_catalog.h"
#include "font_family_obs_face_stamp_win.h"
#include "font_family_obs_store.h"
#include "font_family_obs_types.h"

#include <libaegisub/fs_fwd.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class GdiFontResolver;

enum class FontFamilyObsHitKind : std::uint8_t {
	MissNoPath = 0,
	MissLoad,
	MissContract,
	MissM0,
	MissM1,
	MissIncomplete,
	MissCoverage,
	/// Cross-process rebuild lock timed out and no trusted disk hit was available.
	MissLockTimeout,
	Hit,
	Rebuilt,
	RebuildAborted,
};

struct FontFamilyObsRepositoryResult {
	FontFamilyCatalog catalog;
	FontFamilyObsHitKind hit = FontFamilyObsHitKind::MissNoPath;
	FontFamilyObsStoreStatus store_status = FontFamilyObsStoreStatus::NotFound;
	std::uint64_t physical_probe_count = 0;
	std::size_t face_count = 0;
	/// True when a hit was admitted with an empty FaceIdentity table. M1 cannot
	/// prove file stamps in that case; trust is M0-only (see design B1/M1).
	bool weak_m1 = false;
	bool wrote_cache = false;
	/// Coverage of the observations used for this result (live or loaded).
	ObservationCoverage coverage;
};

char const* FontFamilyObsHitKindName(FontFamilyObsHitKind kind) noexcept;

/// Pure coverage assessment. Does not trust any persisted completeness flag
/// beyond observations.complete being out of scope for this predicate.
ObservationCoverage AssessObservationCoverage(
	FontFamilyCatalogObservations const& observations);

/// Clear serialized process tokens on a trusted loaded snapshot before Derive.
/// Called only after coverage + M1 on the hit path. Exposed for unit tests so
/// the isolation contract (no LiveVariantKey from disk tokens) stays locked.
/// Does not change face_ref or durable FaceIdentity facts.
void SanitizeLoadedTransientTokens(FontFamilyCatalogObservations& observations);

/// Collect M0 fingerprints and sorted GDI family names. Uses enumeration only;
/// it must not issue physical face probes.
/// When `gdi_family_names` is non-null, those names are sorted/unique'd into the
/// manifest instead of calling EnumerateFamilies again (share one full GDI enum
/// with Observe rebuild seeds).
FontFamilyInputManifest CollectWindowsFontFamilyManifest(
	GdiFontResolver& resolver,
	std::vector<std::string> const* gdi_family_names = nullptr);

// M1 face stamps: see font_family_obs_face_stamp_win.h (Validate/Classify).

/// Default on-disk path under `?local`. Empty when config::path is unavailable.
agi::fs::path DefaultWindowsFontFamilyObsCachePath();

/// Load a trusted snapshot (M0+M1) or fully re-observe, then Derive. Never
/// publishes partial observations or writes on shutdown abort.
///
/// When `cache_path` is non-empty, a per-path named mutex serializes rebuilds
/// across processes (GUI + fontcollector). Wait is bounded (~3s); on timeout the
/// function re-tries a disk hit once and otherwise returns MissLockTimeout with
/// an empty catalog — it never starts a second concurrent full Observe.
FontFamilyObsRepositoryResult LoadOrRebuildWindowsFontFamilyCatalog(
	GdiFontResolver& resolver,
	std::string_view locale,
	std::function<bool()> const& shutdown_requested,
	agi::fs::path const& cache_path);

/// Named mutex leaf used for `cache_path` (for tests that hold the lock).
std::wstring FontFamilyObsRebuildLockNameForPath(agi::fs::path const& cache_path);

/// Test seam: override rebuild-lock wait in milliseconds. `nullopt` restores the
/// production default (3000). Pass 0/1 with an externally held lock to exercise
/// MissLockTimeout without sleeping three seconds.
void SetFontFamilyObsRebuildLockWaitMsForTest(std::optional<std::uint32_t> wait_ms);
