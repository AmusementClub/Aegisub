#include <gtest/gtest.h>

#include <libaegisub/crc32.h>

#include <boost/crc.hpp>

TEST(lagi_crc32, matches_boost_crc32_for_known_vectors) {
	for (auto const& sample : {
		"",
		"a",
		"abc",
		"The quick brown fox jumps over the lazy dog",
		"?local/ffms2cache/example file.ass",
		"F:/Video/Stra\xC3\x9F" "e/\xE3\x80\x8A" "clip\xE3\x80\x8B" ".mkv"
	}) {
		boost::crc_32_type legacy;
		legacy.process_bytes(sample, strlen(sample));
		EXPECT_EQ(legacy.checksum(), agi::util::crc32(sample));
	}
}

TEST(lagi_crc32, matches_boost_crc32_for_generated_paths) {
	for (int i = 0; i < 2048; ++i) {
		auto sample = std::string("F:/media/cache/") + std::to_string(i) + "/Stra\xC3\x9F" "e/clip_" + std::to_string(i * 17) + ".mkv";
		boost::crc_32_type legacy;
		legacy.process_bytes(sample.data(), sample.size());
		EXPECT_EQ(legacy.checksum(), agi::util::crc32(sample));
	}
}
