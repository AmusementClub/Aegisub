// Copyright (c) 2014, Thomas Goyne <plorkyeran@aegisub.org>
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
//
// Aegisub Project http://www.aegisub.org/

#include "libaegisub/audio/provider.h"

#include "libaegisub/make_unique.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {
using namespace agi;
using Clock = std::chrono::steady_clock;

std::string FormatWrappedProviderName(char const* wrapper_name, AudioProvider const* source) {
	std::string name = wrapper_name;
	if (!source)
		return name;

	auto const source_name = source->GetMemoryStats().provider_name;
	if (source_name.empty())
		return name;

	name += " (";
	name += source_name;
	name += ")";
	return name;
}

constexpr size_t kMinPageBytes = 128 * 1024;
constexpr size_t kTargetPageBytes = 256 * 1024;
constexpr size_t kMaxPageBytes = 1024 * 1024;
constexpr size_t kSoftBudgetBytes = 128 * 1024 * 1024;
constexpr size_t kHardBudgetBytes = 160 * 1024 * 1024;
constexpr size_t kIdleBudgetBytes = 64 * 1024 * 1024;
constexpr size_t kFreeBufferReservePages = 16;
constexpr size_t kViewportMaxPages = 32;
constexpr auto kIdleTrimDelay = std::chrono::milliseconds(1000);

enum PagePinMask : uint32_t {
	PagePinPlaybackCritical = 1u << 0,
	PagePinPlaybackAhead = 1u << 1,
	PagePinPlaybackBehind = 1u << 2,
	PagePinViewport = 1u << 3
};

size_t AlignDownToFrame(size_t bytes, size_t frame_bytes) {
	if (!frame_bytes)
		return bytes;
	return bytes / frame_bytes * frame_bytes;
}

size_t AlignUpToFrame(size_t bytes, size_t frame_bytes) {
	if (!frame_bytes)
		return bytes;
	auto const remainder = bytes % frame_bytes;
	if (!remainder)
		return bytes;
	return bytes + (frame_bytes - remainder);
}

size_t ChoosePageBytes(size_t frame_bytes) {
	if (!frame_bytes)
		return kTargetPageBytes;

	auto const aligned_min = std::max(frame_bytes, AlignUpToFrame(kMinPageBytes, frame_bytes));
	auto const aligned_target = std::max(frame_bytes, AlignDownToFrame(kTargetPageBytes, frame_bytes));
	auto const aligned_max = std::max(frame_bytes, AlignDownToFrame(kMaxPageBytes, frame_bytes));
	return std::clamp(aligned_target, aligned_min, aligned_max);
}

enum class PageState {
	Empty,
	Loading,
	Ready,
	Failed
};

struct PageBuffer {
	std::unique_ptr<char[]> data;
};

struct AudioPageEntry {
	int64_t start_frame = 0;
	int64_t frame_count = 0;
	size_t payload_bytes = 0;
	PageState state = PageState::Empty;
	std::unique_ptr<PageBuffer> buffer;
	uint64_t last_access_seq = 0;
	uint32_t pin_mask = 0;
	uint32_t active_readers = 0;
};

class ExactPagedRAMAudioProvider final : public AudioProviderWrapper {
	struct PageReadPin final {
		ExactPagedRAMAudioProvider const* owner = nullptr;
		size_t page_index = std::numeric_limits<size_t>::max();
		char const* data = nullptr;

		PageReadPin() = default;
		PageReadPin(ExactPagedRAMAudioProvider const* owner, size_t page_index, char const* data)
		: owner(owner), page_index(page_index), data(data) {
		}

		PageReadPin(PageReadPin&& other) noexcept
		: owner(other.owner), page_index(other.page_index), data(other.data) {
			other.owner = nullptr;
			other.data = nullptr;
		}

