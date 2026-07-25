#pragma once

#ifndef _WIN32
#error "font_family_observe_win.h is Windows-only"
#endif

#include "font_family_obs_types.h"

#include <functional>
#include <vector>

class GdiFontResolver;

/// Options for a single ObserveWindowsFontFamilies call.
struct ObserveWindowsFontFamiliesOptions {
	/// 0 = auto: min(8, hardware_concurrency, seed count). Cap 8 limits concurrent
	/// GDI/DWrite load; inject 1 vs N in tests for serial/parallel equivalence.
	unsigned worker_count = 0;
	/// When non-null, use these seeds (order preserved) instead of enumerating.
	/// Empty vector yields an empty complete observation.
	std::vector<std::string> const* seeds = nullptr;
	/// When non-null and complete, attempt incremental Observe: reuse seeds whose
	/// FaceIdentity stamps still match live files; only probe added/dirty seeds.
	/// Large dirty sets fall back to a full Observe automatically.
	/// Callers must pass a durable previous (strong coverage; typically
	/// disk-loaded after contract/fingerprint checks). Weak or incomplete
	/// bodies are ignored and a full Observe runs instead.
	FontFamilyCatalogObservations const* previous = nullptr;
};

/// Live observation plus process-local probe accounting (not part of the store).
struct ObserveWindowsFontFamiliesResult {
	FontFamilyCatalogObservations observations;
	/// Sum of physical probes across seed workers and the sequential alias pass.
	std::uint64_t physical_probe_count = 0;
	/// True when at least one previous seed was reused (not a full cold Observe).
	bool used_incremental = false;
	/// Seeds that required a fresh RBIZ probe (added or dirty). Full mode: all.
	std::uint32_t probed_seed_count = 0;
	/// Seeds copied from previous with remapped face_ref (incremental only).
	std::uint32_t reused_seed_count = 0;
};

/// Collect all live GDI/DirectWrite facts used by Windows catalog derivation.
/// shutdown_requested is polled between bounded probes; an interrupted result
/// has complete == false and contains no publishable partial catalog.
ObserveWindowsFontFamiliesResult ObserveWindowsFontFamilies(
	GdiFontResolver& resolver,
	std::function<bool()> const& shutdown_requested,
	ObserveWindowsFontFamiliesOptions options = {});
