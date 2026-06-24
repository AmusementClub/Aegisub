#include "provider_index_cache.h"

#include "cache_cleanup.h"
#include "options.h"

#include <libaegisub/crc32.h>
#include <libaegisub/fs.h>
#include <libaegisub/path.h>

#include <filesystem>
#include <functional>
#include <string_view>
#include <utility>

namespace aegisub::provider_index_cache {
namespace {

constexpr int kDefaultCacheSizeMb = 42;
constexpr int kDefaultCacheFiles = 20;

bool IsTokenPath(std::string const& path) {
	return !path.empty() && path[0] == '?';
}

agi::fs::path FallbackCachePath(std::string path) {
	if (IsTokenPath(path))
		path.erase(0, 1);
	for (auto& ch : path) {
		if (ch == '?' || ch == ':')
			ch = '_';
	}
	return std::filesystem::temp_directory_path() / "aegisub-core-cache" / agi::fs::PathFromString(path);
}

agi::fs::path DecodeCachePath(std::string const& path) {
	if (config::path) {
		auto decoded = config::path->Decode(path);
		if (!IsTokenPath(path) || !IsTokenPath(agi::fs::PathToString(decoded)))
			return decoded;
	}

	if (IsTokenPath(path))
		return FallbackCachePath(path);

	return agi::fs::PathFromString(path);
}

int GetCacheOptionInt(char const *option_name, int default_value) {
	return config::GetIntOptionOrDefault(option_name, default_value);
}

int DefaultCacheOptionValue(char const *option_name) {
	std::string_view name(option_name ? option_name : "");
	if (name.ends_with("/Files"))
		return kDefaultCacheFiles;
	if (name.ends_with("/Size"))
		return kDefaultCacheSizeMb;
	return 0;
}

}

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

	auto result = DecodeCachePath(filename);
	agi::fs::CreateDirectory(result.parent_path());
	return result;
}

agi::fs::path CacheDirectory(std::string const& cache_directory_token) {
	auto directory = DecodeCachePath(cache_directory_token);
	agi::fs::CreateDirectory(directory);
	return directory;
}

void Clean(std::string const& cache_directory_token,
           std::string const& file_pattern,
           char const *size_option,
           char const *files_option,
           std::function<void()> after_clean) {
	auto directory = CacheDirectory(cache_directory_token);
	::CleanCache(directory,
		file_pattern,
		GetCacheOptionInt(size_option, DefaultCacheOptionValue(size_option)),
		GetCacheOptionInt(files_option, DefaultCacheOptionValue(files_option)),
		0,
		std::move(after_clean));
}

}
