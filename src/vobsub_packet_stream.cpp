#include "vobsub_packet_stream.h"

#include "secondary_subtitle_packet_io.h"

#include <libaegisub/fs.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
constexpr char kIndexMagic[] = "# VobSub index file, v";
constexpr int kMaxIndexVersion = 7;
constexpr int kMaxTrackIndex = 31;
constexpr int64_t kMillisecondsPerSecond = 1000;
constexpr int64_t kNanosecondsPerMillisecond = 1000000;

using Byte = unsigned char;

std::string_view Trim(std::string_view value) {
	while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
		value.remove_prefix(1);
	while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
		value.remove_suffix(1);
	return value;
}

std::string Lower(std::string_view value) {
	std::string result;
	result.reserve(value.size());
	for (auto ch : value)
		result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
	return result;
}

[[noreturn]] void Fail(std::string message) {
	throw VobSubParseError(std::move(message));
}

template<typename T>
bool ParseUnsigned(std::string_view value, int base, T& result) {
	value = Trim(value);
	if (value.empty())
		return false;
	T parsed = 0;
	auto const *begin = value.data();
	auto const *end = begin + value.size();
	auto const conversion = std::from_chars(begin, end, parsed, base);
	if (conversion.ec != std::errc() || conversion.ptr != end)
		return false;
	result = parsed;
	return true;
}

bool ParseSignedInteger(std::string_view value, int64_t& result) {
	value = Trim(value);
	if (value.empty())
		return false;
	int64_t parsed = 0;
	auto const *begin = value.data();
	auto const *end = begin + value.size();
	auto const conversion = std::from_chars(begin, end, parsed, 10);
	if (conversion.ec != std::errc() || conversion.ptr != end)
		return false;
	result = parsed;
	return true;
}

int64_t CheckedAdd(int64_t left, int64_t right, char const *what) {
	if ((right > 0 && left > std::numeric_limits<int64_t>::max() - right)
		|| (right < 0 && left < std::numeric_limits<int64_t>::min() - right))
		Fail(std::string("VobSub ") + what + " overflows.");
	return left + right;
}

int64_t MillisecondsToNanoseconds(int64_t milliseconds) {
	if (milliseconds > 0
		&& milliseconds > std::numeric_limits<int64_t>::max() / kNanosecondsPerMillisecond)
		Fail("VobSub timestamp overflows nanoseconds.");
	if (milliseconds < 0
		&& milliseconds < std::numeric_limits<int64_t>::min() / kNanosecondsPerMillisecond)
		Fail("VobSub timestamp overflows nanoseconds.");
	return milliseconds * kNanosecondsPerMillisecond;
}

// Parse the VobSub hh:mm:ss:mmm form. The optional sign applies to the whole
// value, and minutes/seconds/milliseconds are deliberately range checked so a
// malformed index cannot silently shift a subtitle by hours.
int64_t ParseTimestampMilliseconds(std::string_view value, char const *what) {
	value = Trim(value);
	if (value.empty())
		Fail(std::string("VobSub ") + what + " is empty.");

	bool negative = false;
	if (value.front() == '+' || value.front() == '-') {
		negative = value.front() == '-';
		value.remove_prefix(1);
		value = Trim(value);
	}

	uint64_t fields[4] = {};
	size_t begin = 0;
	for (size_t field = 0; field < 4; ++field) {
		auto const end = field == 3 ? value.size() : value.find(':', begin);
		if (end == std::string_view::npos || end == begin)
			Fail(std::string("Malformed VobSub ") + what + ".");
		if (!ParseUnsigned(value.substr(begin, end - begin), 10, fields[field]))
			Fail(std::string("Malformed VobSub ") + what + ".");
		begin = field == 3 ? end : end + 1;
	}
	if (begin != value.size())
		Fail(std::string("Malformed VobSub ") + what + ".");
	if (fields[1] > 59 || fields[2] > 59 || fields[3] > 999)
		Fail(std::string("Malformed VobSub ") + what + ".");

	uint64_t total = fields[0];
	auto const checked_mul_add = [&](uint64_t multiplier, uint64_t addend) {
		if (total > (std::numeric_limits<uint64_t>::max() - addend) / multiplier)
			Fail(std::string("VobSub ") + what + " overflows.");
		total = total * multiplier + addend;
	};
	checked_mul_add(60, fields[1]);
	checked_mul_add(60, fields[2]);
	checked_mul_add(kMillisecondsPerSecond, fields[3]);

	uint64_t const negative_limit = static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1;
	if (total > (negative ? negative_limit : static_cast<uint64_t>(std::numeric_limits<int64_t>::max())))
		Fail(std::string("VobSub ") + what + " overflows.");
	if (!negative)
		return static_cast<int64_t>(total);
	if (total == negative_limit)
		return std::numeric_limits<int64_t>::min();
	return -static_cast<int64_t>(total);
}

