#include "skia_audio_content_worker.h"

#include "../../audio_display_source.h"

#include <libaegisub/audio/provider.h>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <exception>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace aegisub::skia::audio {
namespace {

std::uint64_t NextGeneration(std::uint64_t value) noexcept {
	++value;
	return value ? value : 1;
}

struct WorkPlan {
	std::uint64_t serial = 0;
	ContentGeneration generation;
	ContentAnalysisConfig analysis;
	std::vector<ContentTileKey> tiles;
};

}

bool ContentAnalysisConfig::IsValid() const noexcept {
	if (!std::isfinite(milliseconds_per_pixel) || milliseconds_per_pixel <= 0.0)
		return false;
	if (kind == ContentKind::Waveform)
		return spectrum_derivation_size == 0 && spectrum_derivation_distance == 0;
	return spectrum_derivation_size >= 4
		&& spectrum_derivation_size <= 12
		&& spectrum_derivation_distance <= spectrum_derivation_size;
}

struct ContentWorker::Impl {
	explicit Impl(ReadyCallback ready_callback, std::size_t content_budget_bytes)
	: store(content_budget_bytes)
	, ready_callback(std::move(ready_callback)) {
	}

	mutable std::mutex mutex;
	std::condition_variable wake;
	ContentTileStore store;
	ReadyCallback ready_callback;
	std::thread thread;
	agi::AudioProvider *provider = nullptr;
	ContentGeneration generation;
	ContentAnalysisConfig analysis;
	ContentWorkerMetrics metrics;
	std::optional<WorkPlan> latest;
	std::uint64_t request_serial = 0;
	std::uint64_t active_serial = 0;
	bool stop = false;

	bool IsCurrent(std::uint64_t serial, ContentGeneration candidate) const {
		std::lock_guard<std::mutex> lock(mutex);
		return !stop
			&& serial == request_serial
			&& candidate == generation
			&& provider;
	}

	void StopAndJoin() {
		{
			std::lock_guard<std::mutex> lock(mutex);
			stop = true;
			latest.reset();
			++request_serial;
			metrics.request_pending = false;
		}
		wake.notify_all();
		if (thread.joinable())
			thread.join();
		{
			std::lock_guard<std::mutex> lock(mutex);
			stop = false;
			active_serial = 0;
			metrics.build_active = false;
		}
	}

	std::unique_ptr<AudioDisplaySource> CreateSource(
		agi::AudioProvider *worker_provider,
		ContentSourceMode mode) const {
		if (mode == ContentSourceMode::Int16Mono)
			return CreateInt16MonoAudioDisplaySource(worker_provider);
		return CreateAudioDisplaySource(worker_provider);
	}

	void WorkerLoop(agi::AudioProvider *worker_provider) noexcept {
		std::unique_ptr<AudioDisplaySource> source;
		std::unique_ptr<ContentAnalyzer> analyzer;
		std::optional<ContentSourceMode> source_mode;

		for (;;) {
			WorkPlan plan;
			{
				std::unique_lock<std::mutex> lock(mutex);
				wake.wait(lock, [&] { return stop || latest.has_value(); });
				if (stop)
					break;
				plan = std::move(*latest);
				latest.reset();
				active_serial = plan.serial;
				metrics.request_pending = false;
				metrics.build_active = true;
			}

			try {
				if (!source_mode || *source_mode != plan.analysis.source_mode) {
					analyzer.reset();
					source = CreateSource(worker_provider, plan.analysis.source_mode);
					analyzer = source ? std::make_unique<ContentAnalyzer>(*source) : nullptr;
					source_mode = plan.analysis.source_mode;
				}

				for (auto const& key : plan.tiles) {
					if (!IsCurrent(plan.serial, plan.generation))
						break;
					if (store.Find(key))
						continue;

					{
						std::lock_guard<std::mutex> lock(mutex);
						++metrics.builds_started;
					}

					ContentBuildResult built;
					if (analyzer && plan.analysis.kind == ContentKind::Waveform) {
						WaveformBuildRequest request;
						request.key = key;
						request.milliseconds_per_pixel = plan.analysis.milliseconds_per_pixel;
						request.mix_policy = plan.analysis.mix_policy;
						built = analyzer->BuildWaveform(request, [this, serial = plan.serial](ContentGeneration value) {
							return IsCurrent(serial, value);
						});
					}
					else if (analyzer) {
						SpectrumBuildRequest request;
						request.key = key;
						request.milliseconds_per_pixel = plan.analysis.milliseconds_per_pixel;
						request.mix_policy = plan.analysis.mix_policy;
						request.derivation_size = plan.analysis.spectrum_derivation_size;
						request.derivation_distance = plan.analysis.spectrum_derivation_distance;
						built = analyzer->BuildSpectrum(request, [this, serial = plan.serial](ContentGeneration value) {
							return IsCurrent(serial, value);
						});
					}

					if (built.status == ContentBuildStatus::Cancelled) {
						std::lock_guard<std::mutex> lock(mutex);
						++metrics.builds_cancelled;
						break;
					}
					if (built.status != ContentBuildStatus::Ready || !built.tile) {
						std::lock_guard<std::mutex> lock(mutex);
						++metrics.builds_invalid;
						continue;
					}

					auto const published = store.Publish(std::move(built.tile));
					if (published != ContentPublishResult::Accepted)
						continue;
					{
						std::lock_guard<std::mutex> lock(mutex);
						++metrics.builds_ready;
					}
					if (ready_callback && IsCurrent(plan.serial, plan.generation)) {
						ready_callback(plan.generation);
						std::lock_guard<std::mutex> lock(mutex);
						++metrics.ready_notifications;
					}
				}
			}
			catch (...) {
				std::lock_guard<std::mutex> lock(mutex);
				++metrics.builds_invalid;
			}

			{
				std::lock_guard<std::mutex> lock(mutex);
				if (active_serial == plan.serial)
					active_serial = 0;
				metrics.build_active = active_serial != 0;
			}
		}
	}
};

