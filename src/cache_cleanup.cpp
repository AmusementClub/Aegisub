#include "cache_cleanup.h"

#include <libaegisub/dispatch.h>
#include <libaegisub/exception.h>
#include <libaegisub/fs.h>
#include <libaegisub/log.h>

#include <algorithm>
#include <ctime>
#include <limits>
#include <memory>
#include <vector>

void CleanCache(
	agi::fs::path const& directory,
	std::string const& file_type,
	uint64_t max_size,
	uint64_t max_files,
	uint64_t preserve_recent_seconds) {
	static std::unique_ptr<agi::dispatch::Queue> queue;
	if (!queue)
		queue = agi::dispatch::Create();

	max_size <<= 20;
	if (max_files == 0)
		max_files = std::numeric_limits<uint64_t>::max();
	queue->Async([=] {
		LOG_D("utils/clean_cache") << "cleaning " << directory / file_type;
		uint64_t total_size = 0;
		time_t const preserve_recent_cutoff = preserve_recent_seconds == 0
			? 0
			: std::max<time_t>(0, std::time(nullptr) - static_cast<time_t>(preserve_recent_seconds));
		struct cache_item {
			int64_t modified_time = 0;
			agi::fs::path path;
			bool preserve_recent = false;
		};
		std::vector<cache_item> cachefiles;
		for (auto const& file : agi::fs::DirectoryIterator(directory, file_type)) {
			agi::fs::path path = directory / agi::fs::PathFromString(file);
			auto const modified_time = agi::fs::ModifiedTime(path);
			cachefiles.push_back({
				modified_time,
				path,
				preserve_recent_cutoff != 0 && modified_time >= preserve_recent_cutoff
			});
			total_size += agi::fs::Size(path);
		}

		if (cachefiles.size() <= max_files && total_size <= max_size) {
			LOG_D("utils/clean_cache")
				<< "cache does not need cleaning (maxsize=" << max_size
				<< ", cursize=" << total_size
				<< ", maxfiles=" << max_files
				<< ", numfiles=" << cachefiles.size()
				<< "), exiting";
			return;
		}

		sort(begin(cachefiles), end(cachefiles), [](cache_item const& a, cache_item const& b) {
			return a.modified_time < b.modified_time;
		});

		int deleted = 0;
		for (auto const& i : cachefiles) {
			if ((total_size <= max_size && cachefiles.size() - deleted <= max_files) || cachefiles.size() - deleted < 2)
				break;
			if (i.preserve_recent)
				continue;

			uint64_t size = agi::fs::Size(i.path);
			try {
				agi::fs::Remove(i.path);
				LOG_D("utils/clean_cache") << "deleted " << i.path;
			}
			catch (agi::Exception const& e) {
				LOG_D("utils/clean_cache") << "failed to delete file " << i.path << ": " << e.GetMessage();
				continue;
			}

			total_size -= size;
			++deleted;
		}

		LOG_D("utils/clean_cache") << "deleted " << deleted << " files, exiting";
	});
}
