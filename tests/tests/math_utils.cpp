#include <gtest/gtest.h>

#include <libaegisub/math_utils.h>

#include <boost/rational.hpp>

TEST(lagi_math_utils, reduce_ratio_matches_boost_rational) {
	for (auto const& sample : {
		std::pair<int, int>{1920, 1080},
		std::pair<int, int>{1280, 720},
		std::pair<int, int>{1024, 576},
		std::pair<int, int>{853, 480},
		std::pair<int, int>{3840, 2160},
		std::pair<int, int>{0, 1080},
		std::pair<int, int>{1, 1},
		std::pair<int, int>{999, 777}
	}) {
		boost::rational<int> legacy(sample.first, sample.second);
		auto reduced = agi::util::reduce_ratio(sample.first, sample.second);
		EXPECT_EQ(legacy.numerator(), reduced.first);
		EXPECT_EQ(legacy.denominator(), reduced.second);
	}
}