uint64_t ParseFilePosition(std::string_view value) {
	uint64_t position = 0;
	if (!ParseUnsigned(Trim(value), 16, position))
		Fail("Malformed VobSub file position.");
	return position;
}

void ParseSize(std::string_view value, int& width, int& height) {
	value = Trim(value);
	auto const separator = value.find_first_of("xX");
	if (separator == std::string_view::npos)
		Fail("Malformed VobSub size.");
	uint64_t parsed_width = 0;
	uint64_t parsed_height = 0;
	if (!ParseUnsigned(value.substr(0, separator), 10, parsed_width)
		|| !ParseUnsigned(value.substr(separator + 1), 10, parsed_height)
		|| parsed_width == 0 || parsed_height == 0
		|| parsed_width > static_cast<uint64_t>(std::numeric_limits<int>::max())
		|| parsed_height > static_cast<uint64_t>(std::numeric_limits<int>::max()))
		Fail("Malformed VobSub size.");
	width = static_cast<int>(parsed_width);
	height = static_cast<int>(parsed_height);
}

void ValidatePalette(std::string_view value) {
	value = Trim(value);
	for (int i = 0; i < 16; ++i) {
		auto const separator = value.find(',');
		auto part = separator == std::string_view::npos ? value : value.substr(0, separator);
		part = Trim(part);
		if (part.empty() || part.size() > 6) {
			Fail("Malformed VobSub palette.");
		}
		uint32_t color = 0;
		if (!ParseUnsigned(part, 16, color) || color > 0xffffffu)
			Fail("Malformed VobSub palette.");
		if (separator == std::string_view::npos) {
			if (i != 15)
				Fail("Malformed VobSub palette.");
			value = {};
		}
		else {
			value.remove_prefix(separator + 1);
		}
	}
	if (!Trim(value).empty())
		Fail("Malformed VobSub palette.");
}

int ParseTrackIndex(std::string_view value) {
	int64_t parsed = 0;
	if (!ParseSignedInteger(value, parsed) || parsed < 0 || parsed > kMaxTrackIndex)
		Fail("Malformed VobSub language index.");
	return static_cast<int>(parsed);
}

int ParseDefaultTrackIndex(std::string_view value) {
	int64_t parsed = 0;
	if (!ParseSignedInteger(value, parsed) || parsed < -1 || parsed > kMaxTrackIndex)
		Fail("Malformed VobSub default language index.");
	return static_cast<int>(parsed);
}

int FindTrack(std::vector<VobSubTrackInfo> const& tracks, int index) {
	for (size_t i = 0; i < tracks.size(); ++i) {
		if (tracks[i].index == index)
			return static_cast<int>(i);
	}
	return -1;
}

std::string_view ParseKey(std::string_view line, std::string& key, std::string_view& value) {
	auto const separator = line.find(':');
	if (separator == std::string_view::npos) {
		key.clear();
		value = {};
		return {};
	}
	key = Lower(Trim(line.substr(0, separator)));
	value = Trim(line.substr(separator + 1));
	return value;
}

