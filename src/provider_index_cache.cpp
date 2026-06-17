#include "provider_index_cache.h"

#include "cache_cleanup.h"
#include "options.h"

#include <libaegisub/crc32.h>
#include <libaegisub/fs.h>
#include <libaegisub/path.h>

namespace aegisub::provider_index_cache {

std::string StreamPart(char prefix, int stream_index) {
	return std::string(1, prefix) + (stream_index < 0 ? std::string("none") : std::to_string(stream_index));
}

agi::fs::path BuildFilename(agi::fs::path const& media_filename,
                            std::string const& cache_directory_token,
                            std::string const& extension,
                            std::vector<std::string> const& parts) {
	auto filename = cache_directory_token
		+ std::to_string(agi::util::crc32(agi::fs::PathToString(media_filename)))
		+ "_" + std::to_string(agi::fs::Size(media_filename))
		+ "_" + std::to_string(agi::fs::ModifiedTime(media_filename));
	for (auto const& part : parts)
		filename += "_" + part;
	filename += extension;

	auto result = config::path->Decode(filename);
	agi::fs::CreateDirectory(result.parent_path());
	return result;
}

void Clean(std::string const& cache_directory_token,
           std::string const& file_pattern,
           char const *size_option,
           char const *files_option) {
	::CleanCache(config::path->Decode(cache_directory_token),
		file_pattern,
		OPT_GET(size_option)->GetInt(),
		OPT_GET(files_option)->GetInt());
}

}
