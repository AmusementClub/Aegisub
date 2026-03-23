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
#include "video_frame.h"

#include <libaegisub/make_unique.h>

#include <list>
#include <unordered_map>

namespace {
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

struct StepWarmState {
	CachedFrameKey last_key;
	bool has_last_request = false;
	int last_delta = 0;
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
	StepWarmState step_warm;

	void ClearCache() {
		cache_index.clear();
		cache.clear();
		total_cache_size = 0;
		step_warm = { };
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

	bool CanWarmNeighbor(size_t frame_size_bytes) const {
		return max_cache_size != 0
			&& frame_size_bytes != 0
			&& frame_size_bytes <= max_cache_size / 2;
	}

	int UpdateStepWarmState(CachedFrameKey const& key) {
		int warm_delta = 0;
		if (step_warm.has_last_request && step_warm.last_key.kind == key.kind) {
			int delta = key.frame_number - step_warm.last_key.frame_number;
			if (delta != 0) {
				if (step_warm.last_delta == 0 || step_warm.last_delta == delta)
					warm_delta = delta;
				step_warm.last_delta = delta;
			}
			else {
				step_warm.last_delta = 0;
			}
		}
		else {
			step_warm.last_delta = 0;
		}

		step_warm.last_key = key;
		step_warm.has_last_request = true;
		return warm_delta;
	}

	void WarmBgraNeighbor(CachedFrameKey const& key, size_t frame_size_bytes) {
		int const delta = UpdateStepWarmState(key);
		if (delta == 0 || !CanWarmNeighbor(frame_size_bytes))
			return;

		int const target_frame = key.frame_number + delta;
		if (target_frame < 0 || target_frame >= GetFrameCount())
			return;

		CachedFrameKey const target_key = { target_frame, CachedFrameKind::Bgra };
		if (cache_index.find(target_key) != cache_index.end())
			return;

		try {
			VideoFrame warmed;
			master->GetFrame(target_frame, warmed);
			StoreCachedFrame(
				target_key,
				warmed.data.size(),
				[&](CachedFrame& cached) {
					cached.frame = warmed;
					cached.native_frame = { };
					cached.native_owner.reset();
				});
		}
		catch (VideoProviderError const&) {
		}
	}

	void WarmNativeNeighbor(CachedFrameKey const& key, size_t frame_size_bytes) {
		int const delta = UpdateStepWarmState(key);
		if (delta == 0 || !CanWarmNeighbor(frame_size_bytes))
			return;

		int const target_frame = key.frame_number + delta;
		if (target_frame < 0 || target_frame >= GetFrameCount())
			return;

		CachedFrameKey const target_key = { target_frame, CachedFrameKind::Native };
		if (cache_index.find(target_key) != cache_index.end())
			return;

		try {
			SourceFrame warmed;
			std::shared_ptr<void> warmed_owner;
			if (!master->GetNativeFrame(target_frame, warmed, warmed_owner))
				return;
			if (!warmed_owner || !warmed.IsValid())
				return;

			StoreCachedFrame(
				target_key,
				EstimateNativeFrameSize(warmed),
				[&](CachedFrame& cached) {
					cached.frame = { };
					cached.native_frame = warmed;
					cached.native_owner = warmed_owner;
				});
		}
		catch (VideoProviderError const&) {
		}
	}

public:
	VideoProviderCache(std::unique_ptr<VideoProvider> master)
	: master(std::move(master))
	, max_cache_size(OPT_GET("Provider/Video/Cache/Size")->GetInt() << 20) {
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
};

void VideoProviderCache::GetFrame(int n, VideoFrame &out) {
	if (auto it = TouchCachedFrame({ n, CachedFrameKind::Bgra }); it != cache.end()) {
		out = it->frame;
		WarmBgraNeighbor(it->key, it->size_bytes);
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
	WarmBgraNeighbor({ n, CachedFrameKind::Bgra }, frame_size);
}

bool VideoProviderCache::GetNativeFrame(int n, SourceFrame& out, std::shared_ptr<void>& owner) {
	if (auto it = TouchCachedFrame({ n, CachedFrameKind::Native }); it != cache.end()) {
		out = it->native_frame;
		owner = it->native_owner;
		WarmNativeNeighbor(it->key, it->size_bytes);
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
	WarmNativeNeighbor({ n, CachedFrameKind::Native }, frame_size);
	return true;
}
}

std::unique_ptr<VideoProvider> CreateCacheVideoProvider(std::unique_ptr<VideoProvider> parent) {
	return agi::make_unique<VideoProviderCache>(std::move(parent));
}

std::unique_ptr<VideoProvider> CreateCacheVideoProvider(std::unique_ptr<VideoProvider> parent, size_t max_cache_size_bytes) {
	return agi::make_unique<VideoProviderCache>(std::move(parent), max_cache_size_bytes);
}