void ParseId(std::string_view value, std::string& language, int& index) {
	auto const separator = value.find(',');
	if (separator == std::string_view::npos)
		Fail("Malformed VobSub id line.");
	language = std::string(Trim(value.substr(0, separator)));
	if (language.empty())
		Fail("Malformed VobSub id line.");

	auto const remainder = Trim(value.substr(separator + 1));
	auto const index_separator = remainder.find(':');
	if (index_separator == std::string_view::npos
		|| Lower(Trim(remainder.substr(0, index_separator))) != "index")
		Fail("Malformed VobSub id line.");
	index = ParseTrackIndex(remainder.substr(index_separator + 1));
}

void ParseTimestamp(std::string_view value, int64_t delay_ms, VobSubTrackInfo& track) {
	auto const separator = value.find(',');
	if (separator == std::string_view::npos)
		Fail("Malformed VobSub timestamp line.");
	auto const timestamp = ParseTimestampMilliseconds(value.substr(0, separator), "timestamp");
	auto const remainder = Trim(value.substr(separator + 1));
	auto const filepos_separator = remainder.find(':');
	if (filepos_separator == std::string_view::npos
		|| Lower(Trim(remainder.substr(0, filepos_separator))) != "filepos")
		Fail("Malformed VobSub timestamp line.");
	VobSubPacketIndex packet;
	packet.pts_ns = MillisecondsToNanoseconds(CheckedAdd(timestamp, delay_ms, "timestamp"));
	packet.filepos = ParseFilePosition(remainder.substr(filepos_separator + 1));
	track.packets.emplace_back(packet);
}

VobSubIndexInfo ParseIndexInternal(std::string_view data) {
	VobSubIndexInfo result;
	std::vector<std::string> private_lines;
	std::vector<int64_t> delays(static_cast<size_t>(kMaxTrackIndex) + 1, 0);
	std::vector<bool> seen_indices(static_cast<size_t>(kMaxTrackIndex) + 1, false);
	std::optional<int> langidx;
	int64_t global_delay_ms = 0;
	int current_track = -1;
	bool saw_track = false;
	bool first_line = true;

	for (size_t offset = 0; offset <= data.size();) {
		auto const line_end = data.find('\n', offset);
		auto line = data.substr(offset, line_end == std::string_view::npos ? data.size() - offset : line_end - offset);
		if (first_line) {
			first_line = false;
			if (line.size() >= 3
				&& static_cast<unsigned char>(line[0]) == 0xef
				&& static_cast<unsigned char>(line[1]) == 0xbb
				&& static_cast<unsigned char>(line[2]) == 0xbf)
				line.remove_prefix(3);
			auto const header = Trim(line);
			if (header.size() <= sizeof(kIndexMagic) - 1
				|| header.substr(0, sizeof(kIndexMagic) - 1) != kIndexMagic)
				Fail("Invalid VobSub index header.");
			auto version_text = header.substr(sizeof(kIndexMagic) - 1);
			size_t version_length = 0;
			while (version_length < version_text.size()
				&& std::isdigit(static_cast<unsigned char>(version_text[version_length])))
				++version_length;
			uint64_t version = 0;
			if (version_length == 0
				|| !ParseUnsigned(version_text.substr(0, version_length), 10, version)
				|| version == 0
				|| version > static_cast<uint64_t>(kMaxIndexVersion))
				Fail("Unsupported VobSub index version.");
		}
		else {
			line = Trim(line);
			if (!line.empty() && line.front() != '#') {
				std::string key;
				std::string_view value;
				ParseKey(line, key, value);
				if (key == "id") {
					std::string language;
					int index = -1;
					ParseId(value, language, index);
					if (seen_indices[static_cast<size_t>(index)])
						Fail("Duplicate VobSub language index.");
					seen_indices[static_cast<size_t>(index)] = true;
					result.tracks.push_back(VobSubTrackInfo{});
					auto& track = result.tracks.back();
					track.index = index;
					track.language = std::move(language);
					current_track = index;
					// Some VobSub writers emit one global delay before the first
					// language id. Keep that baseline for every track, while still
					// allowing per-track delay adjustments below the id line.
					delays[static_cast<size_t>(index)] = global_delay_ms;
					saw_track = true;
				}
				else if (key == "langidx") {
					if (langidx)
						Fail("Duplicate VobSub langidx entry.");
					langidx = ParseDefaultTrackIndex(value);
				}
				else if (key == "size") {
					ParseSize(value, result.fallback_canvas_width, result.fallback_canvas_height);
				}
				else if (key == "palette") {
					ValidatePalette(value);
				}
				else if (key == "alt") {
					if (current_track < 0)
						Fail("VobSub alt entry appears before a language id.");
					auto const track_number = FindTrack(result.tracks, current_track);
					if (track_number < 0)
						Fail("VobSub alt entry refers to an unknown language.");
					result.tracks[static_cast<size_t>(track_number)].name = std::string(value);
				}
				else if (key == "delay") {
					auto const delay = ParseTimestampMilliseconds(value, "delay");
					if (current_track < 0)
						global_delay_ms = delay;
					else
						delays[static_cast<size_t>(current_track)] = CheckedAdd(
							delays[static_cast<size_t>(current_track)], delay, "delay");
				}
				else if (key == "timestamp") {
					if (current_track < 0)
						Fail("VobSub timestamp entry appears before a language id.");
					auto const track_number = FindTrack(result.tracks, current_track);
					if (track_number < 0)
						Fail("VobSub timestamp refers to an unknown language.");
					ParseTimestamp(value, delays[static_cast<size_t>(current_track)], result.tracks[static_cast<size_t>(track_number)]);
				}

				// FFmpeg's dvdsub decoder consumes the IDX-style size/palette
				// records as codec private data. Preserve every non-comment setting
				// which precedes the first track, including settings unknown to this
				// parser, so newer IDX variants remain usable.
				if (!saw_track) {
					if (key == "size" || key == "palette")
						private_lines.emplace_back(key + ": " + std::string(value));
					else
						private_lines.emplace_back(line);
				}
			}
		}

		if (line_end == std::string_view::npos)
			break;
		offset = line_end + 1;
	}

	if (first_line)
		Fail("Empty VobSub index file.");
	if (result.tracks.empty())
		Fail("VobSub index contains no language tracks.");

	if (!private_lines.empty()) {
		for (size_t i = 0; i < private_lines.size(); ++i) {
			if (i)
				result.codec_private.push_back('\n');
			result.codec_private += private_lines[i];
		}
	}

	int default_index = result.tracks.front().index;
	if (langidx) {
		if (FindTrack(result.tracks, *langidx) >= 0)
			default_index = *langidx;
	}
	result.default_track_index = default_index;
	for (auto& track : result.tracks)
		track.is_default = track.index == default_index;
	return result;
}

