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

#include "video_render_packet.h"

enum class VideoRenderRoutingMode {
	SourceFrameOnly,
	PrimaryRendererDirectOverlay,
	SecondaryRendererDirectOverlay,
	FallbackCompositedFrame
};

inline VideoRenderRoutingMode DecideVideoRenderRouting(
	VideoRenderPacket const& packet,
	bool primary_renderer_supports_direct_overlay) {
	if (!packet.has_subtitle_overlay) {
		if (packet.HasDistinctCompositedFrame())
			return VideoRenderRoutingMode::FallbackCompositedFrame;
		return VideoRenderRoutingMode::SourceFrameOnly;
	}

	if (!packet.subtitle_overlay.IsValid() || !packet.subtitle_overlay.IsDirectRenderable())
		return VideoRenderRoutingMode::FallbackCompositedFrame;

	return primary_renderer_supports_direct_overlay
		? VideoRenderRoutingMode::PrimaryRendererDirectOverlay
		: VideoRenderRoutingMode::SecondaryRendererDirectOverlay;
}
