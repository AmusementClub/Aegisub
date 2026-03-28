#pragma once

#include <functional>
#include <memory>
#include <string>

class wxEvtHandler;

class VideoControllerErrorHost {
public:
	virtual ~VideoControllerErrorHost() = default;
};

std::unique_ptr<VideoControllerErrorHost> CreateVideoControllerErrorHost(
	wxEvtHandler *event_source,
	std::function<void(std::string const&)> on_video_error,
	std::function<void(std::string const&)> on_subtitles_error);
