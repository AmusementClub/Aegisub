#include "skia_audio_upload_payload.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <queue>
#include <unordered_map>
#include <utility>

namespace aegisub::skia::audio {
namespace {

constexpr float kSpectrumPowerEncodingMaximum = 8.f;
constexpr std::uint32_t kSpectrumPowerEncodingFactor = (1u << 24) - 1;

bool CheckedAdd(std::size_t left, std::size_t right, std::size_t& result) noexcept {
	if (left > std::numeric_limits<std::size_t>::max() - right)
		return false;
	result = left + right;
	return true;
}

bool CheckedMultiply(std::size_t left, std::size_t right, std::size_t& result) noexcept {
	if (left != 0 && right > std::numeric_limits<std::size_t>::max() / left)
		return false;
	result = left * right;
	return true;
}

std::size_t HashCombine(std::size_t seed, std::uint64_t value) noexcept {
	return seed ^ (static_cast<std::size_t>(value) + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2));
}

std::size_t HashTileKey(ContentTileKey const& key) noexcept {
	std::size_t hash = 0;
	hash = HashCombine(hash, key.generation.provider);
	hash = HashCombine(hash, key.generation.analysis);
	hash = HashCombine(hash, static_cast<std::uint64_t>(key.kind));
	hash = HashCombine(hash, key.tile_index);
	hash = HashCombine(hash, key.column_count);
	return HashCombine(hash, key.spectrum_bin_count);
}

std::uint16_t EncodeWaveformEndpoint(float value) noexcept {
	value = std::clamp(value, -1.f, 1.f);
	auto const normalized = (static_cast<double>(value) + 1.0) * 0.5;
	return static_cast<std::uint16_t>(std::lround(
		normalized * std::numeric_limits<std::uint16_t>::max()));
}

void StoreWaveformEndpoint(std::uint8_t *destination, float value) noexcept {
	auto const encoded = EncodeWaveformEndpoint(value);
	std::memcpy(destination, &encoded, sizeof(encoded));
}

bool Continue(
	ContentUploadPayloadContinue const& should_continue,
	ContentGeneration generation) {
	return !should_continue || should_continue(generation);
}

}

std::size_t ContentUploadPayloadKeyHash::operator()(
	ContentUploadPayloadKey const& key) const noexcept {
	return HashCombine(HashTileKey(key.tile), key.variant_revision);
}

ContentUploadPayloadKey MakeContentUploadPayloadKey(
	ContentTileKey const& tile,
	SpectrumBandPlan const *spectrum_band_plan) noexcept {
	return {
		tile,
		tile.kind == ContentKind::Waveform
			? kWaveformUploadPayloadRevision
			: spectrum_band_plan ? spectrum_band_plan->revision : 0,
	};
}

bool ContentUploadPayload::HasValidShape() const noexcept {
	if (!key.variant_revision
		|| width == 0
		|| width != key.tile.column_count
		|| height == 0) {
		return false;
	}

	std::size_t pixels = 0;
	if (!CheckedMultiply(width, height, pixels))
		return false;
	if (key.tile.kind == ContentKind::Waveform) {
		std::size_t bytes = 0;
		return key.variant_revision == kWaveformUploadPayloadRevision
			&& key.tile.spectrum_bin_count == 0
			&& height == kWaveformUploadTextureHeight
			&& CheckedMultiply(width, kWaveformUploadBytesPerColumn, bytes)
			&& primary.size() == bytes
			&& secondary.empty();
	}

	std::size_t bytes = 0;
	return key.tile.spectrum_bin_count != 0
		&& CheckedMultiply(pixels, 4, bytes)
		&& primary.size() == bytes
		&& secondary.empty();
}

bool ContentUploadPayload::IsValid() const noexcept {
	return key.tile.generation.provider != 0
		&& key.tile.generation.analysis != 0
		&& HasValidShape();
}

std::size_t ContentUploadPayload::DataBytes() const noexcept {
	std::size_t total = sizeof(ContentUploadPayload);
	if (!CheckedAdd(total, primary.size(), total)
		|| !CheckedAdd(total, secondary.size(), total)) {
		return std::numeric_limits<std::size_t>::max();
	}
	return total;
}