		PageReadPin& operator=(PageReadPin&& other) noexcept {
			if (this == &other)
				return *this;
			Release();
			owner = other.owner;
			page_index = other.page_index;
			data = other.data;
			other.owner = nullptr;
			other.data = nullptr;
			return *this;
		}

		PageReadPin(PageReadPin const&) = delete;
		PageReadPin& operator=(PageReadPin const&) = delete;

		~PageReadPin() {
			Release();
		}

		void Release() {
			if (!owner)
				return;
			owner->UnpinPage(page_index);
			owner = nullptr;
			data = nullptr;
		}
	};

	struct LoadContext {
		size_t page_index = std::numeric_limits<size_t>::max();
		int64_t start_frame = 0;
		int64_t frame_count = 0;
		std::unique_ptr<PageBuffer> buffer;
		bool synchronous = false;

		explicit operator bool() const {
			return page_index != std::numeric_limits<size_t>::max();
		}
	};

	size_t const frame_bytes;
	size_t const page_bytes;
	int64_t const frames_per_page;
	int64_t const viewport_max_frames;

	mutable std::mutex mutex;
	mutable std::condition_variable page_cv;
	mutable std::condition_variable_any worker_cv;
	mutable std::mutex source_mutex;
	mutable std::vector<AudioPageEntry> pages;
	mutable std::vector<std::unique_ptr<PageBuffer>> free_buffers;
	mutable uint64_t access_seq = 0;
	mutable size_t resident_buffer_bytes = 0;
	mutable int64_t resident_pages = 0;
	mutable int64_t resident_frames = 0;
	mutable int64_t loading_page_count = 0;
	mutable size_t loading_buffer_bytes = 0;
	mutable std::vector<size_t> playback_critical_pages;
	mutable std::vector<size_t> playback_ahead_pages;
	mutable std::vector<size_t> playback_behind_pages;
	mutable std::vector<size_t> viewport_pages;
	mutable bool playback_active = false;
	mutable Clock::time_point last_access_time = Clock::now();
	mutable std::jthread worker;

	void FillBuffer(void *buf, int64_t start, int64_t count) const override;

	size_t StorageBytesUnlocked() const;
	size_t FreeBytesUnlocked() const;
	void MarkAccessUnlocked() const;
	size_t PageIndexFromFrame(int64_t frame) const;
	std::vector<size_t> BuildPageRange(int64_t start_frame, int64_t end_frame) const;
	void ApplyPinSetUnlocked(std::vector<size_t>& current_pages, std::vector<size_t> const& new_pages, uint32_t mask) const;
	std::unique_ptr<PageBuffer> TakeFreeBufferUnlocked() const;
	void ReturnBufferUnlocked(std::unique_ptr<PageBuffer> buffer) const;
	void DrainFreeBuffersUnlocked(size_t max_free_buffers) const;
	bool EvictOneUnlocked() const;
	void TrimStorageUnlocked(size_t target_storage_bytes, size_t max_free_buffers) const;
	void TrimAfterAccess() const;
	LoadContext BeginLoadUnlocked(size_t page_index, bool synchronous) const;
	void CompleteLoadSuccess(LoadContext& load) const;
	void CompleteLoadFailure(LoadContext& load) const;
	void LoadPage(LoadContext& load) const;
	PageReadPin PinPage(size_t page_index) const;
	void UnpinPage(size_t page_index) const;
	bool TrySelectPrefetchPageUnlocked(size_t& page_index) const;
	bool HasPrefetchWorkUnlocked() const;
	void ClearViewportPinsUnlocked() const;
	void WorkerLoop(std::stop_token stop_token) const;

public:
	ExactPagedRAMAudioProvider(std::unique_ptr<AudioProvider> src);
	~ExactPagedRAMAudioProvider() override;

	AudioProviderMemoryStats GetMemoryStats() const override;
	void SetPlaybackWindow(int64_t current_frame, int64_t ahead_frames, int64_t behind_frames) override;
	void ClearPlaybackWindow() override;
	void HintVisibleRange(int64_t start_frame, int64_t frame_count) override;
};

