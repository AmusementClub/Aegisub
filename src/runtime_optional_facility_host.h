#pragma once

#include <functional>

struct RuntimeOptionalFacilityHost {
	std::function<void()> install_png_image_handler;
};
