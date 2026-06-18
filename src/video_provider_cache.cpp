// Copyright (c) 2013, Thomas Goyne <plorkyeran@aegisub.org>
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

#include "include/aegisub/video_provider.h"

#include "options.h"
#include "video_memory_stats.h"
#include "video_frame.h"

#include <libaegisub/make_unique.h>

#include <algorithm>
#include <limits>
#include <list>
#include <unordered_map>

namespace {
int GetConfiguredVideoCacheSizeMb() {
	return config::GetIntOptionOrDefault("Provider/Video/Cache/Size", 128);
}

enum class CachedFrameKind {
	Bgra,
	Native
};

struct CachedFrameKey {
	int frame_number = -1;
	CachedFrameKind kind = CachedFrameKind::Bgra;
};

bool operator==(CachedFrameKey const& lhs, CachedFrameKey const& rhs) {
	return lhs.frame_number == rhs.frame_number && lhs.kind == rhs.kind;
}

struct CachedFrameKeyHash {
	size_t operator()(CachedFrameKey const& key) const noexcept {
		return (static_cast<size_t>(static_cast<unsigned int>(key.frame_number)) << 1)
			^ static_cast<size_t>(key.kind);
	}
};

struct CachedFrame {
	CachedFrameKey key;
	size_t size_bytes = 0;
	VideoFrame frame;
	SourceFrame native_frame;
	std::shared_ptr<void> native_owner;

	CachedFrame() = default;
	CachedFrame(CachedFrame const&) = delete;
	CachedFrame(CachedFrame&&) = default;
	CachedFrame& operator=(CachedFrame&&) = default;
};

size_t EstimateNativeFrameSize(SourceFrame const& frame) {
	size_t total_size = 0;
	for (int i = 0; i < frame.plane_count; ++i) {
		auto const& plane = frame.planes[static_cast<size_t>(i)];
		ptrdiff_t stride = plane.stride < 0 ? -plane.stride : plane.stride;
		total_size += static_cast<size_t>(stride) * static_cast<size_t>(plane.height);
	}
	return total_size;
}

size_t ConfiguredCacheSizeBytes() {
	int const cache_size_mb = GetConfiguredVideoCacheSizeMb();
	if (cache_size_mb <= 0)
		return 0;

	size_t const mb = static_cast<size_t>(cache_size_mb);
	constexpr size_t bytes_per_mb = 1u << 20;
	if (mb > std::numeric_limits<size_t>::max() / bytes_per_mb)
		return std::numeric_limits<size_t>::max();
	return mb * bytes_per_mb;
}

size_t EstimateBgraFrameSize(VideoProvider const& provider) {
	int const width = provider.GetWidth();
	int const height = provider.GetHeight();
	if (width <= 0 || height <= 0)
		return 0;

	size_t const w = static_cast<size_t>(width);
	size_t const h = static_cast<size_t>(height);
	constexpr size_t bytes_per_pixel = 4;
	if (w > std::numeric_limits<size_t>::max() / h)
		return 0;
	size_t const pixels = w * h;
	if (pixels > std::numeric_limits<size_t>::max() / bytes_per_pixel)
		return 0;
	return pixels * bytes_per_pixel;
}

size_t EffectiveCacheSizeBytes(VideoProvider const& provider, size_t configured_size) {
	if (configured_size == 0)
		return 0;

	constexpr size_t desired_recent_frames = 6;
	size_t const frame_size = EstimateBgraFrameSize(provider);
	if (frame_size == 0 || frame_size > std::numeric_limits<size_t>::max() / desired_recent_frames)
		return configured_size;

	return std::max(configured_size, frame_size * desired_recent_frames);
}

/// @class VideoProviderCache
/// @brief A wrapper around a video provider which provides LRU caching
class VideoProviderCache final : public VideoProvider {
	/// The source provider to get frames from
	std::unique_ptr<VideoProvider> master;

	/// @brief Maximum size of the cache in bytes
	///
	/// Note that this is a soft limit. The cache stops allocating new frames
	/// once it has exceeded the limit, but it never tries to shrink
	const size_t max_cache_size;

	/// Cache of video/native frames with the most recently used ones at the front.
	std::list<CachedFrame> cache;
	std::unordered_map<CachedFrameKey, std::list<CachedFrame>::iterator, CachedFrameKeyHash> cache_index;
	size_t total_cache_size = 0;

	void ClearCache() {
		cache_index.clear();
		cache.clear();
		total_cache_size = 0;
	}

	std::list<CachedFrame>::iterator TouchCachedFrame(CachedFrameKey const& key) {
		auto it = cache_index.find(key);
		if (it == cache_index.end())
			return cache.end();

		cache.splice(cache.begin(), cache, it->second);
		return cache.begin();
	}

