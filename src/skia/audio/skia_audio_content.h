#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace aegisub::skia::audio {

inline constexpr std::size_t kDefaultContentCacheBudgetBytes = 32 * 1024 * 1024;
inline constexpr std::size_t kDefaultUploadPayloadCacheBudgetBytes = 8 * 1024 * 1024;
inline constexpr std::size_t kMinimumSpectrumAnalysisBudgetBytes = 8 * 1024 * 1024;
inline constexpr std::uint32_t kMaximumContentPrefetchTileCount = 4;

enum class ContentKind {
	Waveform,
	Spectrum,
};

struct ContentGeneration {
	std::uint64_t provider = 0;
	std::uint64_t analysis = 0;

	friend bool operator==(ContentGeneration const&, ContentGeneration const&) = default;
};

struct ContentTileKey {
	ContentGeneration generation;
	ContentKind kind = ContentKind::Waveform;
	std::uint64_t tile_index = 0;
	std::uint32_t column_count = 0;
	std::uint32_t spectrum_bin_count = 0;

	friend bool operator==(ContentTileKey const&, ContentTileKey const&) = default;
};

struct WaveformColumn {
	float peak_min = 0.f;
	float peak_max = 0.f;
	float average_min = 0.f;
	float average_max = 0.f;

	friend bool operator==(WaveformColumn const&, WaveformColumn const&) = default;
};

// Immutable worker output. Spectrum power uses column-major layout so one FFT
// result remains contiguous: column * spectrum_bin_count + bin.
struct ContentTile {
	ContentTileKey key;
	std::vector<WaveformColumn> waveform;
	std::vector<float> spectrum_power;

	bool HasValidShape() const noexcept;
	bool IsValid() const noexcept;
	std::size_t DataBytes() const noexcept;
};

enum class ContentPublishResult {
	Accepted,
	Duplicate,
	Stale,
	Invalid,
	OverBudget,
};

struct ContentStoreMetrics {
	std::uint64_t publishes = 0;
	std::uint64_t duplicates = 0;
	std::uint64_t stale_drops = 0;
	std::uint64_t invalid_drops = 0;
	std::uint64_t over_budget_drops = 0;
	std::uint64_t hits = 0;
	std::uint64_t misses = 0;
	std::uint64_t evictions = 0;
	std::size_t entries = 0;
	std::size_t bytes = 0;
	std::size_t budget_bytes = 0;
};

// Thread-safe handoff/cache for immutable analysis tiles. Find never performs
// provider I/O or analysis; a miss is returned immediately. ResetGeneration
// invalidates queued/in-flight work by making later publication stale.
class ContentTileStore final {
	struct Impl;
	std::unique_ptr<Impl> impl;

public:
	using EvictionCallback = std::function<void(ContentTileKey const&, std::size_t)>;

	explicit ContentTileStore(
		std::size_t budget_bytes = kDefaultContentCacheBudgetBytes,
		EvictionCallback eviction_callback = {});
	~ContentTileStore();

	ContentTileStore(ContentTileStore const&) = delete;
	ContentTileStore& operator=(ContentTileStore const&) = delete;

	void ResetGeneration(ContentGeneration generation);
	ContentGeneration Generation() const;
	bool IsCurrent(ContentGeneration generation) const;
	void SetBudget(std::size_t budget_bytes);

	ContentPublishResult Publish(std::shared_ptr<ContentTile const> tile);
	std::shared_ptr<ContentTile const> Find(ContentTileKey const& key);
	ContentStoreMetrics Metrics() const;
};

struct ContentViewportRequest {
	ContentGeneration generation;
	ContentKind kind = ContentKind::Waveform;
	std::uint64_t first_column = 0;
	std::uint32_t column_count = 0;
	std::uint32_t tile_column_count = 0;
	std::uint32_t spectrum_bin_count = 0;
	// Number of adjacent tiles to build on each side after visible tiles.
	std::uint32_t prefetch_tile_count = 0;

	friend bool operator==(ContentViewportRequest const&, ContentViewportRequest const&) = default;
};

struct ContentCacheBudgetPlan {
	std::size_t configured_total_bytes = 0;
	std::size_t effective_total_bytes = 0;
	std::size_t visible_bytes = 0;
	std::size_t prefetch_bytes = 0;
	std::size_t content_budget_bytes = 0;
	std::size_t payload_visible_bytes = 0;
	std::size_t payload_prefetch_bytes = 0;
	std::size_t payload_budget_bytes = 0;
	std::size_t spectrum_analysis_budget_bytes = 0;
	bool soft_limit_exceeded = false;
	bool valid = false;
};

std::vector<ContentTileKey> PlanVisibleContentTiles(ContentViewportRequest const& request);
std::size_t EstimateContentTileBytes(ContentTileKey const& key) noexcept;
ContentCacheBudgetPlan PlanContentCacheBudget(
	ContentViewportRequest const& request,
	std::size_t configured_total_bytes,
	std::uint32_t spectrum_output_height = 0);

}