bool IsStartCodeAt(std::string_view data, size_t offset) {
	return offset <= data.size() && data.size() - offset >= 4
		&& static_cast<Byte>(data[offset]) == 0
		&& static_cast<Byte>(data[offset + 1]) == 0
		&& static_cast<Byte>(data[offset + 2]) == 1;
}

size_t FindStartCode(std::string_view data, size_t offset) {
	for (size_t i = offset; i + 4 <= data.size(); ++i) {
		if (IsStartCodeAt(data, i))
			return i;
	}
	return std::string_view::npos;
}

size_t ParsePackHeader(std::string_view data, size_t offset) {
	if (data.size() - offset < 12)
		Fail("Truncated VobSub MPEG pack header.");
	auto const marker = static_cast<Byte>(data[offset + 4]);
	size_t header_size = 0;
	if ((marker & 0xc0) == 0x40) {
		if (data.size() - offset < 14)
			Fail("Truncated VobSub MPEG-2 pack header.");
		header_size = 14 + (static_cast<Byte>(data[offset + 13]) & 0x07);
	}
	else if ((marker & 0xf0) == 0x20) {
		header_size = 12;
	}
	else {
		Fail("Invalid VobSub MPEG pack header.");
	}
	if (header_size > data.size() - offset)
		Fail("Truncated VobSub MPEG pack stuffing.");
	return offset + header_size;
}

