#pragma once

#include "secondary_subtitle_packet_stream.h"

#include <libaegisub/exception.h>
#include <libaegisub/fs_fwd.h>

#include <cstdint>
#include <string_view>

DEFINE_EXCEPTION(PgsSupParseError, agi::InvalidInputException);

namespace pgs_sup {
	int64_t TicksToNanoseconds(uint64_t ticks);
}

bool IsPgsSupSubtitlePath(agi::fs::path const& path);
SecondarySubtitlePacketStream ParsePgsSupPacketStream(std::string_view data);
SecondarySubtitlePacketStream ReadPgsSupPacketStream(agi::fs::path const& filename);
