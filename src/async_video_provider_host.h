#pragma once

#include "ui_dispatch.h"
#include "video_render_packet.h"

#include <functional>
#include <string>

struct AsyncVideoProviderEventSink {
	std::function<void(VideoRenderPacket, double)> on_frame_ready;
	std::function<void(std::string const&)> on_video_error;
	std::function<void(std::string const&)> on_subtitles_error;
};

enum class AsyncVideoFrameDeliveryMode {
	EveryFrame,
	VisualSubtitleBatches
};

AsyncVideoProviderEventSink CreateAsyncVideoProviderMainThreadSink(
	agi::ui::WeakLifetime event_lifetime,
	AsyncVideoProviderEventSink sink,
	AsyncVideoFrameDeliveryMode frame_delivery_mode = AsyncVideoFrameDeliveryMode::EveryFrame);