ExactPagedRAMAudioProvider::ExactPagedRAMAudioProvider(std::unique_ptr<AudioProvider> src)
: AudioProviderWrapper(std::move(src))
, frame_bytes(static_cast<size_t>(std::max(1, channels)) * static_cast<size_t>(std::max(1, bytes_per_sample)))
, page_bytes(ChoosePageBytes(frame_bytes))
, frames_per_page(std::max<int64_t>(1, static_cast<int64_t>(page_bytes / frame_bytes)))
, viewport_max_frames(std::min<int64_t>(
	static_cast<int64_t>(kViewportMaxPages) * std::max<int64_t>(1, frames_per_page),
	static_cast<int64_t>(std::max(1, sample_rate)) * 10)) {
	// The paged cache can service any read on demand, so keep the compatibility
	// contract that audio is immediately readable from the UI's perspective.
	decoded_samples = num_samples;

	auto const page_count = static_cast<size_t>((num_samples + frames_per_page - 1) / frames_per_page);
	pages.reserve(page_count);
	for (size_t page_index = 0; page_index < page_count; ++page_index) {
		auto const start_frame = static_cast<int64_t>(page_index) * frames_per_page;
		auto const frame_count = std::min<int64_t>(frames_per_page, num_samples - start_frame);
		pages.push_back({
			start_frame,
			frame_count,
			static_cast<size_t>(frame_count) * frame_bytes
		});
	}

	worker = std::jthread([this](std::stop_token stop_token) { WorkerLoop(stop_token); });
}

ExactPagedRAMAudioProvider::~ExactPagedRAMAudioProvider() {
	if (worker.joinable()) {
		worker.request_stop();
		worker_cv.notify_all();
	}
}

size_t ExactPagedRAMAudioProvider::StorageBytesUnlocked() const {
	return resident_buffer_bytes + loading_buffer_bytes + FreeBytesUnlocked();
}

size_t ExactPagedRAMAudioProvider::FreeBytesUnlocked() const {
	return free_buffers.size() * page_bytes;
}

void ExactPagedRAMAudioProvider::MarkAccessUnlocked() const {
	last_access_time = Clock::now();
}

size_t ExactPagedRAMAudioProvider::PageIndexFromFrame(int64_t frame) const {
	if (pages.empty())
		return 0;
	frame = std::clamp<int64_t>(frame, 0, std::max<int64_t>(0, num_samples - 1));
	return static_cast<size_t>(frame / frames_per_page);
}

std::vector<size_t> ExactPagedRAMAudioProvider::BuildPageRange(int64_t start_frame, int64_t end_frame) const {
	if (pages.empty() || end_frame <= start_frame || num_samples <= 0)
		return {};

	start_frame = std::clamp<int64_t>(start_frame, 0, num_samples);
	end_frame = std::clamp<int64_t>(end_frame, 0, num_samples);
	if (end_frame <= start_frame)
		return {};

	auto const first_page = PageIndexFromFrame(start_frame);
	auto const last_page = PageIndexFromFrame(end_frame - 1);

	std::vector<size_t> result;
	result.reserve(last_page - first_page + 1);
	for (size_t page_index = first_page; page_index <= last_page; ++page_index)
		result.push_back(page_index);
	return result;
}

void ExactPagedRAMAudioProvider::ApplyPinSetUnlocked(std::vector<size_t>& current_pages, std::vector<size_t> const& new_pages, uint32_t mask) const {
	for (auto const page_index : current_pages) {
		if (page_index < pages.size())
			pages[page_index].pin_mask &= ~mask;
	}

	current_pages = new_pages;
	for (auto const page_index : current_pages) {
		if (page_index >= pages.size())
			continue;
		auto& page = pages[page_index];
		page.pin_mask |= mask;
		if (page.state == PageState::Ready)
			page.last_access_seq = ++access_seq;
	}
}

