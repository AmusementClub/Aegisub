#pragma once

#include <libaegisub/fs_fwd.h>

#include <functional>
#include <string>
#include <vector>

namespace aegisub::provider_index_cache {

std::string StreamPart(char prefix, int stream_index);

agi::fs::path BuildFilename(agi::fs::path const& media_filename,
                            std::string const& cache_directory_token,
                            std::string const& extension,
                            std::vector<std::string> const& parts = {});

agi::fs::path CacheDirectory(std::string const& cache_directory_token);

void Clean(std::string const& cache_directory_token,
           std::string const& file_pattern,
           char const *size_option,
           char const *files_option,
           std::function<void()> after_clean = {});

}
