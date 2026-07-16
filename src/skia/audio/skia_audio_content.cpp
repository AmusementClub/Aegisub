#include "skia_audio_content.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <mutex>
#include <queue>
#include <unordered_map>

namespace aegisub::skia::audio {
namespace {

constexpr std::uint32_t kMaximumTileColumns = 512;
constexpr std::uint32_t kMaximumSpectrumBins = 4096;
constexpr std::uint64_t kMaximumVisibleTileCount = 4096;

bool ValidGeneration(ContentGeneration generation) noexcept {
	return generation.provider != 0 && generation.analysis != 0;
}

bool FiniteWaveform(WaveformColumn const& column) noexcept {
	return std::isfinite(column.peak_min)
		&& std::isfinite(column.peak_max)
		&& std::isfinite(column.average_min)
		&& std::isfinite(column.average_max);
}

struct ContentTileKeyHash {
	std::size_t operator()(ContentTileKey const& key) const noexcept {
		auto combine = [](std::size_t seed, std::uint64_t value) {
			return seed ^ (static_cast<std::size_t>(value) + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2));
		};

		std::size_t hash = 0;
		hash = combine(hash, key.generation.provider);
		hash = combine(hash, key.generation.analysis);
		hash = combine(hash, static_cast<std::uint64_t>(key.kind));
		hash = combine(hash, key.tile_index);
		hash = combine(hash, key.column_count);
		return combine(hash, key.spectrum_bin_count);
	}
};

}

bool ContentTile::HasValidShape() const noexcept {
	if (!ValidGeneration(key.generation)
		|| key.column_count == 0
		|| key.column_count > kMaximumTileColumns) {
		return false;
	}

	if (key.kind == ContentKind::Waveform) {
		if (key.spectrum_bin_count != 0
			|| waveform.size() != key.column_count
			|| !spectrum_power.empty()) {
			return false;
		}
		return true;
	}

	if (key.spectrum_bin_count == 0 || key.spectrum_bin_count > kMaximumSpectrumBins || !waveform.empty())
		return false;
	if (key.column_count > std::numeric_limits<std::size_t>::max() / key.spectrum_bin_count)
		return false;
	auto const expected = static_cast<std::size_t>(key.column_count) * key.spectrum_bin_count;
	return spectrum_power.size() == expected;
}

bool ContentTile::IsValid() const noexcept {
	if (!HasValidShape())
		return false;
	if (key.kind == ContentKind::Waveform)
		return std::all_of(waveform.begin(), waveform.end(), FiniteWaveform);
	return std::all_of(spectrum_power.begin(), spectrum_power.end(), [](float value) {
		return std::isfinite(value);
	});
}

std::size_t ContentTile::DataBytes() const noexcept {
	return sizeof(ContentTile)
		+ waveform.capacity() * sizeof(WaveformColumn)
		+ spectrum_power.capacity() * sizeof(float);
}

struct ContentTileStore::Impl {
	struct Entry {
		std::shared_ptr<ContentTile const> tile;
		std::uint64_t touch = 0;
	};
	struct Touch {
		std::uint64_t touch = 0;
		ContentTileKey key;
		bool operator>(Touch const& other) const noexcept { return touch > other.touch; }
	};

	mutable std::mutex mutex;
	ContentGeneration generation;
	std::size_t budget_bytes = 0;
	std::size_t bytes = 0;
	std::uint64_t touch_counter = 0;
	std::unordered_map<ContentTileKey, Entry, ContentTileKeyHash> entries;
	std::priority_queue<Touch, std::vector<Touch>, std::greater<Touch>> touches;
	ContentStoreMetrics metrics;

	explicit Impl(std::size_t budget)
	: budget_bytes(std::max<std::size_t>(1, budget)) {
		metrics.budget_bytes = budget_bytes;
	}

	void TouchEntry(ContentTileKey const& key, Entry& entry) {
		entry.touch = ++touch_counter;
		touches.push({ entry.touch, key });
		if (touches.size() > entries.size() * 4 + 64) {
			decltype(touches) compacted;
			for (auto const& [current_key, current_entry] : entries)
				compacted.push({ current_entry.touch, current_key });
			touches.swap(compacted);
		}
	}

	void Trim() {
		while (bytes > budget_bytes && !entries.empty()) {
			bool evicted = false;
			while (!touches.empty()) {
				auto const candidate = touches.top();
				touches.pop();
				auto entry = entries.find(candidate.key);
				if (entry == entries.end() || entry->second.touch != candidate.touch)
					continue;
				bytes -= entry->second.tile->DataBytes();
				entries.erase(entry);
				++metrics.evictions;
				evicted = true;
				break;
			}
			if (!evicted)
				break;
		}
	}

	void UpdateSizeMetrics() {
		metrics.entries = entries.size();
		metrics.bytes = bytes;
		metrics.budget_bytes = budget_bytes;
	}
};

