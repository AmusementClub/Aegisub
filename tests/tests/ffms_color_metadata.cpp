#include <main.h>

#include "../../src/ffms_color_metadata.h"

#include <gtest/gtest.h>

TEST(ffms_color_metadata, maps_ffms_primaries_to_renderer_facing_tokens) {
	EXPECT_EQ("BT.709", ffms::MapColorPrimaries(1));
	EXPECT_EQ("BT.470M", ffms::MapColorPrimaries(4));
	EXPECT_EQ("BT.601-625", ffms::MapColorPrimaries(5));
	EXPECT_EQ("BT.601-525", ffms::MapColorPrimaries(6));
	EXPECT_EQ("SMPTE-240M", ffms::MapColorPrimaries(7));
	EXPECT_EQ("Film C", ffms::MapColorPrimaries(8));
	EXPECT_EQ("BT.2020", ffms::MapColorPrimaries(9));
	EXPECT_TRUE(ffms::MapColorPrimaries(0).empty());
	EXPECT_TRUE(ffms::MapColorPrimaries(999).empty());
}

TEST(ffms_color_metadata, maps_ffms_transfer_to_renderer_facing_tokens) {
	EXPECT_EQ("BT.1886", ffms::MapTransferCharacteristic(1));
	EXPECT_EQ("Gamma 2.2", ffms::MapTransferCharacteristic(4));
	EXPECT_EQ("Gamma 2.8", ffms::MapTransferCharacteristic(5));
	EXPECT_EQ("BT.1886", ffms::MapTransferCharacteristic(6));
	EXPECT_EQ("Linear", ffms::MapTransferCharacteristic(8));
	EXPECT_EQ("sRGB", ffms::MapTransferCharacteristic(13));
	EXPECT_EQ("BT.1886", ffms::MapTransferCharacteristic(14));
	EXPECT_TRUE(ffms::MapTransferCharacteristic(9).empty());
	EXPECT_TRUE(ffms::MapTransferCharacteristic(999).empty());
}

TEST(ffms_color_metadata, explicit_ffms_primaries_and_transfer_override_legacy_fallbacks) {
	auto color = ffms::MapColorMetadata(
		static_cast<int>(ffms::ColorSpaceValue::Bt470Bg),
		static_cast<int>(ffms::ColorRangeValue::Mpeg),
		static_cast<int>(ffms::ColorPrimariesValue::Film),
		static_cast<int>(ffms::TransferCharacteristicValue::Gamma22),
		"TV.601");

	EXPECT_EQ("TV.601", color.matrix);
	EXPECT_EQ("Film C", color.primaries);
	EXPECT_EQ("Gamma 2.2", color.transfer);
	EXPECT_EQ(SourceFrameColorRange::Limited, color.range);
}

TEST(ffms_color_metadata, falls_back_to_colorspace_specific_primaries_when_ffms_does_not_specify_them) {
	auto color = ffms::MapColorMetadata(
		static_cast<int>(ffms::ColorSpaceValue::Smpte170M),
		static_cast<int>(ffms::ColorRangeValue::Jpeg),
		static_cast<int>(ffms::ColorPrimariesValue::Unspecified),
		static_cast<int>(ffms::TransferCharacteristicValue::Unspecified),
		"PC.601");

	EXPECT_EQ("BT.601-525", color.primaries);
	EXPECT_TRUE(color.transfer.empty());
	EXPECT_EQ(SourceFrameColorRange::Full, color.range);
}

TEST(ffms_color_metadata, rgb_sources_keep_none_matrix_and_can_carry_transfer) {
	auto color = ffms::MapColorMetadata(
		static_cast<int>(ffms::ColorSpaceValue::Rgb),
		static_cast<int>(ffms::ColorRangeValue::Jpeg),
		static_cast<int>(ffms::ColorPrimariesValue::Bt709),
		static_cast<int>(ffms::TransferCharacteristicValue::Iec61966_2_1),
		"None");

	EXPECT_EQ("None", color.matrix);
	EXPECT_EQ("BT.709", color.primaries);
	EXPECT_EQ("sRGB", color.transfer);
	EXPECT_EQ(SourceFrameColorRange::Full, color.range);
}