size_t ParseLengthPacket(std::string_view data, size_t offset, bool require_length) {
	if (data.size() - offset < 6)
		Fail("Truncated VobSub PES header.");
	auto const length = static_cast<size_t>(secondary_subtitle_packet_io::ReadBigEndian16(data, offset + 4));
	if (require_length && length == 0)
		Fail("VobSub private-stream PES has no bounded payload.");
	if (length > data.size() - offset - 6)
		Fail("Truncated VobSub PES payload.");
	return offset + 6 + length;
}

struct PrivatePesPayload {
	Byte substream_id = 0;
	size_t data_begin = 0;
	size_t data_end = 0;
};

PrivatePesPayload ParsePrivatePes(std::string_view data, size_t offset, size_t end) {
	if (end < offset + 6 || end > data.size())
		Fail("Invalid VobSub private-stream PES bounds.");
	size_t cursor = offset + 6;
	if (cursor >= end)
		Fail("VobSub private-stream PES has no optional header.");

	// MPEG-2 PES optional header. The first two bits are '10'.
	if (end - cursor >= 3 && (static_cast<Byte>(data[cursor]) & 0xc0) == 0x80) {
		auto const header_length = static_cast<size_t>(static_cast<Byte>(data[cursor + 2]));
		if (header_length > end - cursor - 3)
			Fail("Truncated VobSub PES optional header.");
		cursor += 3 + header_length;
	}
	else {
		// MPEG-1 PES headers are uncommon in IDX/SUB files, but accepting them
		// costs little and avoids treating their PTS bytes as a substream id.
		while (cursor < end && static_cast<Byte>(data[cursor]) == 0xff)
			++cursor;
		if (cursor < end && (static_cast<Byte>(data[cursor]) & 0xc0) == 0x40) {
			if (end - cursor < 2)
				Fail("Truncated MPEG-1 VobSub PES STD header.");
			cursor += 2;
		}
		if (cursor >= end)
			Fail("Truncated MPEG-1 VobSub PES header.");
		auto const marker = static_cast<Byte>(data[cursor]);
		if ((marker & 0xf0) == 0x20)
			cursor += 5;
		else if ((marker & 0xf0) == 0x30)
			cursor += 10;
		else if (marker == 0x0f)
			++cursor;
		else
			Fail("Invalid MPEG-1 VobSub PES optional header.");
		if (cursor > end)
			Fail("Truncated MPEG-1 VobSub PES optional header.");
	}

	if (cursor >= end)
		Fail("VobSub PES has no substream id.");
	return {static_cast<Byte>(data[cursor]), cursor + 1, end};
}

std::string ExtractSpu(std::string_view sub_data, uint64_t filepos, int track_index) {
	if (filepos > static_cast<uint64_t>(sub_data.size()))
		Fail("VobSub file position is outside the SUB file.");
	if (filepos == static_cast<uint64_t>(sub_data.size()))
		Fail("VobSub file position points at end of SUB file.");
	if (track_index < 0 || track_index > kMaxTrackIndex)
		Fail("VobSub track index is outside the DVD substream range.");

	size_t position = static_cast<size_t>(filepos);
	Byte const wanted_substream = static_cast<Byte>(0x20 + track_index);
	std::string spu;
	size_t expected_size = 0;
	bool expected_known = false;
	bool found_substream = false;

	for (;;) {
		if (position >= sub_data.size())
			break;
		if (!IsStartCodeAt(sub_data, position)) {
			auto const next = FindStartCode(sub_data, position);
			if (next == std::string_view::npos)
				break;
			position = next;
		}

		auto const code = static_cast<Byte>(sub_data[position + 3]);
		if (code == 0xba) {
			position = ParsePackHeader(sub_data, position);
			continue;
		}
		if (code == 0xb9 || code == 0xb8) {
			position += 4;
			continue;
		}

		if (position + 6 > sub_data.size())
			Fail("Truncated VobSub MPEG start code.");
		bool const private_stream = code == 0xbd;
		auto const packet_end = ParseLengthPacket(sub_data, position, private_stream);
		if (private_stream) {
			auto const payload = ParsePrivatePes(sub_data, position, packet_end);
			if (payload.substream_id == wanted_substream) {
				found_substream = true;
				auto const fragment_size = payload.data_end - payload.data_begin;
				if (fragment_size > 0 && spu.size() < std::numeric_limits<size_t>::max() - fragment_size)
					spu.append(sub_data.substr(payload.data_begin, fragment_size));
				else if (fragment_size > 0)
					Fail("VobSub SPU size overflows.");

				if (!expected_known && spu.size() >= 2) {
					auto const short_size = secondary_subtitle_packet_io::ReadBigEndian16(spu, 0);
					if (short_size != 0) {
						expected_size = short_size;
						expected_known = true;
					}
					else if (spu.size() >= 6) {
						expected_size = secondary_subtitle_packet_io::ReadBigEndian32(spu, 2);
						expected_known = true;
					}
					if (expected_known) {
						if (expected_size < (short_size == 0 ? 6u : 2u))
							Fail("VobSub SPU declares an invalid length.");
						if (expected_size > sub_data.size())
							Fail("VobSub SPU length exceeds the SUB file.");
						spu.reserve(expected_size);
					}
				}
				if (expected_known && spu.size() >= expected_size) {
					spu.resize(expected_size);
					return spu;
				}
			}
		}
		position = packet_end;
	}

	if (!found_substream)
		Fail("No VobSub PES for the requested language at the indexed file position.");
	Fail("Truncated VobSub SPU packet.");
}
}

