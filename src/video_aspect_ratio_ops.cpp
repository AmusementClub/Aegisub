#include "video_aspect_ratio_ops.h"

#include <libaegisub/string_utils.h>
#include <libaegisub/util.h>

#include <cmath>

namespace {

constexpr double kMinAspectRatio = 0.5;
constexpr double kMaxAspectRatio = 5.0;

bool TryParseDouble(std::string const& value, double& out) {
	auto trimmed = agi::util::strings::trim_copy(value);
	return !trimmed.empty() && agi::util::try_parse(trimmed, &out);
}

bool TryParseFractionalOrResolutionRatio(std::string const& value, double& out) {
	auto trimmed = agi::util::strings::trim_copy(value);
	auto separator = trimmed.find_first_of(":/xX");
	if (separator == std::string::npos)
		return false;
	if (trimmed.find_first_of(":/xX", separator + 1) != std::string::npos)
		return false;

	double numerator = 0.0;
	double denominator = 0.0;
	if (!TryParseDouble(trimmed.substr(0, separator), numerator)
		|| !TryParseDouble(trimmed.substr(separator + 1), denominator)
		|| denominator == 0.0)
		return false;

	out = numerator / denominator;
	return std::isfinite(out);
}

}

namespace aegisub::video_aspect_ratio_ops {

ParseResult ParseCustomAspectRatio(std::string const& value) {
	double parsed = 0.0;
	if (!TryParseDouble(value, parsed) && !TryParseFractionalOrResolutionRatio(value, parsed))
		return {};

	if (!std::isfinite(parsed))
		return {};

	if (parsed < kMinAspectRatio || parsed > kMaxAspectRatio)
		return {ParseStatus::OutOfRange, parsed};

	return {ParseStatus::Valid, parsed};
}

}