std::unique_ptr<PageBuffer> ExactPagedRAMAudioProvider::TakeFreeBufferUnlocked() const {
	if (free_buffers.empty())
		return {};
	auto buffer = std::move(free_buffers.back());
	free_buffers.pop_back();
	return buffer;
}

void ExactPagedRAMAudioProvider::ReturnBufferUnlocked(std::unique_ptr<PageBuffer> buffer) const {
	if (!buffer)
		return;
	if (free_buffers.size() < kFreeBufferReservePages)
		free_buffers.push_back(std::move(buffer));
}

void ExactPagedRAMAudioProvider::DrainFreeBuffersUnlocked(size_t max_free_buffers) const {
	while (free_buffers.size() > max_free_buffers)
		free_buffers.pop_back();
}

bool ExactPagedRAMAudioProvider::EvictOneUnlocked() const {
	size_t victim = std::numeric_limits<size_t>::max();
	uint64_t oldest_access = std::numeric_limits<uint64_t>::max();

	for (size_t i = 0; i < pages.size(); ++i) {
		auto const& page = pages[i];
		if (page.state != PageState::Ready || page.active_readers != 0 || page.pin_mask != 0 || !page.buffer)
			continue;
		if (page.last_access_seq < oldest_access) {
			oldest_access = page.last_access_seq;
			victim = i;
		}
	}

	if (victim == std::numeric_limits<size_t>::max())
		return false;

	auto& page = pages[victim];
	resident_buffer_bytes -= page_bytes;
	--resident_pages;
	resident_frames -= page.frame_count;
	page.state = PageState::Empty;
	page.last_access_seq = 0;
	ReturnBufferUnlocked(std::move(page.buffer));
	return true;
}

void ExactPagedRAMAudioProvider::TrimStorageUnlocked(size_t target_storage_bytes, size_t max_free_buffers) const {
	DrainFreeBuffersUnlocked(max_free_buffers);

	while (StorageBytesUnlocked() > target_storage_bytes) {
		if (!EvictOneUnlocked())
			break;
		DrainFreeBuffersUnlocked(max_free_buffers);
	}

	DrainFreeBuffersUnlocked(max_free_buffers);
}

void ExactPagedRAMAudioProvider::TrimAfterAccess() const {
	std::lock_guard<std::mutex> lock(mutex);
	if (StorageBytesUnlocked() > kSoftBudgetBytes)
		TrimStorageUnlocked(kSoftBudgetBytes, kFreeBufferReservePages);
}

ExactPagedRAMAudioProvider::LoadContext ExactPagedRAMAudioProvider::BeginLoadUnlocked(size_t page_index, bool synchronous) const {
	auto& page = pages[page_index];
	page.state = PageState::Loading;
	++loading_page_count;
	loading_buffer_bytes += page_bytes;
	TrimStorageUnlocked(synchronous ? kHardBudgetBytes : kSoftBudgetBytes, kFreeBufferReservePages);

	LoadContext load;
	load.page_index = page_index;
	load.start_frame = page.start_frame;
	load.frame_count = page.frame_count;
	load.buffer = TakeFreeBufferUnlocked();
	load.synchronous = synchronous;
	return load;
}

void ExactPagedRAMAudioProvider::CompleteLoadSuccess(LoadContext& load) const {
	std::lock_guard<std::mutex> lock(mutex);
	auto& page = pages[load.page_index];
	page.buffer = std::move(load.buffer);
	page.state = PageState::Ready;
	page.last_access_seq = ++access_seq;
	resident_buffer_bytes += page_bytes;
	++resident_pages;
	resident_frames += page.frame_count;
	--loading_page_count;
	loading_buffer_bytes -= page_bytes;
	page_cv.notify_all();
}

