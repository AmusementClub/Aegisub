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
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace {
using namespace agi;

constexpr size_t kMinPageBytes = 128 * 1024;
constexpr size_t kTargetPageBytes = 256 * 1024;
constexpr size_t kMaxPageBytes = 1024 * 1024;
constexpr size_t kSoftBudgetBytes = 128 * 1024 * 1024;
constexpr size_t kHardBudgetBytes = 160 * 1024 * 1024;

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

struct AudioPageEntry {
	int64_t start_frame = 0;
	int64_t frame_count = 0;
	size_t byte_size = 0;
	PageState state = PageState::Empty;
	std::vector<char> bytes;
	uint64_t last_access_seq = 0;
	uint32_t active_pins = 0;
};

class ExactPagedRAMAudioProvider final : public AudioProviderWrapper {
	struct PageReadPin final {
		ExactPagedRAMAudioProvider const* owner = nullptr;
		size_t page_index = 0;
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

	size_t const frame_bytes;
	size_t const page_bytes;
	int64_t const frames_per_page;

	mutable std::mutex mutex;
	mutable std::condition_variable page_cv;
	mutable std::mutex source_mutex;
	mutable std::vector<AudioPageEntry> pages;
	mutable uint64_t access_seq = 0;
	mutable size_t resident_bytes = 0;
	mutable int64_t resident_frames = 0;

	void FillBuffer(void *buf, int64_t start, int64_t count) const override;

	void TrimToBudget(size_t target_bytes) const;
	void TrimToBudgetUnlocked(size_t target_bytes) const;
	void EnsureBudgetForLoad(size_t next_page_bytes) const;
	PageReadPin PinPage(size_t page_index) const;
	void UnpinPage(size_t page_index) const;

public:
	ExactPagedRAMAudioProvider(std::unique_ptr<AudioProvider> src);

	AudioProviderMemoryStats GetMemoryStats() const override;
};

ExactPagedRAMAudioProvider::ExactPagedRAMAudioProvider(std::unique_ptr<AudioProvider> src)
: AudioProviderWrapper(std::move(src))
, frame_bytes(static_cast<size_t>(std::max(1, channels)) * static_cast<size_t>(std::max(1, bytes_per_sample)))
, page_bytes(ChoosePageBytes(frame_bytes))
, frames_per_page(std::max<int64_t>(1, static_cast<int64_t>(page_bytes / frame_bytes))) {
	// On-demand paging can service any sample range immediately, so treat the
	// provider as fully readable rather than progressively decoded.
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
}

AudioProviderMemoryStats ExactPagedRAMAudioProvider::GetMemoryStats() const {
	std::lock_guard<std::mutex> lock(mutex);

	AudioProviderMemoryStats stats;
	stats.provider_name = "RAM Paged";
	stats.storage_kind = "memory";
	stats.storage_bytes = resident_bytes;
	stats.logical_bytes = static_cast<size_t>(num_samples)
		* static_cast<size_t>(bytes_per_sample)
		* static_cast<size_t>(channels);
	stats.decoded_bytes = resident_bytes;
	stats.num_samples = num_samples;
	stats.decoded_samples = resident_frames;
	stats.sample_rate = sample_rate;
	stats.bytes_per_sample = bytes_per_sample;
	stats.channels = channels;
	stats.float_samples = float_samples;
	return stats;
}

void ExactPagedRAMAudioProvider::TrimToBudgetUnlocked(size_t target_bytes) const {
	while (resident_bytes > target_bytes) {
		size_t victim = std::numeric_limits<size_t>::max();
		uint64_t oldest_access = std::numeric_limits<uint64_t>::max();

		for (size_t i = 0; i < pages.size(); ++i) {
			auto const& page = pages[i];
			if (page.state != PageState::Ready || page.active_pins != 0 || page.bytes.empty())
				continue;
			if (page.last_access_seq < oldest_access) {
				oldest_access = page.last_access_seq;
				victim = i;
			}
		}

		if (victim == std::numeric_limits<size_t>::max())
			return;

		auto& page = pages[victim];
		resident_bytes -= page.byte_size;
		resident_frames -= page.frame_count;
		page.state = PageState::Empty;
		page.last_access_seq = 0;
		std::vector<char>().swap(page.bytes);
	}
}

void ExactPagedRAMAudioProvider::TrimToBudget(size_t target_bytes) const {
	std::lock_guard<std::mutex> lock(mutex);
	TrimToBudgetUnlocked(target_bytes);
}

void ExactPagedRAMAudioProvider::EnsureBudgetForLoad(size_t next_page_bytes) const {
	std::lock_guard<std::mutex> lock(mutex);
	if (resident_bytes + next_page_bytes <= kHardBudgetBytes)
		return;

	size_t const target = kSoftBudgetBytes > next_page_bytes ? kSoftBudgetBytes - next_page_bytes : 0;
	TrimToBudgetUnlocked(target);
}

ExactPagedRAMAudioProvider::PageReadPin ExactPagedRAMAudioProvider::PinPage(size_t page_index) const {
	for (;;) {
		int64_t start_frame = 0;
		int64_t frame_count = 0;
		size_t byte_size = 0;
		{
			std::unique_lock<std::mutex> lock(mutex);
			auto& page = pages[page_index];
			if (page.state == PageState::Ready) {
				++page.active_pins;
				page.last_access_seq = ++access_seq;
				return PageReadPin(this, page_index, page.bytes.data());
			}

			if (page.state == PageState::Loading) {
				page_cv.wait(lock, [&] { return pages[page_index].state != PageState::Loading; });
				continue;
			}

			if (page.state == PageState::Failed)
				throw AudioDecodeError("Failed to load a paged RAM audio block");

			page.state = PageState::Loading;
			start_frame = page.start_frame;
			frame_count = page.frame_count;
			byte_size = page.byte_size;
		}

		EnsureBudgetForLoad(byte_size);

		std::vector<char> bytes;
		try {
			bytes.resize(byte_size);
		}
		catch (std::bad_alloc const&) {
			std::lock_guard<std::mutex> lock(mutex);
			auto& page = pages[page_index];
			page.state = PageState::Failed;
			page_cv.notify_all();
			throw AudioDecodeError("Not enough memory available to cache audio page");
		}

		{
			std::lock_guard<std::mutex> source_lock(source_mutex);
			source->GetAudio(bytes.data(), start_frame, frame_count);
		}

		{
			std::lock_guard<std::mutex> lock(mutex);
			auto& page = pages[page_index];
			page.bytes = std::move(bytes);
			page.state = PageState::Ready;
			resident_bytes += page.byte_size;
			resident_frames += page.frame_count;
			++page.active_pins;
			page.last_access_seq = ++access_seq;
			page_cv.notify_all();
			return PageReadPin(this, page_index, page.bytes.data());
		}
	}
}

void ExactPagedRAMAudioProvider::UnpinPage(size_t page_index) const {
	bool trim = false;
	{
		std::lock_guard<std::mutex> lock(mutex);
		auto& page = pages[page_index];
		if (page.active_pins > 0)
			--page.active_pins;
		trim = resident_bytes > kSoftBudgetBytes;
	}

	if (trim)
		TrimToBudget(kSoftBudgetBytes);
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
