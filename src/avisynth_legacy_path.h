#pragma once

#include <libaegisub/fs_fwd.h>

#include <optional>
#include <string>

namespace avisynth {
	std::optional<std::string> TryGetLegacyPathString(agi::fs::path const& path);
	std::string BuildLegacyPathFailureMessage(char const *function_name, agi::fs::path const& path);
}