ContentUploadPayloadBuildResult BuildWaveformUploadPayload(
	ContentTile const& tile,
	ContentUploadPayloadContinue const& should_continue) {
	if (!tile.IsValid() || tile.key.kind != ContentKind::Waveform)
		return {};

	auto payload = std::make_shared<ContentUploadPayload>();
	payload->key = MakeContentUploadPayloadKey(tile.key);
	payload->width = tile.key.column_count;
	payload->height = kWaveformUploadTextureHeight;
	payload->primary.resize(
		static_cast<std::size_t>(payload->width) * kWaveformUploadBytesPerColumn);

	for (std::size_t x = 0; x < payload->width; ++x) {
		if (!Continue(should_continue, tile.key.generation))
			return { ContentUploadPayloadBuildStatus::Cancelled, {} };
		auto const& column = tile.waveform[x];
		auto peak_min = column.peak_min;
		auto peak_max = column.peak_max;
		auto average_min = column.average_min;
		auto average_max = column.average_max;
		if (peak_min > peak_max)
			std::swap(peak_min, peak_max);
		if (average_min > average_max)
			std::swap(average_min, average_max);
		auto *encoded = payload->primary.data() + x * kWaveformUploadBytesPerColumn;
		StoreWaveformEndpoint(encoded + sizeof(std::uint16_t) * 0, peak_min);
		StoreWaveformEndpoint(encoded + sizeof(std::uint16_t) * 1, peak_max);
		StoreWaveformEndpoint(encoded + sizeof(std::uint16_t) * 2, average_min);
		StoreWaveformEndpoint(encoded + sizeof(std::uint16_t) * 3, average_max);
	}

	if (!payload->IsValid())
		return {};
	return { ContentUploadPayloadBuildStatus::Ready, std::move(payload) };
}

ContentUploadPayloadBuildResult BuildSpectrumUploadPayload(
	ContentTile const& tile,
	SpectrumBandPlan const& plan,
	ContentUploadPayloadContinue const& should_continue) {
	if (!tile.IsValid()
		|| tile.key.kind != ContentKind::Spectrum
		|| !plan.IsValid()
		|| tile.key.spectrum_bin_count != plan.bin_count) {
		return {};
	}

	auto payload = std::make_shared<ContentUploadPayload>();
	payload->key = MakeContentUploadPayloadKey(tile.key, &plan);
	payload->width = tile.key.column_count;
	payload->height = static_cast<std::uint32_t>(plan.output_height);
	auto const pixel_count = static_cast<std::size_t>(payload->width) * payload->height;
	payload->primary.resize(pixel_count * 4);

	for (std::size_t x = 0; x < payload->width; ++x) {
		if (!Continue(should_continue, tile.key.generation))
			return { ContentUploadPayloadBuildStatus::Cancelled, {} };
		for (std::size_t y = 0; y < payload->height; ++y) {
			auto const& band = plan.bands[y];
			float power = 0.f;
			if (plan.interpolated) {
				auto const lower = tile.spectrum_power[
					x * tile.key.spectrum_bin_count + band.first];
				auto const upper = tile.spectrum_power[
					x * tile.key.spectrum_bin_count + band.last];
				power = (1.f - band.fraction) * lower + band.fraction * upper;
			}
			else {
				for (auto bin = band.first; bin <= band.last; ++bin)
					power = std::max(power, tile.spectrum_power[
						x * tile.key.spectrum_bin_count + bin]);
			}
			power = std::clamp(power, 0.f, kSpectrumPowerEncodingMaximum);
			auto const encoded = static_cast<std::uint32_t>(std::lround(
				power / kSpectrumPowerEncodingMaximum
					* kSpectrumPowerEncodingFactor));
			auto const image_y = payload->height - 1 - y;
			auto *pixel = payload->primary.data()
				+ (static_cast<std::size_t>(image_y) * payload->width + x) * 4;
			pixel[0] = static_cast<std::uint8_t>(encoded >> 16);
			pixel[1] = static_cast<std::uint8_t>((encoded >> 8) & 0xFF);
			pixel[2] = static_cast<std::uint8_t>(encoded & 0xFF);
			pixel[3] = 255;
		}
	}

	if (!payload->IsValid())
		return {};
	return { ContentUploadPayloadBuildStatus::Ready, std::move(payload) };
}

std::size_t EstimateContentUploadPayloadBytes(
	ContentTileKey const& key,
	std::uint32_t spectrum_output_height) noexcept {
	if (key.column_count == 0)
		return 0;
	auto const height = key.kind == ContentKind::Waveform
		? kWaveformUploadTextureHeight : spectrum_output_height;
	if (height == 0)
		return 0;

	std::size_t pixels = 0;
	std::size_t bytes = 0;
	std::size_t total = sizeof(ContentUploadPayload);
	if (!CheckedMultiply(key.column_count, height, pixels))
		return 0;
	if (key.kind == ContentKind::Waveform) {
		if (key.spectrum_bin_count != 0
			|| !CheckedMultiply(key.column_count, kWaveformUploadBytesPerColumn, bytes))
			return 0;
	}
	else if (key.spectrum_bin_count == 0 || !CheckedMultiply(pixels, 4, bytes)) {
		return 0;
	}
	return CheckedAdd(total, bytes, total) ? total : 0;
}

struct ContentUploadPayloadStore::Impl {
	struct Entry {
		std::shared_ptr<ContentUploadPayload const> payload;
		std::uint64_t touch = 0;
	};
	struct Touch {
		std::uint64_t touch = 0;
		ContentUploadPayloadKey key;
		bool operator>(Touch const& other) const noexcept { return touch > other.touch; }
	};
	struct Eviction {
		ContentUploadPayloadKey key;
		std::size_t bytes = 0;
	};

