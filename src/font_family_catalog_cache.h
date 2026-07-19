// Process-wide immutable FontFamilyCatalog snapshots.
// Built asynchronously in the background; GetSnapshot waits only if a
// build is already in flight or must start one. Invalidate drops the
// published snapshot so the next use rebuilds.

#pragma once

#include "font_family_catalog.h"

#include <memory>

namespace font_family_catalog_cache {

/// Return a shared immutable snapshot.
/// If no snapshot is ready, starts a background build (if needed) and waits
/// for it. Prefer WarmAsync() so interactive paths usually hit a warm cache.
std::shared_ptr<FontFamilyCatalog const> GetSnapshot();

/// Start building a snapshot on a background thread if none is ready.
/// Safe to call from the GUI thread; does not block on the build.
void WarmAsync();

/// Drop the current snapshot and cancel installation of any in-flight build.
/// Non-blocking: does not wait for a running builder to finish. Unfinished
/// async tasks are retained until complete so their futures are never
/// destroyed under the cache mutex (avoids MSVC async future deadlocks).
void Invalidate();

/// Synchronously rebuild and publish a new snapshot. Returns the new snapshot.
std::shared_ptr<FontFamilyCatalog const> Rebuild();

/// Stop accepting new builds and wait for all background builds to finish.
/// This is a terminal, process-shutdown operation. GetSnapshot and Rebuild
/// throw after shutdown; WarmAsync and Invalidate become no-ops.
void Shutdown();

} // namespace font_family_catalog_cache
