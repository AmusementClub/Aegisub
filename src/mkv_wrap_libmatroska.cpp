// Copyright (c) 2026, Aegisub Project
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
//
// Aegisub Project http://www.aegisub.org/

/// @file mkv_wrap_libmatroska.cpp
/// @brief High-level interface for obtaining various data from Matroska files
/// @ingroup video_input
///

#include "mkv_wrap.h"

#include "ass_file.h"
#include "ass_parser.h"
#include "compat.h"
#include "dialog_progress.h"
#include "mkv_wrap_common.h"
#include "options.h"
#include "transient_font_set.h"

#include <libaegisub/exception.h>
#include <libaegisub/format.h>
#include <libaegisub/fs.h>
#include <libaegisub/log.h>
#include <libaegisub/string_utils.h>

#include <ebml/EbmlHead.h>
#include <ebml/EbmlStream.h>
#include <ebml/IOCallback.h>

#include <matroska/KaxAttached.h>
#include <matroska/KaxAttachments.h>
#include <matroska/KaxBlock.h>
#include <matroska/KaxCluster.h>
#include <matroska/KaxSemantic.h>
#include <matroska/KaxSegment.h>
#include <matroska/KaxTracks.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <ios>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <wx/choicdlg.h> // Keep this last so wxUSE_CHOICEDLG is set.

namespace {
using libebml::EbmlElement;
using libebml::EbmlStream;

char constexpr kMkvLogSection[] = "subtitle/mkv";
std::atomic<uint64_t> next_transient_font_generation{1};

void LogMkvParserBackendOnce() {
	static std::once_flag once;
	std::call_once(once, [] {
		LOG_I(kMkvLogSection) << "Using MKV parser backend: libmatroska/libebml";
	});
}

struct Cursor {
	std::unique_ptr<EbmlElement> element;
	int upper = 0;
};

struct RawBlockPayload {
	uint64_t start_ns = 0;
	std::vector<std::string> packets;
};

Cursor next_child(EbmlStream &stream, libebml::EbmlSemanticContext const& context, bool allow_dummy = true) {
	int upper = 0;
	return { std::unique_ptr<EbmlElement>(stream.FindNextElement(context, upper, std::numeric_limits<uint64_t>::max(), allow_dummy)), upper };
}

Cursor continue_after_nested(EbmlStream &stream, libebml::EbmlSemanticContext const& parent_context, Cursor cursor) {
	if (cursor.upper > 0)
		--cursor.upper;
	if (cursor.upper < 0)
		cursor.upper = 0;
	if (!cursor.element && cursor.upper == 0)
		return next_child(stream, parent_context);
	return cursor;
}

class PathIOCallback final : public libebml::IOCallback {
	FILE *file = nullptr;
	uint64_t current_position = 0;

#ifdef _WIN32
	static int seek_file(FILE *file, int64_t offset, libebml::seek_mode mode) {
		return _fseeki64(file, offset, mode);
	}

	static int64_t tell_file(FILE *file) {
		return _ftelli64(file);
	}
#else
	static int seek_file(FILE *file, int64_t offset, libebml::seek_mode mode) {
		return fseeko(file, static_cast<off_t>(offset), mode);
	}

	static int64_t tell_file(FILE *file) {
		return ftello(file);
	}
#endif

public:
	explicit PathIOCallback(agi::fs::path const& filename) {
		auto const *mode_text = "rb";
#ifdef _WIN32
		file = _wfopen(filename.c_str(), L"rb");
#else
		auto const filename_text = agi::fs::PathToString(filename);
		file = fopen(filename_text.c_str(), mode_text);
#endif
		if (!file) {
			std::stringstream msg;
			msg << "Can't open MKV file \"" << agi::fs::PathToString(filename) << "\" in mode \"" << mode_text << "\"";
			throw std::ios_base::failure(msg.str(), std::error_code(errno, std::system_category()));
		}
	}

	~PathIOCallback() noexcept override {
		if (file && fclose(file) == 0)
			file = nullptr;
	}

	uint32 read(void *buffer, size_t size) override {
		auto const result = fread(buffer, 1, size, file);
		current_position += result;
		return static_cast<uint32>(result);
	}

	void setFilePointer(int64 offset, libebml::seek_mode mode = libebml::seek_beginning) override {
		if (seek_file(file, offset, mode) != 0) {
			std::ostringstream msg;
			msg << "Failed to seek MKV file handle to offset " << offset << " in mode " << mode;
			throw std::ios_base::failure(msg.str(), std::error_code(errno, std::system_category()));
		}

		auto const position = tell_file(file);
		if (position < 0)
			throw std::ios_base::failure("Failed to query MKV file position.", std::error_code(errno, std::system_category()));
		current_position = static_cast<uint64_t>(position);
	}

	size_t write(void const *buffer, size_t size) override {
		auto const result = fwrite(buffer, 1, size, file);
		current_position += result;
		return result;
	}

	uint64 getFilePointer() override {
		return current_position;
	}

