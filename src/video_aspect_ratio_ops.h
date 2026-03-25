#pragma once

#include <string>

namespace aegisub::video_aspect_ratio_ops {

enum class ParseStatus {
	Valid,
	InvalidFormat,
	OutOfRange
};

struct ParseResult {
	ParseStatus status = ParseStatus::InvalidFormat;
	double value = 0.0;

	explicit operator bool() const { return status == ParseStatus::Valid; }
};

ParseResult ParseCustomAspectRatio(std::string const& value);

}
