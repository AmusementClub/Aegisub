// Copyright (c) 2013, Thomas Goyne <plorkyeran@aegisub.org>
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
//
// Aegisub Project http://www.aegisub.org/

#include <main.h>

#include <libaegisub/fs.h>
#include <libaegisub/io.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {
std::string read_all(std::filesystem::path const& path) {
	std::ifstream stream(path, std::ios::binary);
	std::ostringstream buffer;
	buffer << stream.rdbuf();
	return buffer.str();
}

std::vector<std::string> list_matching(std::filesystem::path const& dir, std::string const& pattern) {
	std::vector<std::string> files;
	agi::fs::DirectoryIterator(dir, pattern).GetAll(files);
	return files;
}
}

TEST(lagi_io, save_close_overwrites_file_and_cleans_temp_file) {
	auto const target = std::filesystem::path("data/save_close.txt");
	std::filesystem::remove(target);
	{
		std::ofstream seed(target, std::ios::binary);
		seed << "old-value";
	}

	{
		agi::io::Save save(target, true);
		save.Get() << "new-value";
		save.Close();
		save.Close();
	}

	EXPECT_EQ("new-value", read_all(target));
	EXPECT_TRUE(list_matching("data", "save_close_tmp_*.txt").empty());
}

TEST(lagi_io, save_destructor_commits_file_without_leftovers) {
	auto const target = std::filesystem::path("data/save_destructor.txt");
	std::filesystem::remove(target);

	{
		agi::io::Save save(target, true);
		save.Get() << "written-via-destructor";
	}

	EXPECT_TRUE(agi::fs::FileExists(target));
	EXPECT_EQ("written-via-destructor", read_all(target));
	EXPECT_TRUE(list_matching("data", "save_destructor_tmp_*.txt").empty());
}