ContentTileStore::ContentTileStore(std::size_t budget_bytes)
: impl(std::make_unique<Impl>(budget_bytes)) {
}

ContentTileStore::~ContentTileStore() = default;

void ContentTileStore::ResetGeneration(ContentGeneration generation) {
	std::lock_guard<std::mutex> lock(impl->mutex);
	impl->generation = generation;
	impl->entries.clear();
	impl->touches = {};
	impl->bytes = 0;
	impl->touch_counter = 0;
	impl->UpdateSizeMetrics();
}

ContentGeneration ContentTileStore::Generation() const {
	std::lock_guard<std::mutex> lock(impl->mutex);
	return impl->generation;
}

bool ContentTileStore::IsCurrent(ContentGeneration generation) const {
	std::lock_guard<std::mutex> lock(impl->mutex);
	return ValidGeneration(generation) && generation == impl->generation;
}

void ContentTileStore::SetBudget(std::size_t budget_bytes) {
	std::lock_guard<std::mutex> lock(impl->mutex);
	impl->budget_bytes = std::max<std::size_t>(1, budget_bytes);
	impl->Trim();
	impl->UpdateSizeMetrics();
}

ContentPublishResult ContentTileStore::Publish(std::shared_ptr<ContentTile const> tile) {
	if (!tile || !tile->IsValid()) {
		std::lock_guard<std::mutex> lock(impl->mutex);
		++impl->metrics.invalid_drops;
		return ContentPublishResult::Invalid;
	}

	std::lock_guard<std::mutex> lock(impl->mutex);
	if (tile->key.generation != impl->generation) {
		++impl->metrics.stale_drops;
		return ContentPublishResult::Stale;
	}
	auto const tile_bytes = tile->DataBytes();
	if (tile_bytes == 0 || tile_bytes > impl->budget_bytes) {
		++impl->metrics.over_budget_drops;
		return ContentPublishResult::OverBudget;
	}
	if (impl->entries.find(tile->key) != impl->entries.end()) {
		++impl->metrics.duplicates;
		return ContentPublishResult::Duplicate;
	}

	auto const key = tile->key;
	auto [entry, inserted] = impl->entries.emplace(key, Impl::Entry { std::move(tile), 0 });
	if (!inserted) {
		++impl->metrics.duplicates;
		return ContentPublishResult::Duplicate;
	}
	impl->bytes += tile_bytes;
	impl->TouchEntry(entry->first, entry->second);
	++impl->metrics.publishes;
	impl->Trim();
	impl->UpdateSizeMetrics();
	return ContentPublishResult::Accepted;
}

std::shared_ptr<ContentTile const> ContentTileStore::Find(ContentTileKey const& key) {
	std::lock_guard<std::mutex> lock(impl->mutex);
	if (key.generation != impl->generation) {
		++impl->metrics.misses;
		return {};
	}
	auto entry = impl->entries.find(key);
	if (entry == impl->entries.end()) {
		++impl->metrics.misses;
		return {};
	}
	impl->TouchEntry(entry->first, entry->second);
	++impl->metrics.hits;
	return entry->second.tile;
}

ContentStoreMetrics ContentTileStore::Metrics() const {
	std::lock_guard<std::mutex> lock(impl->mutex);
	impl->UpdateSizeMetrics();
	return impl->metrics;
}

std::vector<ContentTileKey> PlanVisibleContentTiles(ContentViewportRequest const& request) {
	std::vector<ContentTileKey> result;
	if (!ValidGeneration(request.generation)
		|| request.column_count == 0
		|| request.tile_column_count == 0
		|| request.tile_column_count > kMaximumTileColumns) {
		return result;
	}
	if (request.kind == ContentKind::Waveform && request.spectrum_bin_count != 0)
		return result;
	if (request.kind == ContentKind::Spectrum
		&& (request.spectrum_bin_count == 0 || request.spectrum_bin_count > kMaximumSpectrumBins)) {
		return result;
	}
	if (request.first_column > std::numeric_limits<std::uint64_t>::max() - (request.column_count - 1))
		return result;

	auto const last_column = request.first_column + request.column_count - 1;
	auto const first_tile = request.first_column / request.tile_column_count;
	auto const last_tile = last_column / request.tile_column_count;
	auto const tile_count_minus_one = last_tile - first_tile;
	if (tile_count_minus_one >= kMaximumVisibleTileCount
		|| tile_count_minus_one >= std::numeric_limits<std::size_t>::max())
		return result;
	result.reserve(static_cast<std::size_t>(tile_count_minus_one + 1));
	for (auto tile_index = first_tile; tile_index <= last_tile; ++tile_index) {
		result.push_back({
			request.generation,
			request.kind,
			tile_index,
			request.tile_column_count,
			request.kind == ContentKind::Spectrum ? request.spectrum_bin_count : 0,
		});
		if (tile_index == std::numeric_limits<std::uint64_t>::max())
			break;
	}
	return result;
}

}