ContentWorker::ContentWorker(ReadyCallback ready_callback, std::size_t content_budget_bytes)
: impl(std::make_unique<Impl>(std::move(ready_callback), content_budget_bytes)) {
}

ContentWorker::~ContentWorker() {
	impl->StopAndJoin();
}

ContentGeneration ContentWorker::SetProvider(agi::AudioProvider *provider) {
	{
		std::lock_guard<std::mutex> lock(impl->mutex);
		if (impl->provider == provider)
			return impl->generation;
	}

	impl->StopAndJoin();
	ContentGeneration generation;
	{
		std::lock_guard<std::mutex> lock(impl->mutex);
		impl->provider = provider;
		impl->generation.provider = NextGeneration(impl->generation.provider);
		impl->generation.analysis = NextGeneration(impl->generation.analysis);
		generation = impl->generation;
		++impl->request_serial;
		++impl->metrics.provider_resets;
		impl->metrics.provider_attached = provider != nullptr;
	}
	impl->store.ResetGeneration(generation);
	if (provider)
		impl->thread = std::thread([state = impl.get(), provider] { state->WorkerLoop(provider); });
	return generation;
}

ContentGeneration ContentWorker::SetAnalysis(ContentAnalysisConfig config) {
	ContentGeneration generation;
	{
		std::lock_guard<std::mutex> lock(impl->mutex);
		if (impl->analysis == config)
			return impl->generation;
		impl->analysis = config;
		impl->generation.analysis = NextGeneration(impl->generation.analysis);
		generation = impl->generation;
		impl->latest.reset();
		++impl->request_serial;
		++impl->metrics.analysis_resets;
		impl->metrics.request_pending = false;
	}
	impl->store.ResetGeneration(generation);
	impl->wake.notify_all();
	return generation;
}

ContentGeneration ContentWorker::Generation() const {
	std::lock_guard<std::mutex> lock(impl->mutex);
	return impl->generation;
}

void ContentWorker::Request(ContentViewportRequest request) {
	auto tiles = PlanVisibleContentTiles(request);
	if (tiles.empty())
		return;

	{
		std::lock_guard<std::mutex> lock(impl->mutex);
		if (!impl->provider
			|| !impl->analysis.IsValid()
			|| request.generation != impl->generation
			|| request.kind != impl->analysis.kind)
			return;
		if (request.kind == ContentKind::Waveform && request.spectrum_bin_count != 0)
			return;
		if (request.kind == ContentKind::Spectrum) {
			auto const bins = static_cast<std::size_t>(1) << impl->analysis.spectrum_derivation_size;
			if (request.spectrum_bin_count != bins)
				return;
		}

		if (impl->latest || impl->active_serial)
			++impl->metrics.superseded_requests;
		WorkPlan plan;
		plan.serial = ++impl->request_serial;
		plan.generation = impl->generation;
		plan.analysis = impl->analysis;
		plan.tiles = std::move(tiles);
		impl->latest = std::move(plan);
		++impl->metrics.requests;
		impl->metrics.request_pending = true;
	}
	impl->wake.notify_one();
}

std::shared_ptr<ContentTile const> ContentWorker::Find(ContentTileKey const& key) {
	return impl->store.Find(key);
}

ContentWorkerMetrics ContentWorker::Metrics() const {
	std::lock_guard<std::mutex> lock(impl->mutex);
	return impl->metrics;
}

ContentStoreMetrics ContentWorker::StoreMetrics() const {
	return impl->store.Metrics();
}

}