	void close() override {
		if (!file)
			return;
		if (fclose(file) != 0)
			throw std::ios_base::failure("Can't close MKV file handle.", std::error_code(errno, std::system_category()));
		file = nullptr;
	}
};

void skip_ebml_head(EbmlStream &stream) {
	std::unique_ptr<EbmlElement> head(stream.FindNextID(EBML_INFO(libebml::EbmlHead), std::numeric_limits<uint64_t>::max()));
	if (head)
		head->SkipData(stream, EBML_CLASS_CONTEXT(libebml::EbmlHead));
}

std::unique_ptr<EbmlElement> open_segment(EbmlStream &stream) {
	skip_ebml_head(stream);
	return std::unique_ptr<EbmlElement>(stream.FindNextID(EBML_INFO(libmatroska::KaxSegment), std::numeric_limits<uint64_t>::max()));
}

uint64_t scale_track_time_ns(uint64_t value, double track_scale) {
	long double const scaled = static_cast<long double>(value) * static_cast<long double>(track_scale);
	if (scaled <= 0)
		return 0;
	if (scaled >= static_cast<long double>(std::numeric_limits<uint64_t>::max()))
		return std::numeric_limits<uint64_t>::max();
	return static_cast<uint64_t>(scaled + 0.5L);
}

uint64_t scale_block_duration_ns(uint64_t value, uint64_t segment_scale, double track_scale) {
	long double const scaled = static_cast<long double>(value) * static_cast<long double>(segment_scale) * static_cast<long double>(track_scale);
	if (scaled <= 0)
		return 0;
	if (scaled >= static_cast<long double>(std::numeric_limits<uint64_t>::max()))
		return std::numeric_limits<uint64_t>::max();
	return static_cast<uint64_t>(scaled + 0.5L);
}

int ns_to_ass_ms(uint64_t value) {
	auto const limit = static_cast<uint64_t>(std::numeric_limits<int>::max()) * 1000000ULL;
	if (value >= limit)
		return std::numeric_limits<int>::max();
	return static_cast<int>(value / 1000000ULL);
}

std::string to_lower_copy(std::string value) {
	agi::util::strings::to_lower_inplace(value);
	return value;
}

bool looks_like_font_mime_type(std::string const& mime_type) {
	auto const lowered = to_lower_copy(mime_type);
	return agi::util::strings::starts_with(lowered, "font/")
		|| agi::util::strings::contains(lowered, "truetype")
		|| agi::util::strings::contains(lowered, "opentype")
		|| agi::util::strings::contains(lowered, "sfnt");
}

bool is_supported_font_attachment(std::string const& file_name, std::string const& mime_type) {
	auto const ext = to_lower_copy(agi::fs::PathToString(agi::fs::PathFromString(file_name).extension()));
	if (ext == ".ttf" || ext == ".ttc" || ext == ".otf" || ext == ".otc" || ext == ".pfb")
		return true;
	return !mime_type.empty() && looks_like_font_mime_type(mime_type);
}

std::shared_ptr<TransientFontSet> ensure_transient_font_set(std::shared_ptr<TransientFontSet>& fonts) {
	if (!fonts) {
		fonts = std::make_shared<TransientFontSet>();
		fonts->generation = next_transient_font_generation.fetch_add(1, std::memory_order_relaxed);
	}
	return fonts;
}

void throw_if_cancelled(agi::ProgressSink *ps) {
	if (ps && ps->IsCancelled())
		throw agi::UserCancelException("Cancelled by user");
}

char const* describe_content_encoding_algorithm(MkvContentEncodingAlgorithm algorithm) {
	switch (algorithm) {
	case MkvContentEncodingAlgorithm::Zlib:
		return "zlib";
	case MkvContentEncodingAlgorithm::HeaderStripping:
		return "header-stripping";
	case MkvContentEncodingAlgorithm::Unsupported:
		break;
	}
	return "unsupported";
}

std::string describe_content_encoding_target(uint64_t scope) {
	std::vector<std::string> parts;
	if (scope & kMkvContentEncodingScopeBlock)
		parts.emplace_back("block");
	if (scope & kMkvContentEncodingScopePrivate)
		parts.emplace_back("private");
	if (scope & kMkvContentEncodingScopeNext)
		parts.emplace_back("next");
	if (parts.empty())
		return "none";

	std::string joined = parts.front();
	for (size_t i = 1; i < parts.size(); ++i) {
		joined += "+";
		joined += parts[i];
	}
	return joined;
}

std::string describe_content_encodings(std::vector<MkvContentEncoding> const& encodings) {
	if (encodings.empty())
		return "none";

	std::string joined;
	for (size_t i = 0; i < encodings.size(); ++i) {
		if (i)
			joined += ", ";
		joined += agi::format("#%u %s[%s]",
			static_cast<unsigned>(encodings[i].order),
			describe_content_encoding_algorithm(encodings[i].algorithm),
			describe_content_encoding_target(encodings[i].scope));
	}
	return joined;
}

bool has_content_encoding_target(std::vector<MkvContentEncoding> const& encodings, MkvContentEncodingTarget target) {
	auto const mask = target == MkvContentEncodingTarget::Block
		? kMkvContentEncodingScopeBlock
		: kMkvContentEncodingScopePrivate;
	return std::any_of(encodings.begin(), encodings.end(), [&](auto const& encoding) {
		return (encoding.scope & mask) != 0;
	});
}

void append_block_lines(MkvTrackInfo const& track, RawBlockPayload const& raw_block, bool have_duration, uint64_t duration_units, std::vector<std::pair<int, std::string>> &lines, int &fallback_sort_key, uint64_t segment_timecode_scale) {
	if (raw_block.packets.empty())
		return;

	auto start_ns = raw_block.start_ns;
	auto const frame_count = raw_block.packets.size();
	auto const scaled_duration = have_duration ? scale_block_duration_ns(duration_units, segment_timecode_scale, track.timecode_scale) : 0;

	for (size_t i = 0; i < frame_count; ++i) {
		uint64_t end_ns = start_ns;
		if (frame_count == 1) {
			if (have_duration)
				end_ns = start_ns + scaled_duration;
			else if (track.default_duration)
				end_ns = start_ns + track.default_duration;
		}
		else if (have_duration) {
			if (i + 1 < frame_count) {
				end_ns = start_ns + track.default_duration;
			}
			else {
				end_ns = raw_block.start_ns + scaled_duration;
			}
		}
		else if (track.default_duration) {
			end_ns = start_ns + track.default_duration;
		}

		auto line = ParseMkvTextSubtitlePacket(track.subtitle_codec, raw_block.packets[i], ns_to_ass_ms(start_ns), ns_to_ass_ms(end_ns), fallback_sort_key++);
		if (line)
			lines.emplace_back(line->sort_key, std::move(line->line));

		if (frame_count > 1 && track.default_duration)
			start_ns += track.default_duration;
	}
}

std::optional<RawBlockPayload> read_selected_block(EbmlStream &stream, libmatroska::KaxInternalBlock &block, libmatroska::KaxCluster &cluster, MkvTrackInfo const& track) {
	auto const data_start = block.GetElementPosition() + block.HeadSize();

	block.SetParent(cluster);
	block.ReadInternalHead(stream.I_O());

	auto const selected = block.TrackNum() == track.track_number;
	stream.I_O().setFilePointer(data_start, libebml::seek_beginning);

	if (!selected) {
		block.SkipData(stream, EBML_CONTEXT(&block));
		return std::nullopt;
	}

	block.ReadData(stream.I_O(), libebml::SCOPE_ALL_DATA);
	block.SetParent(cluster);

	RawBlockPayload payload;
	payload.start_ns = scale_track_time_ns(block.GlobalTimecode(), track.timecode_scale);
	payload.packets.reserve(block.NumberFrames());
	for (unsigned int i = 0; i < block.NumberFrames(); ++i) {
		auto &buffer = block.GetBuffer(i);
		std::string decoded_error;
		auto decoded = DecodeMkvContentEncodedData(
			std::string_view(reinterpret_cast<char const*>(buffer.Buffer()), buffer.Size()),
			track.content_encodings,
			MkvContentEncodingTarget::Block,
			&decoded_error);
		if (!decoded) {
			throw MatroskaException(agi::format(
				"Failed to decode MKV subtitle block for track %u: %s",
				static_cast<unsigned>(track.track_number),
				decoded_error));
		}
		payload.packets.emplace_back(std::move(*decoded));
	}
	return payload;
}

Cursor parse_content_compression(EbmlStream &stream, EbmlElement &compression_element, MkvContentEncoding &encoding, std::string &error) {
	auto const& compression_context = EBML_CONTEXT(&compression_element);
	auto cursor = next_child(stream, compression_context);

	while (cursor.element && cursor.upper <= 0) {
		auto &child = *cursor.element;

		if (EbmlId(child) == EBML_ID(libmatroska::KaxContentCompAlgo)) {
			auto &value = *static_cast<libmatroska::KaxContentCompAlgo*>(cursor.element.get());
			value.ReadData(stream.I_O());
			switch (value.GetValue()) {
			case libmatroska::MATROSKA_TRACK_ENCODING_COMP_ZLIB:
				encoding.algorithm = MkvContentEncodingAlgorithm::Zlib;
				break;
			case libmatroska::MATROSKA_TRACK_ENCODING_COMP_HEADERSTRIP:
				encoding.algorithm = MkvContentEncodingAlgorithm::HeaderStripping;
				break;
			default:
				error = agi::format("unsupported compression algorithm %u", static_cast<unsigned>(value.GetValue()));
				break;
			}
		}
		else if (EbmlId(child) == EBML_ID(libmatroska::KaxContentCompSettings)) {
			auto &value = *static_cast<libmatroska::KaxContentCompSettings*>(cursor.element.get());
			value.ReadData(stream.I_O());
			encoding.settings.assign(reinterpret_cast<char const*>(value.GetBuffer()), value.GetSize());
		}
		else {
			child.SkipData(stream, EBML_CONTEXT(&child));
		}

		cursor = next_child(stream, compression_context);
	}

	return cursor;
}

Cursor parse_content_encoding(EbmlStream &stream, EbmlElement &encoding_element, MkvContentEncoding &encoding, std::string &error) {
	auto const& encoding_context = EBML_CONTEXT(&encoding_element);
	auto cursor = next_child(stream, encoding_context);

	while (cursor.element && cursor.upper <= 0) {
		auto &child = *cursor.element;

		if (EbmlId(child) == EBML_ID(libmatroska::KaxContentEncodingOrder)) {
			auto &value = *static_cast<libmatroska::KaxContentEncodingOrder*>(cursor.element.get());
			value.ReadData(stream.I_O());
			encoding.order = value.GetValue();
		}
		else if (EbmlId(child) == EBML_ID(libmatroska::KaxContentEncodingScope)) {
			auto &value = *static_cast<libmatroska::KaxContentEncodingScope*>(cursor.element.get());
			value.ReadData(stream.I_O());
			encoding.scope = value.GetValue();
		}
		else if (EbmlId(child) == EBML_ID(libmatroska::KaxContentEncodingType)) {
			auto &value = *static_cast<libmatroska::KaxContentEncodingType*>(cursor.element.get());
			value.ReadData(stream.I_O());
			if (value.GetValue() != libmatroska::MATROSKA_CONTENTENCODINGTYPE_COMPRESSION)
				error = "encryption content encodings are not supported";
		}
		else if (EbmlId(child) == EBML_ID(libmatroska::KaxContentCompression)) {
			auto nested = parse_content_compression(stream, *cursor.element, encoding, error);
			cursor = continue_after_nested(stream, encoding_context, std::move(nested));
			continue;
		}
		else {
			child.SkipData(stream, EBML_CONTEXT(&child));
		}

		cursor = next_child(stream, encoding_context);
	}

	return cursor;
}

Cursor parse_content_encodings(EbmlStream &stream, EbmlElement &encodings_element, MkvTrackInfo &track) {
	auto const& encodings_context = EBML_CONTEXT(&encodings_element);
	auto cursor = next_child(stream, encodings_context);

	while (cursor.element && cursor.upper <= 0) {
		if (EbmlId(*cursor.element) == EBML_ID(libmatroska::KaxContentEncoding)) {
			MkvContentEncoding encoding;
			std::string error;
			auto nested = parse_content_encoding(stream, *cursor.element, encoding, error);
			if (!error.empty() && track.unsupported_content_encoding_reason.empty())
				track.unsupported_content_encoding_reason = error;
			else if (track.unsupported_content_encoding_reason.empty())
				track.content_encodings.emplace_back(std::move(encoding));
			cursor = continue_after_nested(stream, encodings_context, std::move(nested));
		}
		else {
			cursor.element->SkipData(stream, EBML_CONTEXT(cursor.element.get()));
			cursor = next_child(stream, encodings_context);
		}
	}

	std::stable_sort(track.content_encodings.begin(), track.content_encodings.end(), [](auto const& left, auto const& right) {
		return left.order > right.order;
	});

	return cursor;
}

Cursor parse_attached_file(EbmlStream &stream, EbmlElement &attached_element, std::shared_ptr<TransientFontSet>& fonts) {
	auto const& attached_context = EBML_CONTEXT(&attached_element);
	auto cursor = next_child(stream, attached_context);

	std::string file_name;
	std::string mime_type;
	std::vector<char> data;
	std::optional<bool> supported_font;

	auto update_supported_font = [&] {
		if (file_name.empty() && mime_type.empty())
			return;
		supported_font = is_supported_font_attachment(file_name, mime_type);
	};

	while (cursor.element && cursor.upper <= 0) {
		auto &child = *cursor.element;

		if (EbmlId(child) == EBML_ID(libmatroska::KaxFileName)) {
			auto &value = *static_cast<libmatroska::KaxFileName*>(cursor.element.get());
			value.ReadData(stream.I_O());
			file_name = value.GetValueUTF8();
			update_supported_font();
		}
		else if (EbmlId(child) == EBML_ID(libmatroska::KaxMimeType)) {
			auto &value = *static_cast<libmatroska::KaxMimeType*>(cursor.element.get());
			value.ReadData(stream.I_O());
			mime_type = value.GetValue();
			update_supported_font();
		}
		else if (EbmlId(child) == EBML_ID(libmatroska::KaxFileData)) {
			if (supported_font && !*supported_font) {
				child.SkipData(stream, EBML_CONTEXT(&child));
			}
			else {
				auto &value = *static_cast<libmatroska::KaxFileData*>(cursor.element.get());
				value.ReadData(stream.I_O());
				data.assign(reinterpret_cast<char const*>(value.GetBuffer()), reinterpret_cast<char const*>(value.GetBuffer()) + value.GetSize());
			}
		}
		else {
			child.SkipData(stream, EBML_CONTEXT(&child));
		}

		cursor = next_child(stream, attached_context);
	}

	auto const attachment_is_supported = supported_font.value_or(is_supported_font_attachment(file_name, mime_type));
	if (!file_name.empty() && !data.empty()) {
		if (attachment_is_supported) {
			auto font_set = ensure_transient_font_set(fonts);
			font_set->fonts.push_back({ file_name, mime_type, std::move(data) });
			LOG_I(kMkvLogSection) << "Collected MKV font attachment: " << file_name
				<< (mime_type.empty() ? "" : agi::format(" (%s)", mime_type));
		}
		else {
			LOG_D(kMkvLogSection) << "Ignoring non-font MKV attachment: " << file_name;
		}
	}
	else if (!file_name.empty() && !attachment_is_supported) {
		LOG_D(kMkvLogSection) << "Ignoring non-font MKV attachment: " << file_name;
	}

	return cursor;
}

Cursor parse_attachments(EbmlStream &stream, EbmlElement &attachments_element, std::shared_ptr<TransientFontSet>& fonts) {
	auto const& attachments_context = EBML_CONTEXT(&attachments_element);
	auto cursor = next_child(stream, attachments_context);

	while (cursor.element && cursor.upper <= 0) {
		if (EbmlId(*cursor.element) == EBML_ID(libmatroska::KaxAttached)) {
			auto nested = parse_attached_file(stream, *cursor.element, fonts);
			cursor = continue_after_nested(stream, attachments_context, std::move(nested));
		}
		else {
			cursor.element->SkipData(stream, EBML_CONTEXT(cursor.element.get()));
			cursor = next_child(stream, attachments_context);
		}
	}

	return cursor;
}

Cursor parse_track_audio(EbmlStream &stream, EbmlElement &audio_element, MkvTrackInfo &track) {
	auto const& audio_context = EBML_CONTEXT(&audio_element);
	auto cursor = next_child(stream, audio_context);

	while (cursor.element && cursor.upper <= 0) {
		auto &child = *cursor.element;

		if (EbmlId(child) == EBML_ID(libmatroska::KaxAudioChannels)) {
			auto &value = *static_cast<libmatroska::KaxAudioChannels*>(cursor.element.get());
			value.ReadData(stream.I_O());
			if (value.GetValue() <= static_cast<uint64_t>(std::numeric_limits<int>::max()))
				track.audio_channels = static_cast<int>(value.GetValue());
		}
		else {
			child.SkipData(stream, EBML_CONTEXT(&child));
		}

		cursor = next_child(stream, audio_context);
	}

	return cursor;
}

Cursor parse_track_entry(EbmlStream &stream, EbmlElement &entry_element, MkvTrackInfo &track) {
	auto const& entry_context = EBML_CONTEXT(&entry_element);
	auto cursor = next_child(stream, entry_context);

	while (cursor.element && cursor.upper <= 0) {
		auto &child = *cursor.element;

		if (EbmlId(child) == EBML_ID(libmatroska::KaxTrackNumber)) {
			auto &value = *static_cast<libmatroska::KaxTrackNumber*>(cursor.element.get());
			value.ReadData(stream.I_O());
			track.track_number = value.GetValue();
		}
		else if (EbmlId(child) == EBML_ID(libmatroska::KaxTrackType)) {
			auto &value = *static_cast<libmatroska::KaxTrackType*>(cursor.element.get());
			value.ReadData(stream.I_O());
			track.type = ClassifyMkvTrackType(value.GetValue());
		}
		else if (EbmlId(child) == EBML_ID(libmatroska::KaxCodecID)) {
			auto &value = *static_cast<libmatroska::KaxCodecID*>(cursor.element.get());
			value.ReadData(stream.I_O());
			track.codec_id = value.GetValue();
			track.subtitle_codec = ClassifyMkvTextSubtitleCodec(track.codec_id);
		}
		else if (EbmlId(child) == EBML_ID(libmatroska::KaxTrackLanguage)) {
			auto &value = *static_cast<libmatroska::KaxTrackLanguage*>(cursor.element.get());
			value.ReadData(stream.I_O());
			track.language = value.GetValue();
		}
		else if (EbmlId(child) == EBML_ID(libmatroska::KaxLanguageIETF)) {
			auto &value = *static_cast<libmatroska::KaxLanguageIETF*>(cursor.element.get());
			value.ReadData(stream.I_O());
			track.language_ietf = value.GetValue();
		}
		else if (EbmlId(child) == EBML_ID(libmatroska::KaxTrackName)) {
			auto &value = *static_cast<libmatroska::KaxTrackName*>(cursor.element.get());
			value.ReadData(stream.I_O());
			track.name = value.GetValueUTF8();
		}
		else if (EbmlId(child) == EBML_ID(libmatroska::KaxTrackTimecodeScale)) {
			auto &value = *static_cast<libmatroska::KaxTrackTimecodeScale*>(cursor.element.get());
			value.ReadData(stream.I_O());
			track.timecode_scale = value.GetValue();
		}
		else if (EbmlId(child) == EBML_ID(libmatroska::KaxTrackDefaultDuration)) {
			auto &value = *static_cast<libmatroska::KaxTrackDefaultDuration*>(cursor.element.get());
			value.ReadData(stream.I_O());
			track.default_duration = value.GetValue();
		}
		else if (EbmlId(child) == EBML_ID(libmatroska::KaxCodecPrivate)) {
			auto &value = *static_cast<libmatroska::KaxCodecPrivate*>(cursor.element.get());
			value.ReadData(stream.I_O());
			track.codec_private.assign(reinterpret_cast<char const*>(value.GetBuffer()), value.GetSize());
		}
		else if (EbmlId(child) == EBML_ID(libmatroska::KaxContentEncodings)) {
			auto nested = parse_content_encodings(stream, child, track);
			cursor = continue_after_nested(stream, entry_context, std::move(nested));
			continue;
		}
		else if (EbmlId(child) == EBML_ID(libmatroska::KaxTrackAudio)) {
			auto nested = parse_track_audio(stream, child, track);
			cursor = continue_after_nested(stream, entry_context, std::move(nested));
			continue;
		}
		else {
			child.SkipData(stream, EBML_CONTEXT(&child));
		}

		cursor = next_child(stream, entry_context);
	}

	return cursor;
}

Cursor parse_tracks(EbmlStream &stream, EbmlElement &tracks_element, MkvTrackScanResult &result) {
	auto const& tracks_context = EBML_CONTEXT(&tracks_element);
	auto cursor = next_child(stream, tracks_context);
	int video_count = 0;
	int audio_count = 0;
	int subtitle_count = 0;
	int other_count = 0;

	while (cursor.element && cursor.upper <= 0) {
		if (EbmlId(*cursor.element) == EBML_ID(libmatroska::KaxTrackEntry)) {
			MkvTrackInfo track;
			auto nested = parse_track_entry(stream, *cursor.element, track);
			track.global_ordinal = static_cast<int>(result.tracks.size());
			switch (track.type) {
			case MkvTrackType::Video:
				track.type_ordinal = video_count++;
				break;
			case MkvTrackType::Audio:
				track.type_ordinal = audio_count++;
				break;
			case MkvTrackType::Subtitle:
				track.type_ordinal = subtitle_count++;
				break;
			case MkvTrackType::Other:
				track.type_ordinal = other_count++;
				break;
			}
			result.tracks.emplace_back(std::move(track));

			cursor = continue_after_nested(stream, tracks_context, std::move(nested));
		}
		else {
			cursor.element->SkipData(stream, EBML_CONTEXT(cursor.element.get()));
			cursor = next_child(stream, tracks_context);
		}
	}

	return cursor;
}

Cursor parse_info(EbmlStream &stream, EbmlElement &info_element, MkvTrackScanResult &result) {
	auto const& info_context = EBML_CONTEXT(&info_element);
	auto cursor = next_child(stream, info_context);

	while (cursor.element && cursor.upper <= 0) {
		if (EbmlId(*cursor.element) == EBML_ID(libmatroska::KaxTimecodeScale)) {
			auto &value = *static_cast<libmatroska::KaxTimecodeScale*>(cursor.element.get());
			value.ReadData(stream.I_O());
			result.segment_timecode_scale = value.GetValue();
		}
		else {
			cursor.element->SkipData(stream, EBML_CONTEXT(cursor.element.get()));
		}

		cursor = next_child(stream, info_context);
	}

	return cursor;
}

MkvTrackScanResult scan_tracks(agi::fs::path const& filename) {
	PathIOCallback input(filename);
	EbmlStream stream(input);
	auto segment = open_segment(stream);
	if (!segment)
		throw MatroskaException("File is not a Matroska file.");

	MkvTrackScanResult result;
	auto const& segment_context = EBML_CONTEXT(segment.get());
	auto cursor = next_child(stream, segment_context);

	while (cursor.element && cursor.upper <= 0) {
		if (EbmlId(*cursor.element) == EBML_ID(libmatroska::KaxInfo)) {
			cursor = continue_after_nested(stream, segment_context, parse_info(stream, *cursor.element, result));
		}
		else if (EbmlId(*cursor.element) == EBML_ID(libmatroska::KaxTracks)) {
			cursor = continue_after_nested(stream, segment_context, parse_tracks(stream, *cursor.element, result));
		}
		else if (EbmlId(*cursor.element) == EBML_ID(libmatroska::KaxCluster)) {
			break;
		}
		else {
			cursor.element->SkipData(stream, EBML_CONTEXT(cursor.element.get()));
			cursor = next_child(stream, segment_context);
		}
	}

	return result;
}

std::vector<MkvTrackInfo const*> collect_importable_subtitle_tracks(MkvTrackScanResult const& scan, bool log_tracks) {
	std::vector<MkvTrackInfo const*> tracks;
	tracks.reserve(scan.tracks.size());

	for (auto const& track : scan.tracks) {
		if (track.type != MkvTrackType::Subtitle)
			continue;

		if (!track.codec_id.empty() && track.subtitle_codec == MkvTextSubtitleCodec::Unsupported) {
			if (log_tracks)
				LOG_I(kMkvLogSection) << "Skipping MKV subtitle track " << track.track_number << " with unsupported codec " << track.codec_id;
		}
		else if (!track.unsupported_content_encoding_reason.empty()) {
			if (log_tracks) {
				LOG_I(kMkvLogSection) << "Skipping MKV subtitle track " << track.track_number << " (" << track.codec_id
					<< ") because content encodings are unsupported: " << track.unsupported_content_encoding_reason;
			}
		}
		else if (IsImportableMkvSubtitleTrack(track)) {
			if (log_tracks) {
				LOG_I(kMkvLogSection) << "Found importable MKV subtitle track " << track.track_number << " (" << track.codec_id
					<< "), content encodings: " << describe_content_encodings(track.content_encodings);
			}
			tracks.push_back(&track);
		}
	}

	return tracks;
}

Cursor parse_block_group(EbmlStream &stream, EbmlElement &group_element, libmatroska::KaxCluster &cluster, MkvTrackInfo const& track, std::vector<std::pair<int, std::string>> &lines, int &fallback_sort_key, uint64_t segment_timecode_scale) {
	auto const& group_context = EBML_CONTEXT(&group_element);
	auto cursor = next_child(stream, group_context);

	std::optional<RawBlockPayload> raw_block;
	uint64_t duration_units = 0;
	bool have_duration = false;

	while (cursor.element && cursor.upper <= 0) {
		if (EbmlId(*cursor.element) == EBML_ID(libmatroska::KaxBlockDuration)) {
			auto &duration = *static_cast<libmatroska::KaxBlockDuration*>(cursor.element.get());
			duration.ReadData(stream.I_O());
			duration_units = duration.GetValue();
			have_duration = true;
		}
		else if (EbmlId(*cursor.element) == EBML_ID(libmatroska::KaxBlock)) {
			auto &block = *static_cast<libmatroska::KaxBlock*>(cursor.element.get());
			raw_block = read_selected_block(stream, block, cluster, track);
		}
		else {
			cursor.element->SkipData(stream, EBML_CONTEXT(cursor.element.get()));
		}

		cursor = next_child(stream, group_context);
	}

	if (raw_block)
		append_block_lines(track, *raw_block, have_duration, duration_units, lines, fallback_sort_key, segment_timecode_scale);

	return cursor;
}

void import_track(agi::fs::path const& filename, MkvTrackInfo const& track, uint64_t segment_timecode_scale, agi::ProgressSink *ps, std::vector<std::pair<int, std::string>> &lines, std::shared_ptr<TransientFontSet>& transient_fonts) {
	PathIOCallback input(filename);
	EbmlStream stream(input);
	auto segment = open_segment(stream);
	if (!segment)
		throw MatroskaException("File is not a Matroska file.");

	auto const file_size = std::max<uint64_t>(agi::fs::Size(filename), 1);
	auto const& segment_context = EBML_CONTEXT(segment.get());
	auto cursor = next_child(stream, segment_context);
	int fallback_sort_key = 0;

	while (cursor.element && cursor.upper <= 0) {
		throw_if_cancelled(ps);

		if (track.subtitle_codec != MkvTextSubtitleCodec::Utf8 && EbmlId(*cursor.element) == EBML_ID(libmatroska::KaxAttachments)) {
			cursor = continue_after_nested(stream, segment_context, parse_attachments(stream, *cursor.element, transient_fonts));
		}
		else if (EbmlId(*cursor.element) == EBML_ID(libmatroska::KaxCluster)) {
			auto &cluster = *static_cast<libmatroska::KaxCluster*>(cursor.element.get());
			auto const& cluster_context = EBML_CONTEXT(cursor.element.get());
			auto child = next_child(stream, cluster_context);
			bool cluster_initialized = false;

			while (child.element && child.upper <= 0) {
				throw_if_cancelled(ps);

				if (EbmlId(*child.element) == EBML_ID(libmatroska::KaxClusterTimecode)) {
					auto &timestamp = *static_cast<libmatroska::KaxClusterTimecode*>(child.element.get());
					timestamp.ReadData(stream.I_O());
					cluster.InitTimecode(timestamp.GetValue(), segment_timecode_scale);
					cluster_initialized = true;
					child = next_child(stream, cluster_context);
				}
				else if (EbmlId(*child.element) == EBML_ID(libmatroska::KaxBlockGroup)) {
					if (cluster_initialized)
						child = continue_after_nested(stream, cluster_context, parse_block_group(stream, *child.element, cluster, track, lines, fallback_sort_key, segment_timecode_scale));
					else {
						child.element->SkipData(stream, EBML_CONTEXT(child.element.get()));
						child = next_child(stream, cluster_context);
					}
				}
				else if (EbmlId(*child.element) == EBML_ID(libmatroska::KaxSimpleBlock)) {
					if (cluster_initialized) {
						auto &block = *static_cast<libmatroska::KaxSimpleBlock*>(child.element.get());
						auto raw_block = read_selected_block(stream, block, cluster, track);
						if (raw_block)
							append_block_lines(track, *raw_block, false, 0, lines, fallback_sort_key, segment_timecode_scale);
						child = next_child(stream, cluster_context);
					}
					else {
						child.element->SkipData(stream, EBML_CONTEXT(child.element.get()));
						child = next_child(stream, cluster_context);
					}
				}
				else {
					child.element->SkipData(stream, EBML_CONTEXT(child.element.get()));
					child = next_child(stream, cluster_context);
				}

				if (ps)
					ps->SetProgress(stream.I_O().getFilePointer(), file_size);
			}

			cursor = continue_after_nested(stream, segment_context, std::move(child));
		}
		else {
			cursor.element->SkipData(stream, EBML_CONTEXT(cursor.element.get()));
			cursor = next_child(stream, segment_context);
		}
	}
}
}

