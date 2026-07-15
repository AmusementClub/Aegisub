#pragma once

#include "skia/skia_gl_device.h"

#include <cstdint>
#include <string>

class SkiaVideoCompositor final {
	SkiaVideoFailureInjection failure_injection = SkiaVideoFailureInjection::None;
	SkiaGlDevice device;
	bool frame_open = false;
	bool failure_logged = false;
	SkiaGlContextToken active_context;
	SkiaGlContextToken last_context;
	std::uint64_t last_present_generation = 0;

public:
	explicit SkiaVideoCompositor(SkiaVideoFailureInjection failure_injection);
	~SkiaVideoCompositor();

	SkiaVideoCompositor(SkiaVideoCompositor const&) = delete;
	SkiaVideoCompositor& operator=(SkiaVideoCompositor const&) = delete;

	bool BeginFrame(SkiaGlContextToken context, SkiaVideoFrameTarget const& target);
	bool FinishFrame(SkiaGlContextToken context, bool submit);
	bool ProbeFrame(SkiaGlContextToken context, SkiaVideoFrameTarget const& target);
	void FailFrame(SkiaGlContextToken context, SkiaGlDeviceFailure failure, std::string detail) noexcept;
	void NotifyContextActivationFailure(SkiaGlContextToken context) noexcept;
	void Release(SkiaGlContextToken context) noexcept;

	SkiaGlDevice& Device() noexcept { return device; }
	SkiaGlDevice const& Device() const noexcept { return device; }
	std::string TakeFailureLogMessage();
};
