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
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF THE USE OR
// PERFORMANCE OF THIS SOFTWARE.

#include "async_video_provider_host.h"

#include <utility>

AsyncVideoProviderEventSink CreateAsyncVideoProviderMainThreadSink(
	agi::ui::WeakLifetime event_lifetime,
	AsyncVideoProviderEventSink sink) {
	AsyncVideoProviderEventSink main_thread_sink;

	if (sink.on_frame_ready) {
		main_thread_sink.on_frame_ready =
			[event_lifetime, callback = std::move(sink.on_frame_ready)](VideoRenderPacket packet, double time) mutable {
				agi::ui::MainAsyncIfAlive(event_lifetime, [callback, packet = std::move(packet), time]() mutable {
					callback(std::move(packet), time);
				});
			};
	}

	if (sink.on_video_error) {
		main_thread_sink.on_video_error =
			[event_lifetime, callback = std::move(sink.on_video_error)](std::string const& message) mutable {
				agi::ui::MainAsyncIfAlive(event_lifetime, [callback, message]() mutable {
					callback(message);
				});
			};
	}

	if (sink.on_subtitles_error) {
		main_thread_sink.on_subtitles_error =
			[event_lifetime, callback = std::move(sink.on_subtitles_error)](std::string const& message) mutable {
				agi::ui::MainAsyncIfAlive(event_lifetime, [callback, message]() mutable {
					callback(message);
				});
			};
	}

	return main_thread_sink;
}