void ExactPagedRAMAudioProvider::CompleteLoadFailure(LoadContext& load) const {
	std::lock_guard<std::mutex> lock(mutex);
	auto& page = pages[load.page_index];
	ReturnBufferUnlocked(std::move(load.buffer));
	page.state = PageState::Failed;
	--loading_page_count;
	loading_buffer_bytes -= page_bytes;
	page_cv.notify_all();
}

void ExactPagedRAMAudioProvider::LoadPage(LoadContext& load) const {
	try {
		if (!load.buffer) {
			load.buffer = std::make_unique<PageBuffer>();
			load.buffer->data = std::make_unique<char[]>(page_bytes);
		}

		std::lock_guard<std::mutex> source_lock(source_mutex);
		source->GetAudio(load.buffer->data.get(), load.start_frame, load.frame_count);
		CompleteLoadSuccess(load);
	}
	catch (...) {
		CompleteLoadFailure(load);
		throw;
	}
}

ExactPagedRAMAudioProvider::PageReadPin ExactPagedRAMAudioProvider::PinPage(size_t page_index) const {
	for (;;) {
		LoadContext load;
		{
			std::unique_lock<std::mutex> lock(mutex);
			auto& page = pages[page_index];
			if (page.state == PageState::Ready && page.buffer) {
				++page.active_readers;
				page.last_access_seq = ++access_seq;
				MarkAccessUnlocked();
				return PageReadPin(this, page_index, page.buffer->data.get());
			}

			if (page.state == PageState::Loading) {
				page_cv.wait(lock, [&] { return pages[page_index].state != PageState::Loading; });
				continue;
			}

			if (page.state == PageState::Failed) {
				page.state = PageState::Empty;
				throw AudioDecodeError("Failed to load a paged RAM audio block");
			}

			load = BeginLoadUnlocked(page_index, true);
			MarkAccessUnlocked();
		}

		try {
			LoadPage(load);
		}
		catch (std::bad_alloc const&) {
			throw AudioDecodeError("Not enough memory available to cache audio page");
		}
		catch (AudioDecodeError const&) {
			throw;
		}
		catch (std::exception const& err) {
			throw AudioDecodeError(err.what());
		}
		catch (...) {
			throw AudioDecodeError("Unknown paged RAM audio load failure");
		}
	}
}

void ExactPagedRAMAudioProvider::UnpinPage(size_t page_index) const {
	{
		std::lock_guard<std::mutex> lock(mutex);
		auto& page = pages[page_index];
		if (page.active_readers > 0)
			--page.active_readers;
	}

	TrimAfterAccess();
}

bool ExactPagedRAMAudioProvider::TrySelectPrefetchPageUnlocked(size_t& page_index) const {
	auto try_select = [&](std::vector<size_t> const& candidates) {
		for (auto const index : candidates) {
			if (index >= pages.size())
				continue;
			auto& page = pages[index];
			if (page.state == PageState::Failed)
				page.state = PageState::Empty;
			if (page.state == PageState::Empty) {
				page_index = index;
				return true;
			}
		}
		return false;
	};

	return try_select(playback_critical_pages)
		|| try_select(playback_ahead_pages)
		|| try_select(playback_behind_pages)
		|| try_select(viewport_pages);
}

bool ExactPagedRAMAudioProvider::HasPrefetchWorkUnlocked() const {
	size_t ignored = 0;
	return TrySelectPrefetchPageUnlocked(ignored);
}

void ExactPagedRAMAudioProvider::ClearViewportPinsUnlocked() const {
	for (auto const page_index : viewport_pages) {
		if (page_index < pages.size())
			pages[page_index].pin_mask &= ~PagePinViewport;
	}
	viewport_pages.clear();
}

