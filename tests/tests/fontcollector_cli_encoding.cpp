#include "../../src/fontcollector_cli_encoding.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

struct TempDirectory {
	std::filesystem::path path;

	TempDirectory() {
		auto const unique = std::to_string(
			static_cast<long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
		path = std::filesystem::temp_directory_path()
			/ std::filesystem::path("aegisub-fontcollector-encoding-test-" + unique);
		std::filesystem::create_directories(path);
	}

	~TempDirectory() {
		std::error_code error;
		std::filesystem::remove_all(path, error);
	}
};

void WriteBytes(std::filesystem::path const& path, std::vector<unsigned char> const& bytes) {
	std::ofstream output(path, std::ios::binary);
	if (bytes.empty())
		return;
	output.write(
		reinterpret_cast<char const*>(bytes.data()),
		static_cast<std::streamsize>(bytes.size()));
}

}

TEST(fontcollector_cli_encoding, identifies_boms_and_defers_ambiguous_control_bytes) {
	TempDirectory temp;
	auto const empty = temp.path / "empty.ass";
	auto const utf16 = temp.path / "utf16.ass";
	auto const utf16_empty = temp.path / "utf16-empty.ass";
	auto const binary = temp.path / "binary.ass";
	WriteBytes(empty, {});
	WriteBytes(utf16, {0xff, 0xfe, 0x41, 0x00});
	WriteBytes(utf16_empty, {0xff, 0xfe});
	WriteBytes(binary, {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x41});

	EXPECT_EQ(aegisub::fontcollector_cli::PreferredAutomaticEncoding(empty), "ascii");
	EXPECT_EQ(aegisub::fontcollector_cli::PreferredAutomaticEncoding(utf16), "utf-16le");
	EXPECT_EQ(aegisub::fontcollector_cli::PreferredAutomaticEncoding(utf16_empty), "utf-16le");
	EXPECT_TRUE(aegisub::fontcollector_cli::PreferredAutomaticEncoding(binary).empty());
}

TEST(fontcollector_cli_encoding, prefers_strict_utf8_across_read_boundaries) {
	TempDirectory temp;
	auto const path = temp.path / "utf8.ass";
	std::vector<unsigned char> bytes(65535, static_cast<unsigned char>('A'));
	bytes.insert(bytes.end(), {0xe4, 0xb8, 0x80});
	WriteBytes(path, bytes);

	EXPECT_EQ(aegisub::fontcollector_cli::PreferredAutomaticEncoding(path), "utf-8");
}

TEST(fontcollector_cli_encoding, defers_non_utf8_text_to_the_legacy_detector) {
	TempDirectory temp;
	auto const path = temp.path / "legacy.ass";
	WriteBytes(path, {0x82, 0xa0, 0x82, 0xa2});

	EXPECT_TRUE(aegisub::fontcollector_cli::PreferredAutomaticEncoding(path).empty());
}

TEST(fontcollector_cli_encoding, defers_escape_heavy_iso2022_style_input) {
	TempDirectory temp;
	auto const path = temp.path / "iso2022.ass";
	std::vector<unsigned char> bytes;
	for (int index = 0; index < 128; ++index)
		bytes.insert(bytes.end(), {0x1b, 0x24, 0x42, 0x36, 0x59, 0x1b, 0x28, 0x42});
	WriteBytes(path, bytes);

	EXPECT_TRUE(aegisub::fontcollector_cli::PreferredAutomaticEncoding(path).empty());
}
