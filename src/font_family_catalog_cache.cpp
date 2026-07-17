#include "font_family_catalog_cache.h"

#include <chrono>
#include <future>
#include <mutex>
#include <utility>
#include <vector>

namespace font_family_catalog_cache {
namespace {

using Snapshot = std::shared_ptr<FontFamilyCatalog const>;
using SnapshotFuture = std::shared_future<Snapshot>;

std::mutex g_mutex;
// Bumped on Invalidate so in-flight builders never publish a stale snapshot.
std::uint64_t g_generation = 0;
Snapshot g_snapshot;
SnapshotFuture g_inflight;
// Retired async futures that must not be destroyed under g_mutex (MSVC STL
// blocks on the last async shared_future destructor). Incomplete futures stay
// here until they are ready so Invalidate remains non-blocking.
std::vector<SnapshotFuture> g_retired;

void ReapRetiredLocked() {
	// Caller holds g_mutex. Only destroy futures that will not block.
	std::vector<SnapshotFuture> ready;
	std::vector<SnapshotFuture> pending;
	ready.reserve(g_retired.size());
	pending.reserve(g_retired.size());
	for (auto& f : g_retired) {
		if (!f.valid())
			continue;
		if (f.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
			ready.push_back(std::move(f));
		else
			pending.push_back(std::move(f));
	}
	g_retired.swap(pending);
	// Drop ready futures after leaving the ready container's move, still under
	// lock: ready shared states do not block on destruction.
	ready.clear();
}

void RetireInflightLocked() {
	// Caller holds g_mutex. Move the active handle into g_retired without
	// destroying an unfinished async shared state under the lock.
	if (!g_inflight.valid())
		return;
	g_retired.push_back(std::move(g_inflight));
	g_inflight = {};
	ReapRetiredLocked();
}

SnapshotFuture StartBuildLocked(std::uint64_t generation) {
	// Caller holds g_mutex. Must not hold the lock while BuildFontFamilyCatalog runs.
	return std::async(std::launch::async, [generation]() {
		auto built = std::make_shared<FontFamilyCatalog const>(BuildFontFamilyCatalog());
		std::lock_guard lock(g_mutex);
		if (generation == g_generation) {
			// Only publish if this build still matches the current generation.
			g_snapshot = built;
		}
		// Return the local result for waiters of this generation even when
		// invalidated; do not install a stale process snapshot in that case.
		return built;
	}).share();
}

SnapshotFuture EnsureInflightLocked() {
	if (g_snapshot) {
		// Already published; synthesize a ready future for callers that wait.
		std::promise<Snapshot> ready;
		ready.set_value(g_snapshot);
		return ready.get_future().share();
	}
	ReapRetiredLocked();
	if (!g_inflight.valid())
		g_inflight = StartBuildLocked(g_generation);
	return g_inflight;
}

} // namespace

std::shared_ptr<FontFamilyCatalog const> GetSnapshot() {
	SnapshotFuture inflight;
	{
		std::lock_guard lock(g_mutex);
		if (g_snapshot)
			return g_snapshot;
		inflight = EnsureInflightLocked();
	}
	// Wait outside the mutex so builders and other waiters are not serialized
	// behind this lock for the full enumeration.
	return inflight.get();
}

void WarmAsync() {
	std::lock_guard lock(g_mutex);
	if (g_snapshot)
		return;
	ReapRetiredLocked();
	if (!g_inflight.valid())
		g_inflight = StartBuildLocked(g_generation);
}

void Invalidate() {
	// Must not destroy the last reference to an unfinished std::async future
	// while holding g_mutex: MSVC waits for the task in that destructor, and
	// the task needs g_mutex to publish/return.
	//
	// Keep unfinished futures in g_retired so Invalidate itself never blocks;
	// only reaped when already ready.
	std::lock_guard lock(g_mutex);
	++g_generation;
	g_snapshot.reset();
	RetireInflightLocked();
}

std::shared_ptr<FontFamilyCatalog const> Rebuild() {
	Invalidate();
	return GetSnapshot();
}

} // namespace font_family_catalog_cache