MkvTrackScanResult MatroskaWrapper::ScanTracks(agi::fs::path const& filename) {
	LogMkvParserBackendOnce();
	return scan_tracks(filename);
}

void MatroskaWrapper::GetSubtitles(agi::fs::path const& filename, AssFile *target) {
	LogMkvParserBackendOnce();
	target->SetTransientFonts({});

	auto scan = scan_tracks(filename);
	auto subtitle_tracks = collect_importable_subtitle_tracks(scan, true);
	if (subtitle_tracks.empty())
		throw MatroskaException("File has no recognised subtitle tracks.");

	MkvTrackInfo const* selected_track = subtitle_tracks.front();
	if (subtitle_tracks.size() > 1) {
		std::vector<std::string> choices;
		choices.reserve(subtitle_tracks.size());
		for (auto const* track : subtitle_tracks)
			choices.emplace_back(DescribeMkvTrack(*track));

		int choice = wxGetSingleChoiceIndex(_("Choose which track to read:"), _("Multiple subtitle tracks found"), to_wx(choices));
		if (choice == -1)
			throw agi::UserCancelException("canceled");

		selected_track = subtitle_tracks[choice];
	}

	LOG_I(kMkvLogSection) << "Importing MKV subtitle track " << selected_track->track_number << " (" << selected_track->codec_id << ") from " << agi::fs::PathToString(filename);
	if (!selected_track->content_encodings.empty())
		LOG_I(kMkvLogSection) << "Decoding MKV content encodings for track " << selected_track->track_number << ": " << describe_content_encodings(selected_track->content_encodings);

	AssParser parser(target, selected_track->subtitle_codec != MkvTextSubtitleCodec::Ssa);
	if (selected_track->subtitle_codec == MkvTextSubtitleCodec::Utf8) {
		target->LoadDefault(false, OPT_GET("Subtitle Format/SRT/Default Style Catalog")->GetString());
	}
	else {
		std::string decoded_error;
		auto decoded_codec_private = DecodeMkvContentEncodedData(
			selected_track->codec_private,
			selected_track->content_encodings,
			MkvContentEncodingTarget::Private,
			&decoded_error);
		if (!decoded_codec_private) {
			throw MatroskaException(agi::format(
				"Failed to decode MKV CodecPrivate for track %u: %s",
				static_cast<unsigned>(selected_track->track_number),
				decoded_error));
		}
		if (has_content_encoding_target(selected_track->content_encodings, MkvContentEncodingTarget::Private)) {
			LOG_I(kMkvLogSection) << "Decoded MKV CodecPrivate for track " << selected_track->track_number;
		}
		for (auto const& line : SplitMkvCodecPrivateLines(*decoded_codec_private))
			parser.AddLine(line);
	}
	parser.AddLine("[Events]");

	std::vector<std::pair<int, std::string>> lines;
	std::shared_ptr<TransientFontSet> transient_fonts;
	std::string error;

	DialogProgress progress(nullptr, _("Parsing Matroska"), _("Reading subtitles from Matroska file."));
	progress.Run([&](agi::ProgressSink *ps) {
		try {
			import_track(filename, *selected_track, scan.segment_timecode_scale, ps, lines, transient_fonts);
		}
		catch (agi::UserCancelException const&) {
			throw;
		}
		catch (agi::Exception const& e) {
			error = e.GetMessage();
			ps->Log(error);
		}
		catch (std::exception const& e) {
			error = e.what();
			ps->Log(error);
		}
		catch (...) {
			error = "Unknown Matroska parsing error.";
			ps->Log(error);
		}
	});

	if (!error.empty())
		throw MatroskaException(error);

	std::stable_sort(lines.begin(), lines.end(), [](auto const& left, auto const& right) {
		return left.first < right.first;
	});

	for (auto &line : lines)
		parser.AddLine(line.second);

	target->SetTransientFonts(transient_fonts);
	if (transient_fonts && !transient_fonts->empty()) {
		LOG_I(kMkvLogSection) << "Collected " << transient_fonts->fonts.size()
			<< " transient MKV font attachment(s) for track " << selected_track->track_number
			<< ", generation " << transient_fonts->generation;
	}

	LOG_I(kMkvLogSection) << "Imported " << lines.size() << " subtitle lines from MKV track " << selected_track->track_number;
}

bool MatroskaWrapper::HasSubtitles(agi::fs::path const& filename) {
	LogMkvParserBackendOnce();

	try {
		return !collect_importable_subtitle_tracks(scan_tracks(filename), false).empty();
	}
	catch (...) {
		return false;
	}
}
