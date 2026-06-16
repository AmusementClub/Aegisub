#pragma once

#include <functional>

struct RuntimeOptionalFacilityHost {
	std::function<void()> install_png_image_handler;
	std::function<void()> register_subtitle_format_extensions;
};
