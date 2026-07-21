#include "font_family_catalog_cache.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace font_family_catalog_cache {
namespace {

using Snapshot = std::shared_ptr<FontFamilyCatalog const>;
using SnapshotFuture = std::shared_future<Snapshot>;
using CatalogBuilder = std::function<FontFamilyCatalog()>;

class FunctionCatalogSource final : public IFontFamilyCatalogSource {
	CatalogBuilder builder;
	FontFamilyCatalogSourceInfo info;

public:
	explicit FunctionCatalogSource(CatalogBuilder builder)
	: builder(std::move(builder))
	{
		info.provider = "injected";
	}

	FontFamilyCatalogSourceInfo const& Info() const noexcept override {
		return info;
	}

	FontFamilyCatalog Build() override {
		return builder();
	}
};

std::mutex g_mutex;
// Bumped on Invalidate so in-flight builders never publish a stale snapshot.
std::uint64_t g_generation = 0;
// Published snapshots are immutable; readers can take this fast path without
// contending with in-flight build coordination.
std::atomic<Snapshot> g_snapshot;
SnapshotFuture g_inflight;
std::uint64_t g_inflight_generation = 0;
std::uint64_t g_inflight_attempt = 0;
std::uint64_t g_next_attempt = 0;
bool g_shutdown = false;
// Retired async futures that must not be destroyed under g_mutex (MSVC STL
// blocks on the last async shared_future destructor). Incomplete futures stay
// here until they are ready so Invalidate remains non-blocking.
std::vector<SnapshotFuture> g_retired;
FontFamilyCatalogSourcePtr g_source;

FontFamilyCatalogSourcePtr EnsureSourceLocked() {
	// Caller holds g_mutex. Lazy initialization avoids cross-translation-unit
	// static initialization ordering between the cache and platform factory.
	if (!g_source)
		g_source = CreatePlatformFontFamilyCatalogSource();
	return g_source;
}

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
	g_inflight_generation = 0;
	g_inflight_attempt = 0;
	ReapRetiredLocked();
}

SnapshotFuture StartBuildLocked(std::uint64_t generation) {
	// Caller holds g_mutex. Must not hold the lock while BuildFontFamilyCatalog runs.
	auto source = EnsureSourceLocked();
	return std::async(std::launch::async, [generation, source = std::move(source)]() {
		auto built = std::make_shared<FontFamilyCatalog const>(source->Build());
		std::lock_guard lock(g_mutex);
		if (!g_shutdown && generation == g_generation) {
			// Only publish if this build still matches the current generation.
			g_snapshot.store(built, std::memory_order_release);
		}
		// Return the local result for waiters of this generation even when
		// invalidated; do not install a stale process snapshot in that case.
		return built;
	}).share();
}

SnapshotFuture EnsureInflightLocked() {
	if (g_shutdown)
		throw std::logic_error("Font family catalog cache is shut down");
	if (auto snapshot = g_snapshot.load(std::memory_order_acquire)) {
		// Already published; synthesize a ready future for callers that wait.
		std::promise<Snapshot> ready;
		ready.set_value(std::move(snapshot));
		return ready.get_future().share();
	}
	ReapRetiredLocked();
	if (!g_inflight.valid()) {
		g_inflight = StartBuildLocked(g_generation);
		g_inflight_generation = g_generation;
		g_inflight_attempt = ++g_next_attempt;
	}
	return g_inflight;
}

} // namespace

void SetSource(FontFamilyCatalogSourcePtr source) {
	if (!source)
		throw std::invalid_argument("Font family catalog source must not be null");

	std::lock_guard lock(g_mutex);
	if (g_shutdown)
		throw std::logic_error("Font family catalog cache is shut down");
	++g_generation;
	g_snapshot.store(Snapshot{}, std::memory_order_release);
	RetireInflightLocked();
	g_source = std::move(source);
}

FontFamilyCatalogSourceInfo GetSourceInfo() {
	std::lock_guard lock(g_mutex);
	if (g_shutdown)
		return {};
	auto source = EnsureSourceLocked();
	return source ? source->Info() : FontFamilyCatalogSourceInfo{};
}

std::uint64_t GetGeneration() {
	std::lock_guard lock(g_mutex);
	return g_generation;
}

