#include <main.h>

#include "../../src/subtitle_format_microdvd_parser.h"

#include <limits>

TEST(subtitle_format_microdvd, parse_frames_accepts_valid_values) {
	int start = -1;
	int end = -1;

	EXPECT_TRUE(TryParseMicroDVDFrames("123", "456", start, end));
	EXPECT_EQ(123, start);
	EXPECT_EQ(456, end);
}

TEST(subtitle_format_microdvd, parse_frames_rejects_overflow) {
	int start = 0;
	int end = 0;
	auto too_large = std::to_string(static_cast<long long>(std::numeric_limits<int>::max()) + 1);

	EXPECT_FALSE(TryParseMicroDVDFrames(too_large, "1", start, end));
}

TEST(subtitle_format_microdvd, parse_frames_rejects_non_numeric_input) {
	int start = 0;
	int end = 0;

	EXPECT_FALSE(TryParseMicroDVDFrames("12x", "34", start, end));
	EXPECT_FALSE(TryParseMicroDVDFrames("12", "", start, end));
}