	mutable std::mutex mutex;
	ContentGeneration generation;
	std::size_t budget_bytes = 0;
	std::size_t bytes = 0;
	std::uint64_t touch_counter = 0;
	std::unordered_map<ContentUploadPayloadKey, Entry, ContentUploadPayloadKeyHash> entries;
	std::priority_queue<Touch, std::vector<Touch>, std::greater<Touch>> touches;
	ContentStoreMetrics metrics;
	EvictionCallback eviction_callback;

	Impl(std::size_t budget, EvictionCallback callback)
	: budget_bytes(std::max<std::size_t>(1, budget))
	, eviction_callback(std::move(callback)) {
		metrics.budget_bytes = budget_bytes;
	}

	void TouchEntry(ContentUploadPayloadKey const& key, Entry& entry) {
		entry.touch = ++touch_counter;
		touches.push({ entry.touch, key });
		if (touches.size() > entries.size() * 4 + 64) {
			decltype(touches) compacted;
			for (auto const& [current_key, current_entry] : entries)
				compacted.push({ current_entry.touch, current_key });
			touches.swap(compacted);
		}
	}

	void Trim(std::vector<Eviction>& evictions) {
		while (bytes > budget_bytes && !entries.empty()) {
			bool evicted = false;
			while (!touches.empty()) {
				auto const candidate = touches.top();
				touches.pop();
				auto entry = entries.find(candidate.key);
				if (entry == entries.end() || entry->second.touch != candidate.touch)
					continue;
				auto const entry_bytes = entry->second.payload->DataBytes();
				bytes -= entry_bytes;
				evictions.push_back({ entry->first, entry_bytes });
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

	void NotifyEvictions(std::vector<Eviction> const& evictions) const {
		if (!eviction_callback)
			return;
		for (auto const& eviction : evictions)
			eviction_callback(eviction.key, eviction.bytes);
	}
};

ContentUploadPayloadStore::ContentUploadPayloadStore(
	std::size_t budget_bytes,
	EvictionCallback eviction_callback)
: impl(std::make_unique<Impl>(budget_bytes, std::move(eviction_callback))) {
}

ContentUploadPayloadStore::~ContentUploadPayloadStore() = default;

void ContentUploadPayloadStore::ResetGeneration(ContentGeneration generation) {
	std::lock_guard<std::mutex> lock(impl->mutex);
	impl->generation = generation;
	impl->entries.clear();
	impl->touches = {};
	impl->bytes = 0;
	impl->touch_counter = 0;
	impl->UpdateSizeMetrics();
}

void ContentUploadPayloadStore::SetBudget(std::size_t budget_bytes) {
	std::vector<Impl::Eviction> evictions;
	{
		std::lock_guard<std::mutex> lock(impl->mutex);
		impl->budget_bytes = std::max<std::size_t>(1, budget_bytes);
		impl->Trim(evictions);
		impl->UpdateSizeMetrics();
	}
	impl->NotifyEvictions(evictions);
}

ContentPublishResult ContentUploadPayloadStore::Publish(
	std::shared_ptr<ContentUploadPayload const> payload) {
	if (!payload || !payload->IsValid()) {
		std::lock_guard<std::mutex> lock(impl->mutex);
		++impl->metrics.invalid_drops;
		return ContentPublishResult::Invalid;
	}

	std::vector<Impl::Eviction> evictions;
	{
		std::lock_guard<std::mutex> lock(impl->mutex);
		if (payload->key.tile.generation != impl->generation) {
			++impl->metrics.stale_drops;
			return ContentPublishResult::Stale;
		}
		auto const payload_bytes = payload->DataBytes();
		if (payload_bytes == 0 || payload_bytes > impl->budget_bytes) {
			++impl->metrics.over_budget_drops;
			return ContentPublishResult::OverBudget;
		}
		if (impl->entries.find(payload->key) != impl->entries.end()) {
			++impl->metrics.duplicates;
			return ContentPublishResult::Duplicate;
		}

		auto const key = payload->key;
		auto [entry, inserted] = impl->entries.emplace(
			key,
			Impl::Entry { std::move(payload), 0 });
		if (!inserted) {
			++impl->metrics.duplicates;
			return ContentPublishResult::Duplicate;
		}
		impl->bytes += payload_bytes;
		impl->TouchEntry(entry->first, entry->second);
		++impl->metrics.publishes;
		impl->Trim(evictions);
		impl->UpdateSizeMetrics();
	}
	impl->NotifyEvictions(evictions);
	return ContentPublishResult::Accepted;
}

std::shared_ptr<ContentUploadPayload const> ContentUploadPayloadStore::Find(
	ContentUploadPayloadKey const& key) {
	std::lock_guard<std::mutex> lock(impl->mutex);
	if (key.tile.generation != impl->generation) {
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
	return entry->second.payload;
}

ContentStoreMetrics ContentUploadPayloadStore::Metrics() const {
	std::lock_guard<std::mutex> lock(impl->mutex);
	impl->UpdateSizeMetrics();
	return impl->metrics;
}

}