std::shared_ptr<FontFamilyCatalog const> GetSnapshot() {
	for (;;) {
		if (auto snapshot = g_snapshot.load(std::memory_order_acquire))
			return snapshot;

		SnapshotFuture inflight;
		std::uint64_t generation = 0;
		std::uint64_t attempt = 0;
		{
			std::lock_guard lock(g_mutex);
			if (auto snapshot = g_snapshot.load(std::memory_order_acquire))
				return snapshot;
			inflight = EnsureInflightLocked();
			generation = g_inflight_generation;
			attempt = g_inflight_attempt;
		}

		Snapshot built;
		try {
			// Wait outside the mutex so builders and other waiters are not
			// serialized behind this lock for the full enumeration.
			built = inflight.get();
		}
		catch (...) {
			bool retry_current_generation = false;
			{
				std::lock_guard lock(g_mutex);
				// Invalidate() can retire this future and start a newer build while
				// the old builder is still running. Its exception belongs only to
				// the retired generation; wait for the current generation instead.
				retry_current_generation = generation != g_generation;
				if (g_inflight.valid() &&
				    g_inflight_generation == generation &&
				    g_inflight_attempt == attempt) {
					g_inflight = {};
					g_inflight_generation = 0;
					g_inflight_attempt = 0;
				}
			}
			if (retry_current_generation)
				continue;
			throw;
		}

		std::lock_guard lock(g_mutex);
		if (g_shutdown)
			throw std::logic_error("Font family catalog cache is shut down");
		if (generation != g_generation)
			continue;
		if (auto snapshot = g_snapshot.load(std::memory_order_acquire))
			return snapshot;
		// The matching-generation builder normally publishes before returning.
		// Keep the returned value as a defensive fallback if publication was
		// skipped for an unexpected reason.
		g_snapshot.store(built, std::memory_order_release);
		return built;
	}
}

std::shared_ptr<FontFamilyCatalog const> TryGetSnapshot() noexcept {
	try {
		return GetSnapshot();
	}
	catch (...) {
		return {};
	}
}

std::shared_ptr<FontFamilyCatalog const> GetReadySnapshot() noexcept {
	return g_snapshot.load(std::memory_order_acquire);
}

void WarmAsync() {
	std::lock_guard lock(g_mutex);
	if (g_shutdown || g_snapshot.load(std::memory_order_acquire))
		return;
	ReapRetiredLocked();
	if (g_inflight.valid() &&
	    g_inflight.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
		try {
			(void)g_inflight.get();
		}
		catch (...) {
			// A ready failed future is cheap to inspect here. Clear it so a UI
			// caller using only the nonblocking path can initiate a retry.
			g_inflight = {};
			g_inflight_generation = 0;
			g_inflight_attempt = 0;
		}
	}
	if (!g_inflight.valid()) {
		g_inflight = StartBuildLocked(g_generation);
		g_inflight_generation = g_generation;
		g_inflight_attempt = ++g_next_attempt;
	}
}

void Invalidate() {
	// Must not destroy the last reference to an unfinished std::async future
	// while holding g_mutex: MSVC waits for the task in that destructor, and
	// the task needs g_mutex to publish/return.
	//
	// Keep unfinished futures in g_retired so Invalidate itself never blocks;
	// only reaped when already ready.
	std::lock_guard lock(g_mutex);
	if (g_shutdown)
		return;
	++g_generation;
	g_snapshot.store(Snapshot{}, std::memory_order_release);
	RetireInflightLocked();
}

std::shared_ptr<FontFamilyCatalog const> Rebuild() {
	Invalidate();
	return GetSnapshot();
}

void Shutdown() {
	std::vector<SnapshotFuture> futures;
	{
		std::lock_guard lock(g_mutex);
		if (g_shutdown)
			return;
		g_shutdown = true;
		++g_generation;
		g_snapshot.store(Snapshot{}, std::memory_order_release);
		if (g_inflight.valid())
			futures.push_back(std::move(g_inflight));
		g_inflight = {};
		g_inflight_generation = 0;
		g_inflight_attempt = 0;
		for (auto& future : g_retired)
			futures.push_back(std::move(future));
		g_retired.clear();
	}
	// Explicitly wait rather than relying on last-reference destruction: a
	// concurrent GetSnapshot caller may still hold another shared_future.
	for (auto const& future : futures) {
		if (future.valid())
			future.wait();
	}
}

namespace testing {

void Reset() {
	std::vector<SnapshotFuture> futures;
	{
		std::lock_guard lock(g_mutex);
		g_shutdown = false;
		++g_generation;
		g_snapshot.store(Snapshot{}, std::memory_order_release);
		if (g_inflight.valid())
			futures.push_back(std::move(g_inflight));
		g_inflight = {};
		g_inflight_generation = 0;
		g_inflight_attempt = 0;
		for (auto& future : g_retired)
			futures.push_back(std::move(future));
		g_retired.clear();
	}
	// The last std::async future reference may wait. Destroy it only after
	// releasing g_mutex so builders can finish their publication check.
	futures.clear();
}

void SetBuilder(std::function<FontFamilyCatalog()> builder) {
	Reset();
	std::lock_guard lock(g_mutex);
	g_source = std::make_shared<FunctionCatalogSource>(std::move(builder));
}

void SetSource(FontFamilyCatalogSourcePtr source) {
	if (!source)
		throw std::invalid_argument("Font family catalog source must not be null");
	Reset();
	std::lock_guard lock(g_mutex);
	g_source = std::move(source);
}

void RestoreDefaultBuilder() {
	Reset();
	std::lock_guard lock(g_mutex);
	g_source = CreatePlatformFontFamilyCatalogSource();
}

} // namespace testing

} // namespace font_family_catalog_cache
