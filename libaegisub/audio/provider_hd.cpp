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

#include <libaegisub/file_mapping.h>
#include <libaegisub/format.h>
#include <libaegisub/fs.h>
#include <libaegisub/path.h>
#include <libaegisub/make_unique.h>

#include <filesystem>
#include <boost/interprocess/detail/os_thread_functions.hpp>
#include <ctime>
#include <mutex>
#include <thread>
#include <vector>

namespace {
using namespace agi;

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

class HDAudioProvider final : public AudioProviderWrapper {
	static constexpr int64_t block_samples = 65536;
	mutable temp_file_mapping file;
	std::unique_ptr<std::atomic<bool>[]> failed_blocks;
	mutable std::mutex source_mutex;
	mutable std::mutex mapping_mutex;
	mutable std::vector<char> decode_buffer;
	std::jthread decoder;

	// The caller owns source_mutex; mapping views are only held while copying,
	// so reads of healthy blocks do not wait for source decoding or recovery.
	void CacheBlock(int64_t start, int64_t count) const {
		source->GetAudioChecked(decode_buffer.data(), start, count);
		auto const frame_bytes = static_cast<int64_t>(bytes_per_sample) * channels;
		std::scoped_lock lock(mapping_mutex);
		memcpy(file.write(start * frame_bytes, count * frame_bytes),
			   decode_buffer.data(), count * frame_bytes);
	}

	void FillBuffer(void *buf, int64_t start, int64_t count) const override {
		auto const processed = decoded_samples.load();
		if (start >= processed || count > processed - start)
			throw AudioDecodeError("HD audio cache has not decoded the requested samples yet");

		auto *output = static_cast<char *>(buf);
		auto const frame_bytes = static_cast<int64_t>(bytes_per_sample) * channels;
		while (count > 0) {
			auto const index = static_cast<size_t>(start / block_samples);
			if (failed_blocks[index].load(std::memory_order_acquire)) {
				std::scoped_lock lock(source_mutex);
				if (failed_blocks[index].load(std::memory_order_relaxed)) {
					auto const block_start = static_cast<int64_t>(index) * block_samples;
					CacheBlock(block_start, std::min(block_samples, num_samples - block_start));
					failed_blocks[index].store(false, std::memory_order_release);
				}
			}
			auto const frames = std::min(count, block_samples - start % block_samples);
			auto const bytes = frames * frame_bytes;
			{
				std::scoped_lock lock(mapping_mutex);
				memcpy(output, file.read(start * frame_bytes, bytes), bytes);
			}
			output += bytes;
			start += frames;
			count -= frames;
		}
	}

	fs::path CacheFilename(fs::path const& dir) {
		// Check free space
		if ((uint64_t)num_samples * bytes_per_sample * channels > fs::FreeSpace(dir))
			throw AudioProviderError("Not enough free disk space in " + fs::PathToString(dir) + " to cache the audio");

		return format("audio-%lld-%lld", time(nullptr),
		              boost::interprocess::ipcdetail::get_current_process_id());
	}

public:
	HDAudioProvider(std::unique_ptr<AudioProvider> src, agi::fs::path const& dir)
	: AudioProviderWrapper(std::move(src))
	, file(dir / CacheFilename(dir), num_samples * bytes_per_sample * channels)
	, failed_blocks(std::make_unique<std::atomic<bool>[]>(
		static_cast<size_t>(num_samples / block_samples + (num_samples % block_samples != 0))))
	, decode_buffer(static_cast<size_t>(block_samples) * bytes_per_sample * channels)
	{
		decoded_samples = 0;
		decoder = std::jthread([this](std::stop_token stop_token) {
			for (int64_t i = 0; i < num_samples; i += block_samples) {
				if (stop_token.stop_requested()) break;
				auto const block = std::min(block_samples, num_samples - i);
				{
					std::scoped_lock lock(source_mutex);
					try {
						CacheBlock(i, block);
					}
					catch (...) {
						failed_blocks[i / block_samples].store(true, std::memory_order_release);
					}
				}
				// A processed but failed block remains unavailable until a later
				// checked read successfully replaces its complete contents.
				decoded_samples += block;
			}
		});
	}
	~HDAudioProvider() = default;
	AudioProviderMemoryStats GetMemoryStats() const override {
		return BuildMemoryStats(
			FormatWrappedProviderName("HD", source.get()),
			"disk",
			static_cast<size_t>(num_samples)
				* static_cast<size_t>(bytes_per_sample)
				* static_cast<size_t>(channels));
	}
};
}

namespace agi {
std::unique_ptr<AudioProvider> CreateHDAudioProvider(std::unique_ptr<AudioProvider> src, agi::fs::path const& dir) {
	return agi::make_unique<HDAudioProvider>(std::move(src), dir);
}
}
