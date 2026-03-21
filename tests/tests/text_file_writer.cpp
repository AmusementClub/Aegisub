#include <main.h>

#include "../../src/text_file_writer.h"

#include <libaegisub/fs.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
std::vector<std::filesystem::path> list_matching(std::filesystem::path const& dir, std::string const& pattern) {
	std::vector<std::string> matches;
	agi::fs::DirectoryIterator(dir, pattern).GetAll(matches);

	std::vector<std::filesystem::path> result;
	result.reserve(matches.size());
	for (auto const& match : matches)
		result.emplace_back(match);
	return result;
}

#ifdef _WIN32
class ExclusiveFileLock {
	HANDLE handle = INVALID_HANDLE_VALUE;
	DWORD error = ERROR_SUCCESS;

public:
	explicit ExclusiveFileLock(std::filesystem::path const& path) {
		handle = CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (handle == INVALID_HANDLE_VALUE)
			error = GetLastError();
	}

	~ExclusiveFileLock() {
		if (handle != INVALID_HANDLE_VALUE)
			CloseHandle(handle);
	}

	bool IsLocked() const {
		return handle != INVALID_HANDLE_VALUE;
	}

	DWORD Error() const {
		return error;
	}
};
#endif
}

TEST(text_file_writer, close_reports_write_failures) {
#ifdef _WIN32
	auto const target = std::filesystem::path(agi::fs::UniquePath("data/text_writer_locked_%%%%%%%%.txt"));
	auto const tmp_pattern = target.stem().string() + "_tmp_*" + target.extension().string();
	auto const before = list_matching("data", tmp_pattern);

	{
		std::ofstream seed(target, std::ios::binary);
		seed << "seed";
	}

	{
		ExclusiveFileLock lock(target);
		ASSERT_TRUE(lock.IsLocked()) << "CreateFileW failed with " << lock.Error();

		TextFileWriter writer(target, "UTF-8");
		writer.WriteLineToFile("write should fail");
		EXPECT_THROW(writer.Close(), agi::fs::FileSystemError);
	}

	auto const after = list_matching("data", tmp_pattern);
	for (auto const& path : after) {
		if (std::find(before.begin(), before.end(), path) == before.end())
			std::filesystem::remove(path);
	}
	std::error_code ec;
	std::filesystem::remove(target, ec);
#else
	GTEST_SKIP() << "File-lock save failure regression is Windows-specific.";
#endif
}
