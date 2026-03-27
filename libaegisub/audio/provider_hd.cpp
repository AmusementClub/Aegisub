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
#include <thread>

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
	mutable temp_file_mapping file;
	std::jthread decoder;

	void FillBuffer(void *buf, int64_t start, int64_t count) const override {
		auto missing = std::min(count, start + count - decoded_samples);
		if (missing > 0) {
			memset(static_cast<int16_t*>(buf) + count - missing, 0, missing * bytes_per_sample * channels);
			count -= missing;
		}

		if (count > 0) {
			start *= bytes_per_sample * channels;
			count *= bytes_per_sample * channels;
			memcpy(buf, file.read(start, count), count);
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
	, file(dir / CacheFilename(dir), num_samples * bytes_per_sample* channels)
	{
		decoded_samples = 0;
		decoder = std::jthread([this](std::stop_token stop_token) {
			int64_t block = 65536;
			for (int64_t i = 0; i < num_samples; i += block) {
				if (stop_token.stop_requested()) break;
				block = std::min(block, num_samples - i);
				source->GetAudio(file.write(i * bytes_per_sample * channels, block * bytes_per_sample * channels), i, block);
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
