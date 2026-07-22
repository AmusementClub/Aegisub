#pragma once

#include <filesystem>
#include <string>

namespace aegisub::fontcollector_cli {

// Returns an unambiguous encoding, or an empty string to use the configured
// legacy charset detector.
std::string PreferredAutomaticEncoding(std::filesystem::path const& input);

}