	template<typename Fill>
	void StoreCachedFrame(CachedFrameKey const& key, size_t size_bytes, Fill&& fill) {
		if (cache.empty() || total_cache_size < max_cache_size) {
			cache.emplace_front();
			auto it = cache.begin();
			fill(*it);
			it->key = key;
			it->size_bytes = size_bytes;
			cache_index[key] = it;
			total_cache_size += size_bytes;
			return;
		}

		auto it = std::prev(cache.end());
		cache_index.erase(it->key);
		total_cache_size -= it->size_bytes;
		cache.splice(cache.begin(), cache, it);

		fill(*cache.begin());
		cache.begin()->key = key;
		cache.begin()->size_bytes = size_bytes;
		cache_index[key] = cache.begin();
		total_cache_size += size_bytes;
	}

public:
	VideoProviderCache(std::unique_ptr<VideoProvider> master)
	: master(std::move(master))
	, max_cache_size(EffectiveCacheSizeBytes(*this->master, ConfiguredCacheSizeBytes())) {
	}

	VideoProviderCache(std::unique_ptr<VideoProvider> master, size_t max_cache_size)
	: master(std::move(master))
	, max_cache_size(max_cache_size) {
	}

	void GetFrame(int n, VideoFrame &frame) override;
	bool GetNativeFrame(int n, SourceFrame& frame, std::shared_ptr<void>& owner) override;

	void SetColorSpace(std::string const& m) override {
		ClearCache();
		return master->SetColorSpace(m);
	}

	int GetFrameCount() const override             { return master->GetFrameCount(); }
	int GetWidth() const override                  { return master->GetWidth(); }
	int GetHeight() const override                 { return master->GetHeight(); }
	double GetDAR() const override                 { return master->GetDAR(); }
	agi::vfr::Framerate GetFPS() const override    { return master->GetFPS(); }
	std::vector<int> GetKeyFrames() const override { return master->GetKeyFrames(); }
	std::string GetWarning() const override        { return master->GetWarning(); }
	std::string GetDecoderName() const override    { return master->GetDecoderName(); }
	std::string GetColorSpace() const override     { return master->GetColorSpace(); }
	std::string GetRealColorSpace() const override { return master->GetRealColorSpace(); }
	SourceFrameColorMetadata GetColorMetadata() const override { return master->GetColorMetadata(); }
	SourceFrameColorMetadata GetRealColorMetadata() const override { return master->GetRealColorMetadata(); }
	SourceFrameGeometry GetFrameGeometry() const override { return master->GetFrameGeometry(); }
	SourceFrameNativeFormatIdentity GetNativeFormatIdentity() const override { return master->GetNativeFormatIdentity(); }
	std::string GetNativeFormatDescription() const override { return master->GetNativeFormatDescription(); }
	std::vector<SourceFrameOutputMode> GetAvailableSourceModes() const override { return master->GetAvailableSourceModes(); }
	bool SetOutputMode(SourceFrameOutputMode mode) override {
		ClearCache();
		return master->SetOutputMode(mode);
	}
	bool ShouldSetVideoProperties() const override { return master->ShouldSetVideoProperties(); }
	bool HasAudio() const override                 { return master->HasAudio(); }
	VideoProviderMemoryStats GetMemoryStats() const override;
};

void VideoProviderCache::GetFrame(int n, VideoFrame &out) {
	if (auto it = TouchCachedFrame({ n, CachedFrameKind::Bgra }); it != cache.end()) {
		out = it->frame;
		return;
	}

	master->GetFrame(n, out);

	if (max_cache_size == 0)
		return;

	size_t const frame_size = out.data.size();

	StoreCachedFrame(
		{ n, CachedFrameKind::Bgra },
		frame_size,
		[&](CachedFrame& cached) {
			cached.frame = out;
			cached.native_frame = { };
			cached.native_owner.reset();
		});
}

bool VideoProviderCache::GetNativeFrame(int n, SourceFrame& out, std::shared_ptr<void>& owner) {
	if (auto it = TouchCachedFrame({ n, CachedFrameKind::Native }); it != cache.end()) {
		out = it->native_frame;
		owner = it->native_owner;
		return true;
	}

	if (!master->GetNativeFrame(n, out, owner))
		return false;

	// Providers which do not return an owner may expose transient frame memory.
	if (max_cache_size == 0 || !owner || !out.IsValid())
		return true;

	size_t const frame_size = EstimateNativeFrameSize(out);

	StoreCachedFrame(
		{ n, CachedFrameKind::Native },
		frame_size,
		[&](CachedFrame& cached) {
			cached.frame = { };
			cached.native_frame = out;
			cached.native_owner = owner;
		});
	return true;
}

VideoProviderMemoryStats VideoProviderCache::GetMemoryStats() const {
	VideoProviderMemoryStats stats = master->GetMemoryStats();
	for (auto const& cached : cache) {
		stats.cache_total_bytes += cached.size_bytes;
		if (cached.key.kind == CachedFrameKind::Native) {
			stats.cache_native_bytes += cached.size_bytes;
			++stats.cache_native_frames;
		}
		else {
			stats.cache_bgra_bytes += cached.size_bytes;
			++stats.cache_bgra_frames;
		}
	}
	return stats;
}
}

std::unique_ptr<VideoProvider> CreateCacheVideoProvider(std::unique_ptr<VideoProvider> parent) {
	return agi::make_unique<VideoProviderCache>(std::move(parent));
}

std::unique_ptr<VideoProvider> CreateCacheVideoProvider(std::unique_ptr<VideoProvider> parent, size_t max_cache_size_bytes) {
	return agi::make_unique<VideoProviderCache>(std::move(parent), max_cache_size_bytes);
}