bool IsVobSubIndexPath(agi::fs::path const& path) {
	return agi::fs::HasExtension(path, "idx");
}

agi::fs::path GetVobSubCompanionPath(agi::fs::path const& index_path, bool uppercase_extension) {
	auto companion = index_path;
	companion.replace_extension(uppercase_extension ? ".SUB" : ".sub");
	return companion;
}

VobSubIndexInfo ParseVobSubIndex(std::string_view data) {
	return ParseIndexInternal(data);
}

VobSubIndexInfo ReadVobSubIndex(agi::fs::path const& filename) {
	return ParseVobSubIndex(secondary_subtitle_packet_io::ReadFile(filename));
}

SecondarySubtitlePacketStream ParseVobSubPacketStream(
	std::string_view idx_data,
	std::string_view sub_data,
	int track_index) {
	auto index = ParseVobSubIndex(idx_data);
	if (track_index < 0)
		track_index = index.default_track_index;
	auto const track_number = FindTrack(index.tracks, track_index);
	if (track_number < 0)
		Fail("Requested VobSub language track is not present in the IDX file.");
	auto const& track = index.tracks[static_cast<size_t>(track_number)];

	SecondarySubtitlePacketStream stream;
	stream.codec_id = kSecondarySubtitleCodecDvdSubtitle;
	stream.codec_private = index.codec_private;
	stream.fallback_canvas_width = index.fallback_canvas_width;
	stream.fallback_canvas_height = index.fallback_canvas_height;
	stream.packets.reserve(track.packets.size());
	for (auto const& entry : track.packets) {
		SecondarySubtitlePacket packet;
		packet.pts_ns = entry.pts_ns;
		packet.payload = ExtractSpu(sub_data, entry.filepos, track.index);
		stream.packets.emplace_back(std::move(packet));
	}
	if (stream.packets.empty())
		Fail("Selected VobSub language track contains no timestamped packets.");
	return stream;
}

SecondarySubtitlePacketStream ReadVobSubPacketStream(
	agi::fs::path const& filename,
	int track_index) {
	auto const idx_data = secondary_subtitle_packet_io::ReadFile(filename);
	auto sub_path = GetVobSubCompanionPath(filename);
	if (!agi::fs::FileExists(sub_path)) {
		auto upper_case = GetVobSubCompanionPath(filename, true);
		if (agi::fs::FileExists(upper_case))
			sub_path = std::move(upper_case);
	}
	auto const sub_data = secondary_subtitle_packet_io::ReadFile(sub_path);
	return ParseVobSubPacketStream(idx_data, sub_data, track_index);
}
