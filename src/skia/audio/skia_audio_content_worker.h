#pragma once

#include "skia_audio_content.h"
#include "skia_audio_content_analysis.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace agi { class AudioProvider; }

namespace aegisub::skia::audio {

enum class ContentSourceMode {
	FloatInterleaved,
	Int16Mono,
};

struct ContentAnalysisConfig {
	ContentKind kind = ContentKind::Waveform;
	ContentSourceMode source_mode = ContentSourceMode::FloatInterleaved;
	double milliseconds_per_pixel = 0.0;
	AudioMixPolicy mix_policy = AudioMixPolicy::MonoAverage;
	std::size_t spectrum_derivation_size = 0;
	std::size_t spectrum_derivation_distance = 0;

	bool IsValid() const noexcept;
	friend bool operator==(ContentAnalysisConfig const&, ContentAnalysisConfig const&) = default;
};

struct ContentWorkerMetrics {
	std::uint64_t provider_resets = 0;
	std::uint64_t analysis_resets = 0;
	std::uint64_t requests = 0;
	std::uint64_t superseded_requests = 0;
	std::uint64_t builds_started = 0;
	std::uint64_t builds_ready = 0;
	std::uint64_t builds_cancelled = 0;
	std::uint64_t builds_invalid = 0;
	std::uint64_t ready_notifications = 0;
	bool provider_attached = false;
	bool request_pending = false;
	bool build_active = false;
};

// One latest-only analysis worker for one AudioProvider lifetime. SetProvider
// synchronously stops and joins the previous worker before returning, which is
// required because Project destroys its raw AudioProvider immediately after
// announcing nullptr. Request only replaces queued/in-flight viewport work;
// it never performs provider I/O on the caller thread.
class ContentWorker final {
	struct Impl;
	std::unique_ptr<Impl> impl;

public:
	using ReadyCallback = std::function<void(ContentGeneration)>;

	explicit ContentWorker(
		ReadyCallback ready_callback = {},
		std::size_t content_budget_bytes = 32 * 1024 * 1024);
	~ContentWorker();

	ContentWorker(ContentWorker const&) = delete;
	ContentWorker& operator=(ContentWorker const&) = delete;

	ContentGeneration SetProvider(agi::AudioProvider *provider);
	ContentGeneration SetAnalysis(ContentAnalysisConfig config);
	ContentGeneration Generation() const;

	void Request(ContentViewportRequest request);
	std::shared_ptr<ContentTile const> Find(ContentTileKey const& key);

	ContentWorkerMetrics Metrics() const;
	ContentStoreMetrics StoreMetrics() const;
};

}
