#pragma once

#include <libaegisub/fs_fwd.h>

#include <string>
#include <vector>

namespace aegisub::provider_index_cache {

std::string StreamPart(char prefix, int stream_index);

agi::fs::path BuildFilename(agi::fs::path const& media_filename,
                            std::string const& cache_directory_token,
                            std::string const& extension,
                            std::vector<std::string> const& parts = {});

void Clean(std::string const& cache_directory_token,
           std::string const& file_pattern,
           char const *size_option,
           char const *files_option);

}
