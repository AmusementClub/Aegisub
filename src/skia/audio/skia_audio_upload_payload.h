#pragma once

#include "skia_audio_content.h"
#include "skia_audio_frame_model.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace aegisub::skia::audio {

inline constexpr std::uint32_t kWaveformUploadMaskHeight = 256;
inline constexpr std::uint64_t kWaveformUploadPayloadRevision = 1;

struct ContentUploadPayloadKey {
	ContentTileKey tile;
	std::uint64_t variant_revision = 0;

	friend bool operator==(ContentUploadPayloadKey const&, ContentUploadPayloadKey const&) = default;
};

struct ContentUploadPayloadKeyHash {
	std::size_t operator()(ContentUploadPayloadKey const& key) const noexcept;
};

// Immutable worker output in the exact byte layout consumed by Skia uploads.
// Waveform payloads contain two A8 masks; spectrum payloads contain one RGBA
// power texture and are revisioned by the SpectrumBandPlan.
struct ContentUploadPayload {
	ContentUploadPayloadKey key;
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	std::vector<std::uint8_t> primary;
	std::vector<std::uint8_t> secondary;

	bool HasValidShape() const noexcept;
	bool IsValid() const noexcept;
	std::size_t DataBytes() const noexcept;
};

enum class ContentUploadPayloadBuildStatus {
	Ready,
	Cancelled,
	InvalidRequest,
};

struct ContentUploadPayloadBuildResult {
	ContentUploadPayloadBuildStatus status = ContentUploadPayloadBuildStatus::InvalidRequest;
	std::shared_ptr<ContentUploadPayload const> payload;
};

using ContentUploadPayloadContinue = std::function<bool(ContentGeneration)>;

ContentUploadPayloadKey MakeContentUploadPayloadKey(
	ContentTileKey const& tile,
	SpectrumBandPlan const *spectrum_band_plan = nullptr) noexcept;

ContentUploadPayloadBuildResult BuildWaveformUploadPayload(
	ContentTile const& tile,
	ContentUploadPayloadContinue const& should_continue = {});
ContentUploadPayloadBuildResult BuildSpectrumUploadPayload(
	ContentTile const& tile,
	SpectrumBandPlan const& plan,
	ContentUploadPayloadContinue const& should_continue = {});

std::size_t EstimateContentUploadPayloadBytes(
	ContentTileKey const& key,
	std::uint32_t spectrum_output_height = 0) noexcept;

class ContentUploadPayloadStore final {
	struct Impl;
	std::unique_ptr<Impl> impl;

public:
	using EvictionCallback = std::function<void(ContentUploadPayloadKey const&, std::size_t)>;

	explicit ContentUploadPayloadStore(
		std::size_t budget_bytes,
		EvictionCallback eviction_callback = {});
	~ContentUploadPayloadStore();

	ContentUploadPayloadStore(ContentUploadPayloadStore const&) = delete;
	ContentUploadPayloadStore& operator=(ContentUploadPayloadStore const&) = delete;

	void ResetGeneration(ContentGeneration generation);
	void SetBudget(std::size_t budget_bytes);
	ContentPublishResult Publish(std::shared_ptr<ContentUploadPayload const> payload);
	std::shared_ptr<ContentUploadPayload const> Find(ContentUploadPayloadKey const& key);
	ContentStoreMetrics Metrics() const;
};

}