void ExactPagedRAMAudioProvider::WorkerLoop(std::stop_token stop_token) const {
	while (!stop_token.stop_requested()) {
		LoadContext load;
		bool should_idle_trim = false;
		{
			std::unique_lock<std::mutex> lock(mutex);
			while (!stop_token.stop_requested()) {
				size_t page_index = std::numeric_limits<size_t>::max();
				if (TrySelectPrefetchPageUnlocked(page_index)) {
					load = BeginLoadUnlocked(page_index, false);
					break;
				}

				auto const idle_deadline = last_access_time + kIdleTrimDelay;
				if (StorageBytesUnlocked() > kIdleBudgetBytes && Clock::now() >= idle_deadline) {
					should_idle_trim = true;
					break;
				}

				worker_cv.wait_until(lock, stop_token, idle_deadline, [&] {
					return stop_token.stop_requested()
						|| HasPrefetchWorkUnlocked()
						|| (StorageBytesUnlocked() > kIdleBudgetBytes && Clock::now() >= last_access_time + kIdleTrimDelay);
				});
			}
		}

		if (stop_token.stop_requested())
			return;

		if (should_idle_trim) {
			std::lock_guard<std::mutex> lock(mutex);
			if (!playback_active)
				ClearViewportPinsUnlocked();
			TrimStorageUnlocked(kIdleBudgetBytes, 0);
			continue;
		}

		if (!load)
			continue;

		try {
			LoadPage(load);
		}
		catch (...) {
		}
	}
}

AudioProviderMemoryStats ExactPagedRAMAudioProvider::GetMemoryStats() const {
	std::lock_guard<std::mutex> lock(mutex);

	int64_t pinned_pages = 0;
	for (auto const& page : pages) {
		if ((page.state == PageState::Ready || page.state == PageState::Loading) && page.pin_mask != 0)
			++pinned_pages;
	}

	AudioProviderMemoryStats stats;
	stats.provider_name = FormatWrappedProviderName("RAM Paged", source.get());
	stats.storage_kind = "memory";
	stats.storage_bytes = StorageBytesUnlocked();
	stats.logical_bytes = static_cast<size_t>(num_samples)
		* static_cast<size_t>(bytes_per_sample)
		* static_cast<size_t>(channels);
	stats.decoded_bytes = resident_buffer_bytes;
	stats.page_size_bytes = page_bytes;
	stats.loading_bytes = loading_buffer_bytes;
	stats.pinned_bytes = static_cast<size_t>(pinned_pages) * page_bytes;
	stats.free_bytes = FreeBytesUnlocked();
	stats.num_samples = num_samples;
	stats.decoded_samples = resident_frames;
	stats.resident_pages = resident_pages;
	stats.loading_pages = loading_page_count;
	stats.pinned_pages = pinned_pages;
	stats.free_pages = static_cast<int64_t>(free_buffers.size());
	stats.sample_rate = sample_rate;
	stats.bytes_per_sample = bytes_per_sample;
	stats.channels = channels;
	stats.float_samples = float_samples;
	return stats;
}

void ExactPagedRAMAudioProvider::SetPlaybackWindow(int64_t current_frame, int64_t ahead_frames, int64_t behind_frames) {
	if (num_samples <= 0)
		return;

	std::lock_guard<std::mutex> lock(mutex);
	MarkAccessUnlocked();
	playback_active = true;

	current_frame = std::clamp<int64_t>(current_frame, 0, num_samples - 1);
	ahead_frames = std::max<int64_t>(0, ahead_frames);
	behind_frames = std::max<int64_t>(0, behind_frames);

	auto const current_page = PageIndexFromFrame(current_frame);
	auto const last_ahead_frame = std::clamp<int64_t>(current_frame + ahead_frames, 0, num_samples - 1);
	auto const first_behind_frame = std::clamp<int64_t>(current_frame - behind_frames, 0, num_samples - 1);
	auto const last_ahead_page = PageIndexFromFrame(last_ahead_frame);
	auto const first_behind_page = PageIndexFromFrame(first_behind_frame);

	std::vector<size_t> critical_pages{ current_page };
	std::vector<size_t> ahead_pages;
	std::vector<size_t> behind_pages;

	for (size_t page_index = current_page + 1; page_index <= last_ahead_page && page_index < pages.size(); ++page_index)
		ahead_pages.push_back(page_index);
	for (size_t page_index = current_page; page_index > first_behind_page; --page_index)
		behind_pages.push_back(page_index - 1);

	ApplyPinSetUnlocked(playback_critical_pages, critical_pages, PagePinPlaybackCritical);
	ApplyPinSetUnlocked(playback_ahead_pages, ahead_pages, PagePinPlaybackAhead);
	ApplyPinSetUnlocked(playback_behind_pages, behind_pages, PagePinPlaybackBehind);

	if (StorageBytesUnlocked() > kHardBudgetBytes)
		TrimStorageUnlocked(kHardBudgetBytes, kFreeBufferReservePages);

	worker_cv.notify_all();
}

