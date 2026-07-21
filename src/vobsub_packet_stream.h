#pragma once

#include "secondary_subtitle_packet_stream.h"

#include <libaegisub/exception.h>
#include <libaegisub/fs_fwd.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

DEFINE_EXCEPTION(VobSubParseError, agi::InvalidInputException);

/// A timestamp/file-position pair from a VobSub index file.
struct VobSubPacketIndex {
	int64_t pts_ns = kSecondarySubtitleTimestampUnknown;
	uint64_t filepos = 0;
};

/// Metadata for one VobSub language/subpicture stream.
struct VobSubTrackInfo {
	int index = -1;
	std::string language;
	std::string name;
	bool is_default = false;
	std::vector<VobSubPacketIndex> packets;
};

/// Parsed metadata shared by all tracks in an IDX file.
struct VobSubIndexInfo {
	std::string codec_private;
	int fallback_canvas_width = 0;
	int fallback_canvas_height = 0;
	int default_track_index = -1;
	std::vector<VobSubTrackInfo> tracks;
};

bool IsVobSubIndexPath(agi::fs::path const& path);
agi::fs::path GetVobSubCompanionPath(agi::fs::path const& index_path, bool uppercase_extension = false);

VobSubIndexInfo ParseVobSubIndex(std::string_view data);
VobSubIndexInfo ReadVobSubIndex(agi::fs::path const& filename);

/// Parse an IDX and its companion SUB data and return one complete-SPU packet
/// stream for the requested language index. A negative index selects the IDX
/// default (`langidx`, or the first track when `langidx` is absent/invalid).
SecondarySubtitlePacketStream ParseVobSubPacketStream(
	std::string_view idx_data,
	std::string_view sub_data,
	int track_index = -1);

SecondarySubtitlePacketStream ReadVobSubPacketStream(
	agi::fs::path const& filename,
	int track_index = -1);
