#pragma once

#include <libaegisub/fs_fwd.h>

#include <cstdint>
#include <functional>
#include <string>

/// Clean up the given cache directory, limiting the size to max_size.
void CleanCache(
	agi::fs::path const& directory,
	std::string const& file_type,
	uint64_t max_size,
	uint64_t max_files = static_cast<uint64_t>(-1),
	uint64_t preserve_recent_seconds = 0,
	std::function<void()> after_clean = {});