void ExactPagedRAMAudioProvider::ClearPlaybackWindow() {
	std::lock_guard<std::mutex> lock(mutex);
	playback_active = false;
	ApplyPinSetUnlocked(playback_critical_pages, {}, PagePinPlaybackCritical);
	ApplyPinSetUnlocked(playback_ahead_pages, {}, PagePinPlaybackAhead);
	ApplyPinSetUnlocked(playback_behind_pages, {}, PagePinPlaybackBehind);
	if (StorageBytesUnlocked() > kSoftBudgetBytes)
		TrimStorageUnlocked(kSoftBudgetBytes, kFreeBufferReservePages);
	worker_cv.notify_all();
}

void ExactPagedRAMAudioProvider::HintVisibleRange(int64_t start_frame, int64_t frame_count) {
	if (num_samples <= 0 || frame_count <= 0)
		return;

	std::lock_guard<std::mutex> lock(mutex);
	MarkAccessUnlocked();

	start_frame = std::clamp<int64_t>(start_frame, 0, num_samples);
	auto end_frame = std::clamp<int64_t>(start_frame + frame_count, 0, num_samples);
	if (end_frame <= start_frame) {
		ClearViewportPinsUnlocked();
		worker_cv.notify_all();
		return;
	}

	auto const visible_frames = end_frame - start_frame;
	auto const padded_frames = std::min<int64_t>(viewport_max_frames, visible_frames + visible_frames);
	auto center_frame = start_frame + visible_frames / 2;
	auto request_start = std::max<int64_t>(0, center_frame - padded_frames / 2);
	auto request_end = std::min<int64_t>(num_samples, request_start + padded_frames);
	request_start = std::max<int64_t>(0, request_end - padded_frames);

	auto new_pages = BuildPageRange(request_start, request_end);
	ApplyPinSetUnlocked(viewport_pages, new_pages, PagePinViewport);
	worker_cv.notify_all();
}

void ExactPagedRAMAudioProvider::FillBuffer(void *buf, int64_t start, int64_t count) const {
	auto* output = static_cast<char *>(buf);

	while (count > 0) {
		auto const page_index = static_cast<size_t>(start / frames_per_page);
		auto const page_start = pages[page_index].start_frame;
		auto const page_offset_frames = start - page_start;
		auto const frames_from_page = std::min<int64_t>(count, pages[page_index].frame_count - page_offset_frames);
		auto const byte_offset = static_cast<size_t>(page_offset_frames) * frame_bytes;
		auto const byte_count = static_cast<size_t>(frames_from_page) * frame_bytes;

		auto page = PinPage(page_index);
		std::memcpy(output, page.data + byte_offset, byte_count);

		output += byte_count;
		start += frames_from_page;
		count -= frames_from_page;
	}
}
}

namespace agi {
std::unique_ptr<AudioProvider> CreateRAMAudioProvider(std::unique_ptr<AudioProvider> src) {
	return agi::make_unique<ExactPagedRAMAudioProvider>(std::move(src));
}
}
