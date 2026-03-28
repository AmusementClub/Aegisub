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

#include "video_controller_error_host.h"

#include "async_video_provider.h"

#include <utility>

namespace {

class NoopVideoControllerErrorHost final : public VideoControllerErrorHost {
};

class WxVideoControllerErrorHost final : public VideoControllerErrorHost, public wxEvtHandler {
	wxEvtHandler *event_source = nullptr;
	std::function<void(std::string const&)> on_video_error;
	std::function<void(std::string const&)> on_subtitles_error;

	void HandleVideoError(VideoProviderErrorEvent &event) {
		if (on_video_error)
			on_video_error(event.GetMessage());
	}

	void HandleSubtitlesError(SubtitlesProviderErrorEvent &event) {
		if (on_subtitles_error)
			on_subtitles_error(event.GetMessage());
	}

public:
	WxVideoControllerErrorHost(
		wxEvtHandler *event_source,
		std::function<void(std::string const&)> on_video_error,
		std::function<void(std::string const&)> on_subtitles_error)
	: event_source(event_source)
	, on_video_error(std::move(on_video_error))
	, on_subtitles_error(std::move(on_subtitles_error)) {
		if (!this->event_source)
			return;
		this->event_source->Bind(EVT_VIDEO_ERROR, &WxVideoControllerErrorHost::HandleVideoError, this);
		this->event_source->Bind(EVT_SUBTITLES_ERROR, &WxVideoControllerErrorHost::HandleSubtitlesError, this);
	}

	~WxVideoControllerErrorHost() override {
		if (!event_source)
			return;
		event_source->Unbind(EVT_VIDEO_ERROR, &WxVideoControllerErrorHost::HandleVideoError, this);
		event_source->Unbind(EVT_SUBTITLES_ERROR, &WxVideoControllerErrorHost::HandleSubtitlesError, this);
	}
};

}

std::unique_ptr<VideoControllerErrorHost> CreateVideoControllerErrorHost(
	wxEvtHandler *event_source,
	std::function<void(std::string const&)> on_video_error,
	std::function<void(std::string const&)> on_subtitles_error) {
	if (!event_source)
		return std::make_unique<NoopVideoControllerErrorHost>();
	return std::make_unique<WxVideoControllerErrorHost>(
		event_source,
		std::move(on_video_error),
		std::move(on_subtitles_error));
}
