#pragma once

#include "skia_gl_contract.h"

#include <cstdint>
#include <string>
#include <string_view>

enum class SkiaVideoFailureInjection {
	None,
	ContextInitialization,
	FrameBegin,
	FlushSubmit,
	Unsupported,
};

SkiaVideoFailureInjection ParseSkiaVideoFailureInjection(std::string_view value) noexcept;
char const *ToString(SkiaVideoFailureInjection injection) noexcept;

enum class SkiaVideoTargetOrigin {
	Unknown,
	BottomLeft,
	TopLeft,
};

enum class SkiaVideoTargetPixelFormat {
	Unknown,
	Rgba8,
};

enum class SkiaVideoTargetColorSpace {
	Unknown,
	SdrPreview,
};

struct SkiaVideoTargetViewport {
	int x = 0;
	int y = 0;
	int width = 0;
	int height = 0;
};

struct SkiaVideoFrameTarget {
	unsigned int framebuffer_id = 0;
	std::uint64_t context_generation = 0;
	int width = 0;
	int height = 0;
	SkiaVideoTargetViewport viewport;
	SkiaVideoTargetOrigin origin = SkiaVideoTargetOrigin::Unknown;
	int sample_count = 0;
	int stencil_bits = 0;
	SkiaVideoTargetPixelFormat pixel_format = SkiaVideoTargetPixelFormat::Unknown;
	SkiaVideoTargetColorSpace color_space = SkiaVideoTargetColorSpace::Unknown;
	bool hdr_to_sdr_complete = false;
	std::uint64_t present_generation = 0;
};

struct SkiaVideoFrameTargetValidation {
	bool valid = false;
	std::string detail;
};

SkiaVideoFrameTargetValidation ValidateSkiaVideoFrameTarget(
	SkiaVideoFrameTarget const& target,
	SkiaGlContextToken context);
