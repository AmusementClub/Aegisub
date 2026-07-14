#pragma once

#include "secondary_subtitle_packet_stream.h"

#include <memory>
#include <string>

class SubtitlesProvider;

namespace secondary_subtitle_decoder {
	bool IsAvailable(std::string const& codec_id);
	std::unique_ptr<SubtitlesProvider> Create(std::shared_ptr<const SecondarySubtitlePacketStream> stream);
}
