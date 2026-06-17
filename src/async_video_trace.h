#pragma once

namespace aegisub::async_video_trace {

class Sink {
public:
	virtual ~Sink() = default;
	virtual void ObserveFrameResult(int frame, double time, bool delivered, bool immediate) = 0;
	virtual void ObserveVideoFrameRenderDuration(int frame, double time, bool delivered, bool immediate, double duration_ms) = 0;
};

void SetSink(Sink *sink);
void ObserveFrameResult(int frame, double time, bool delivered, bool immediate);
void ObserveVideoFrameRenderDuration(int frame, double time, bool delivered, bool immediate, double duration_ms);

}

