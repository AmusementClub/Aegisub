// Copyright (c) 2026
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

#pragma once

#include "include/aegisub/video_provider.h"
#include "subtitle_overlay.h"
#include "video_render_packet.h"

#include <libaegisub/audio/provider.h>

#include <cstddef>
#include <string>

inline size_t EstimateVideoFrameStorageBytes(VideoFrame const& frame) {
	return frame.data.capacity();
}

inline size_t EstimateSourceFrameReferencedBytes(SourceFrame const& frame) {
	size_t total_size = 0;
	for (int i = 0; i < frame.plane_count; ++i) {
		auto const& plane = frame.planes[static_cast<size_t>(i)];
		ptrdiff_t stride = plane.stride < 0 ? -plane.stride : plane.stride;
		total_size += static_cast<size_t>(stride) * static_cast<size_t>(plane.height);
	}
	return total_size;
}

inline size_t EstimateSubtitleOverlayStorageBytes(SubtitleOverlayStorage const& storage) {
	return storage.pixels.capacity()
		+ storage.dirty_rects.capacity() * sizeof(SubtitleOverlayDirtyRect)
		+ storage.row_ranges.capacity() * sizeof(SubtitleOverlayRowRange);
}

inline size_t EstimateVideoRenderPacketReferencedBytes(VideoRenderPacket const& packet) {
	size_t total_size = 0;
	auto const* source_storage = packet.source_frame_storage.get();
	auto const* composited_storage = packet.composited_frame_storage.get();

	if (source_storage)
		total_size += EstimateVideoFrameStorageBytes(*source_storage);
	else
		total_size += EstimateSourceFrameReferencedBytes(packet.source_frame);

	if (composited_storage && composited_storage != source_storage)
		total_size += EstimateVideoFrameStorageBytes(*composited_storage);

	if (packet.subtitle_overlay_storage)
		total_size += EstimateSubtitleOverlayStorageBytes(*packet.subtitle_overlay_storage);

	return total_size;
}

struct AsyncVideoProviderMemoryStats {
	VideoProviderMemoryStats provider;
	size_t source_pool_bytes = 0;
	size_t composited_pool_bytes = 0;
	size_t subtitle_overlay_pool_bytes = 0;
	int source_pool_buffers = 0;
	int composited_pool_buffers = 0;
	int subtitle_overlay_pool_buffers = 0;
	SourceFrameOutputMode selected_source_mode = SourceFrameOutputMode::Bgra8;
	std::string decoder_name;
	std::string subtitles_provider_name;
	std::string subtitles_render_mode;
	bool compatibility_requires_bgra8 = false;
	bool subtitles_loaded = false;
	bool pending_subtitles_update = false;
	int subtitles_event_count = 0;
};

struct VideoDisplayMemoryStats {
	size_t pending_packet_ref_bytes = 0;
	size_t displayed_packet_ref_bytes = 0;
	size_t primary_renderer_texture_bytes = 0;
	size_t secondary_renderer_texture_bytes = 0;
	std::string primary_renderer_name;
	std::string secondary_renderer_name;
};

struct VideoMemorySnapshot {
	AsyncVideoProviderMemoryStats async;
	VideoDisplayMemoryStats display;
	agi::AudioProviderMemoryStats audio;
	size_t process_working_set_bytes = 0;
	size_t process_private_bytes = 0;
};
