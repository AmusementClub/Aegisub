// Copyright (c) 2026
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

#pragma once

#include "include/aegisub/video_color_metadata.h"

#include <string>

namespace ffms {

enum class ColorSpaceValue {
	Rgb = 0,
	Bt709 = 1,
	Unspecified = 2,
	Fcc = 4,
	Bt470Bg = 5,
	Smpte170M = 6,
	Smpte240M = 7,
	Ycocg = 8,
	Bt2020Ncl = 9,
	Bt2020Cl = 10
};

enum class ColorRangeValue {
	Unspecified = 0,
	Mpeg = 1,
	Jpeg = 2
};

enum class ColorPrimariesValue {
	Reserved0 = 0,
	Bt709 = 1,
	Unspecified = 2,
	Reserved = 3,
	Bt470M = 4,
	Bt470Bg = 5,
	Smpte170M = 6,
	Smpte240M = 7,
	Film = 8,
	Bt2020 = 9
};

enum class TransferCharacteristicValue {
	Reserved0 = 0,
	Bt709 = 1,
	Unspecified = 2,
	Reserved = 3,
	Gamma22 = 4,
	Gamma28 = 5,
	Smpte170M = 6,
	Smpte240M = 7,
	Linear = 8,
	Log = 9,
	LogSqrt = 10,
	Iec61966_2_4 = 11,
	Bt1361Ecg = 12,
	Iec61966_2_1 = 13,
	Bt2020_10 = 14,
	Bt2020_12 = 15
};

inline std::string MapColorPrimaries(int value) {
	switch (value) {
		case static_cast<int>(ColorPrimariesValue::Bt709):
			return "BT.709";
		case static_cast<int>(ColorPrimariesValue::Bt470M):
			return "BT.470M";
		case static_cast<int>(ColorPrimariesValue::Bt470Bg):
			return "BT.601-625";
		case static_cast<int>(ColorPrimariesValue::Smpte170M):
			return "BT.601-525";
		case static_cast<int>(ColorPrimariesValue::Smpte240M):
			return "SMPTE-240M";
		case static_cast<int>(ColorPrimariesValue::Film):
			return "Film C";
		case static_cast<int>(ColorPrimariesValue::Bt2020):
			return "BT.2020";
		default:
			return std::string();
	}
}

inline std::string FallbackPrimariesForColorSpace(int cs) {
	switch (cs) {
		case static_cast<int>(ColorSpaceValue::Bt709):
			return "BT.709";
		case static_cast<int>(ColorSpaceValue::Bt470Bg):
			return "BT.601-625";
		case static_cast<int>(ColorSpaceValue::Smpte170M):
			return "BT.601-525";
		case static_cast<int>(ColorSpaceValue::Smpte240M):
			return "SMPTE-240M";
		case static_cast<int>(ColorSpaceValue::Bt2020Ncl):
		case static_cast<int>(ColorSpaceValue::Bt2020Cl):
			return "BT.2020";
		default:
			return std::string();
	}
}

inline std::string MapTransferCharacteristic(int value) {
	switch (value) {
		case static_cast<int>(TransferCharacteristicValue::Bt709):
		case static_cast<int>(TransferCharacteristicValue::Smpte170M):
		case static_cast<int>(TransferCharacteristicValue::Smpte240M):
		case static_cast<int>(TransferCharacteristicValue::Iec61966_2_4):
		case static_cast<int>(TransferCharacteristicValue::Bt1361Ecg):
		case static_cast<int>(TransferCharacteristicValue::Bt2020_10):
		case static_cast<int>(TransferCharacteristicValue::Bt2020_12):
			return "BT.1886";
		case static_cast<int>(TransferCharacteristicValue::Gamma22):
			return "Gamma 2.2";
		case static_cast<int>(TransferCharacteristicValue::Gamma28):
			return "Gamma 2.8";
		case static_cast<int>(TransferCharacteristicValue::Linear):
			return "Linear";
		case static_cast<int>(TransferCharacteristicValue::Iec61966_2_1):
			return "sRGB";
		default:
			return std::string();
	}
}

inline SourceFrameColorMetadata MapColorMetadata(
	int cs,
	int cr,
	int primaries,
	int transfer,
	std::string const& matrix) {
	auto color = SourceFrameColorMetadataFromLegacyColorSpace(matrix);
	color.range = cr == static_cast<int>(ColorRangeValue::Jpeg)
		? SourceFrameColorRange::Full
		: SourceFrameColorRange::Limited;

	auto mapped_primaries = MapColorPrimaries(primaries);
	if (!mapped_primaries.empty())
		color.primaries = std::move(mapped_primaries);
	else {
		auto fallback_primaries = FallbackPrimariesForColorSpace(cs);
		if (!fallback_primaries.empty())
			color.primaries = std::move(fallback_primaries);
	}

	auto mapped_transfer = MapTransferCharacteristic(transfer);
	if (!mapped_transfer.empty())
		color.transfer = std::move(mapped_transfer);

	if (cs == static_cast<int>(ColorSpaceValue::Rgb))
		color.matrix = "None";

	return color;
}

}
