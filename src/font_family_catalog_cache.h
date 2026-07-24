// Process-wide immutable FontFamilyCatalog snapshots.
// Built asynchronously in the background; GetSnapshot waits only if a
// build is already in flight or must start one. Invalidate drops the
// published snapshot so the next use rebuilds.

#pragma once

#include "font_family_catalog_source.h"

#include <cstdint>
#include <memory>

namespace font_family_catalog_cache {

/// Replace the source used for subsequent catalog builds.
///
/// Installing a source invalidates the published snapshot and advances the
/// cache generation. Existing in-flight builds may finish, but can never
/// publish into the new generation. The source itself is not tied to a
/// generation; this separation lets callers retain old snapshots safely.
void SetSource(FontFamilyCatalogSourcePtr source);

/// Return metadata for the source selected for the current generation.
/// Returns an Unknown identity after shutdown or when no source is installed.
FontFamilyCatalogSourceInfo GetSourceInfo();

/// Return the cache generation. This is independent of source backend identity
/// and changes whenever Invalidate() or SetSource() drops a snapshot.
std::uint64_t GetGeneration();

/// Return a shared immutable snapshot.
/// If no snapshot is ready, starts a background build (if needed) and waits
/// for it. Prefer WarmAsync() so interactive paths usually hit a warm cache.
std::shared_ptr<FontFamilyCatalog const> GetSnapshot();

/// Waiting best-effort lookup retained for compatibility. Returns null when
/// building fails or the cache has shut down.
std::shared_ptr<FontFamilyCatalog const> TryGetSnapshot() noexcept;

/// Return the currently published snapshot without starting or waiting for a
/// build. Returns null when the current generation is not ready.
std::shared_ptr<FontFamilyCatalog const> GetReadySnapshot() noexcept;

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

/// True after Shutdown() has been requested. Long platform builders should poll
/// this and abort so process exit is not stuck inside GDI/DWrite enumeration.
bool IsShutdownRequested() noexcept;

} // namespace font_family_catalog_cache
