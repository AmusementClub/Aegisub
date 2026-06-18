#include <aegisub/core_c_api.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" int aegisub_core_c_header_smoke(void);

namespace {

class ScopedFile {
	std::filesystem::path path;

public:
	explicit ScopedFile(std::filesystem::path path) : path(std::move(path)) { }
	~ScopedFile() {
		std::error_code ec;
		std::filesystem::remove(path, ec);
	}

	std::filesystem::path const& get() const { return path; }
};

std::filesystem::path MakeTempAssPath() {
	auto const stamp = std::chrono::steady_clock::now().time_since_epoch().count();
	return std::filesystem::temp_directory_path() / ("aegisub_core_c_api_" + std::to_string(stamp) + ".ass");
}

void WriteSmokeAss(std::filesystem::path const& path) {
	std::ofstream file(path, std::ios::binary);
	if (!file)
		throw std::runtime_error("failed to create C API smoke ASS file");

	file <<
		"[Script Info]\n"
		"ScriptType: v4.00+\n"
		"PlayResX: 1280\n"
		"PlayResY: 720\n"
		"\n"
		"[V4+ Styles]\n"
		"Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, "
		"Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, "
		"Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\n"
		"Style: Default,Arial,20,&H00FFFFFF,&H000000FF,&H00000000,&H80000000,"
		"0,0,0,0,100,100,0,0,1,2,2,2,10,10,10,1\n"
		"\n"
		"[Events]\n"
		"Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
		"Dialogue: 0,0:00:01.00,0:00:02.50,Default,Actor,0010,0020,0030,,Hello core\n"
		"Comment: 1,0:00:03.00,0:00:04.00,Default,,0000,0000,0000,fx,Hidden note\n";
}

void WriteEditorOpsAss(std::filesystem::path const& path) {
	std::ofstream file(path, std::ios::binary);
	if (!file)
		throw std::runtime_error("failed to create C API editor-ops ASS file");

	file <<
		"[Script Info]\n"
		"ScriptType: v4.00+\n"
		"PlayResX: 1280\n"
		"PlayResY: 720\n"
		"\n"
		"[V4+ Styles]\n"
		"Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, "
		"Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, "
		"Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\n"
		"Style: Default,Arial,20,&H00FFFFFF,&H000000FF,&H00000000,&H80000000,"
		"0,0,0,0,100,100,0,0,1,2,2,2,10,10,10,1\n"
		"\n"
		"[Events]\n"
		"Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
		"Dialogue: 0,0:00:05.00,0:00:06.00,Default,Echo,0010,0020,0030,,Alpha\n"
		"Dialogue: 0,0:00:01.00,0:00:02.00,Default,Delta,0011,0021,0031,,Bravo\n"
		"Dialogue: 0,0:00:04.00,0:00:05.00,Default,Charlie,0012,0022,0032,,Charlie\n"
		"Dialogue: 0,0:00:03.00,0:00:04.00,Default,Bravo,0013,0023,0033,,Delta\n"
		"Dialogue: 0,0:00:02.00,0:00:03.00,Default,Alpha,0014,0024,0034,,Echo\n";
}

std::string ToString(aegisub_core_string value) {
	return std::string(value.data ? value.data : "", value.size);
}

std::string TakeString(aegisub_core_owned_string value) {
	std::string copy(value.data ? value.data : "", value.size);
	aegisub_core_free_string(value);
	return copy;
}

void RequireStatus(aegisub_core_status actual, aegisub_core_status expected, char const* context) {
	if (actual != expected)
		throw std::runtime_error(std::string(context) + " returned unexpected status");
}

std::vector<std::string> ReadSubtitleTexts(aegisub_core_subtitle_session const *session) {
	auto const count = aegisub_core_subtitle_row_count(session);
	std::vector<aegisub_core_subtitle_row> rows(count);
	size_t written = 0;
	RequireStatus(aegisub_core_subtitle_rows_get(session, 0, count, rows.data(), &written),
		AEGISUB_CORE_STATUS_OK,
		"subtitle text snapshot rows get");
	if (written != count)
		throw std::runtime_error("subtitle text snapshot row count was not stable");

	std::vector<std::string> texts;
	texts.reserve(rows.size());
	for (auto const& row : rows)
		texts.push_back(ToString(row.text));
	return texts;
}

void RequireTexts(aegisub_core_subtitle_session const *session,
                  std::vector<std::string> const& expected,
                  char const *context) {
	auto actual = ReadSubtitleTexts(session);
	if (actual != expected)
		throw std::runtime_error(std::string(context) + " produced unexpected row order");
}

[[nodiscard]] aegisub_core_subtitle_session *OpenSubtitleForSmoke(
	aegisub_core_context *context,
	std::filesystem::path const& path,
	char const *context_name) {
	auto path_text = path.string();
	aegisub_core_subtitle_open_options options{};
	options.abi_version = AEGISUB_CORE_ABI_VERSION;
	options.path = {path_text.data(), path_text.size()};
	options.encoding = {"utf-8", 5};

	aegisub_core_subtitle_session *session = nullptr;
	RequireStatus(aegisub_core_subtitle_open(context, &options, &session),
		AEGISUB_CORE_STATUS_OK,
		context_name);
	if (!session)
		throw std::runtime_error(std::string(context_name) + " returned null session");
	return session;
}

bool CatalogHasProvider(aegisub_core_provider_catalog const *catalog, char const *name, uint8_t hidden, uint8_t available) {
	auto const count = aegisub_core_provider_catalog_count(catalog);
	std::vector<aegisub_core_provider_descriptor> descriptors(count);
	size_t written = 0;
	RequireStatus(aegisub_core_provider_catalog_descriptors_get(
		catalog,
		0,
		count,
		descriptors.data(),
		&written),
		AEGISUB_CORE_STATUS_OK,
		"provider catalog descriptors get");
	if (written != count)
		throw std::runtime_error("provider catalog descriptor window count was not stable");

	for (auto const& descriptor : descriptors) {
		if (ToString(descriptor.name) == name
			&& descriptor.hidden == hidden
			&& descriptor.available == available)
			return true;
	}
	return false;
}

void RequireCatalog(aegisub_core_context *context,
                    aegisub_core_provider_kind kind,
                    char const *preferred,
                    char const *required_name,
                    uint8_t hidden,
                    char const *context_name) {
	aegisub_core_provider_catalog *catalog = nullptr;
	aegisub_core_string preferred_view{preferred, std::strlen(preferred)};
	RequireStatus(aegisub_core_provider_catalog_get(context, kind, preferred_view, &catalog),
		AEGISUB_CORE_STATUS_OK,
		context_name);
	if (!catalog)
		throw std::runtime_error(std::string(context_name) + " returned null catalog");
	if (!CatalogHasProvider(catalog, required_name, hidden, 1))
		throw std::runtime_error(std::string(context_name) + " did not include required provider");
	aegisub_core_provider_catalog_destroy(catalog);

	if (!TakeString(aegisub_core_context_last_error(context)).empty())
		throw std::runtime_error(std::string(context_name) + " left stale last_error after success");
}

void RequireProviderOpenReport(aegisub_core_context *context,
                               aegisub_core_provider_kind kind,
                               char const *expected_provider,
                               char const *context_name) {
	aegisub_core_provider_open_report *bad_report = reinterpret_cast<aegisub_core_provider_open_report *>(0x1);
	RequireStatus(aegisub_core_provider_open_report_get(nullptr, kind, &bad_report),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"null context provider open report get");
	if (bad_report)
		throw std::runtime_error("null context provider open report get did not clear output report");
	RequireStatus(aegisub_core_provider_open_report_get(context, kind, nullptr),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"null output provider open report get");
	RequireStatus(aegisub_core_provider_open_report_info_get(nullptr, nullptr),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"null provider open report info get");
	if (aegisub_core_provider_open_report_attempt_count(nullptr) != 0)
		throw std::runtime_error("null provider open report reported attempts");
	RequireStatus(aegisub_core_provider_open_report_attempt_get_at(nullptr, 0, nullptr),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"null provider open report attempt get");
	aegisub_core_provider_open_attempt attempt_window[8]{};
	size_t attempts_written = 99;
	RequireStatus(aegisub_core_provider_open_report_attempts_get(nullptr, 0, 1, attempt_window, &attempts_written),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"null provider open report attempt window get");
	if (attempts_written != 0)
		throw std::runtime_error("null provider open report attempt window did not clear written count");

	aegisub_core_provider_open_report *report = nullptr;
	RequireStatus(aegisub_core_provider_open_report_get(context, kind, &report),
		AEGISUB_CORE_STATUS_OK,
		context_name);
	if (!report)
		throw std::runtime_error(std::string(context_name) + " returned null report");

	aegisub_core_provider_open_report_info info{};
	RequireStatus(aegisub_core_provider_open_report_info_get(report, &info),
		AEGISUB_CORE_STATUS_OK,
		"provider open report info get");
	if (info.kind != kind
		|| ToString(info.preferred_provider) != expected_provider
		|| ToString(info.selected_provider) != expected_provider
		|| info.attempt_count != aegisub_core_provider_open_report_attempt_count(report)
		|| info.attempt_count == 0)
		throw std::runtime_error(std::string(context_name) + " info was not stable");

	aegisub_core_provider_open_attempt attempt{};
	RequireStatus(aegisub_core_provider_open_report_attempt_get_at(report, info.attempt_count - 1, &attempt),
		AEGISUB_CORE_STATUS_OK,
		"provider open report last attempt get");
	if (ToString(attempt.provider_name) != expected_provider || ToString(attempt.outcome) != "opened")
		throw std::runtime_error(std::string(context_name) + " last attempt was not stable");
	std::vector<aegisub_core_provider_open_attempt> attempts(info.attempt_count);
	RequireStatus(aegisub_core_provider_open_report_attempts_get(
		report,
		0,
		info.attempt_count,
		attempts.data(),
		&attempts_written),
		AEGISUB_CORE_STATUS_OK,
		"provider open report attempt window get");
	if (attempts_written != info.attempt_count
		|| ToString(attempts.back().provider_name) != expected_provider
		|| ToString(attempts.back().outcome) != "opened")
		throw std::runtime_error(std::string(context_name) + " attempt window was not stable");
	RequireStatus(aegisub_core_provider_open_report_attempts_get(
		report,
		info.attempt_count - 1,
		8,
		attempt_window,
		&attempts_written),
		AEGISUB_CORE_STATUS_OK,
		"provider open report clipped attempt window get");
	if (attempts_written != 1 || ToString(attempt_window[0].provider_name) != expected_provider)
		throw std::runtime_error(std::string(context_name) + " clipped attempt window was not stable");
	RequireStatus(aegisub_core_provider_open_report_attempts_get(
		report,
		info.attempt_count,
		1,
		attempt_window,
		&attempts_written),
		AEGISUB_CORE_STATUS_OK,
		"provider open report empty tail attempt window get");
	if (attempts_written != 0)
		throw std::runtime_error(std::string(context_name) + " empty tail attempt window wrote attempts");
	RequireStatus(aegisub_core_provider_open_report_attempts_get(
		report,
		info.attempt_count + 1,
		1,
		attempt_window,
		&attempts_written),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"provider open report out-of-range attempt window get");
	if (attempts_written != 0)
		throw std::runtime_error(std::string(context_name) + " invalid attempt window did not clear written count");
	RequireStatus(aegisub_core_provider_open_report_attempts_get(report, 0, 1, nullptr, &attempts_written),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"provider open report null attempt window output");
	RequireStatus(aegisub_core_provider_open_report_attempts_get(report, 0, 0, nullptr, &attempts_written),
		AEGISUB_CORE_STATUS_OK,
		"provider open report zero attempt window with null output");
	if (attempts_written != 0)
		throw std::runtime_error(std::string(context_name) + " zero attempt window wrote attempts");
	RequireStatus(aegisub_core_provider_open_report_attempts_get(report, 0, 1, attempt_window, nullptr),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"provider open report null attempt window written output");
	RequireStatus(aegisub_core_provider_open_report_info_get(report, nullptr),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"provider open report null info get");
	RequireStatus(aegisub_core_provider_open_report_attempt_get_at(report, info.attempt_count, &attempt),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"provider open report out-of-range attempt get");
	RequireStatus(aegisub_core_provider_open_report_attempt_get_at(report, 0, nullptr),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"provider open report null attempt get");
	aegisub_core_provider_open_report_destroy(report);
}

void RequireDummyVideoOpen(aegisub_core_context *context) {
	aegisub_core_video_session *bad_session = reinterpret_cast<aegisub_core_video_session *>(0x1);
	RequireStatus(aegisub_core_video_open(nullptr, nullptr, &bad_session),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"null context video open");
	if (bad_session)
		throw std::runtime_error("null context video open did not clear output session");
	RequireStatus(aegisub_core_video_open(context, nullptr, &bad_session),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"null options video open");
	if (bad_session)
		throw std::runtime_error("null options video open did not clear output session");
	RequireStatus(aegisub_core_video_open(context, nullptr, nullptr),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"null output video open");
	RequireStatus(aegisub_core_video_info_get(nullptr, nullptr),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"null video info get");
	aegisub_core_video_frame_info frame_info{};
	RequireStatus(aegisub_core_video_frame_bgra_info_get(nullptr, 0, &frame_info),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"null video frame info get");
	unsigned char tiny_frame_buffer[4] = {};
	RequireStatus(aegisub_core_video_frame_bgra_get(nullptr, 0, tiny_frame_buffer, sizeof(tiny_frame_buffer), &frame_info),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"null video frame get");
	if (aegisub_core_video_keyframe_count(nullptr) != 0)
		throw std::runtime_error("null video session reported keyframes");
	int32_t keyframe = -1;
	RequireStatus(aegisub_core_video_keyframe_get_at(nullptr, 0, &keyframe),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"null video keyframe get");
	int32_t keyframes[4] = {-1, -1, -1, -1};
	size_t keyframes_written = 99;
	RequireStatus(aegisub_core_video_keyframes_get(nullptr, 0, 1, keyframes, &keyframes_written),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"null video keyframe window get");
	if (keyframes_written != 0)
		throw std::runtime_error("null video keyframe window did not clear written count");

	aegisub_core_video_open_options invalid_options{};
	invalid_options.abi_version = AEGISUB_CORE_ABI_VERSION + 1;
	RequireStatus(aegisub_core_video_open(context, &invalid_options, &bad_session),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"invalid video open options ABI");
	if (bad_session)
		throw std::runtime_error("invalid video ABI unexpectedly returned a session");

	aegisub_core_video_open_options bad_string_options{};
	bad_string_options.abi_version = AEGISUB_CORE_ABI_VERSION;
	bad_string_options.path = {nullptr, 1};
	RequireStatus(aegisub_core_video_open(context, &bad_string_options, &bad_session),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"invalid video path string view");
	if (bad_session)
		throw std::runtime_error("invalid video path string view returned a session");
	if (TakeString(aegisub_core_context_last_error(context)).empty())
		throw std::runtime_error("invalid video path string view did not set last_error");

	aegisub_core_video_open_options options{};
	options.abi_version = AEGISUB_CORE_ABI_VERSION;
	char const *path = "?dummy:24:2:16:8:10:20:30:";
	options.path = {path, std::strlen(path)};
	options.preferred_provider = {"Dummy", 5};
	options.max_cache_size_bytes = 0;

	aegisub_core_video_session *session = nullptr;
	RequireStatus(aegisub_core_video_open(context, &options, &session),
		AEGISUB_CORE_STATUS_OK,
		"dummy video open");
	if (!session)
		throw std::runtime_error("dummy video open returned null session");
	if (!TakeString(aegisub_core_context_last_error(context)).empty())
		throw std::runtime_error("dummy video open left stale last_error after success");

	aegisub_core_video_info info{};
	RequireStatus(aegisub_core_video_info_get(session, &info),
		AEGISUB_CORE_STATUS_OK,
		"dummy video info get");
	if (ToString(info.decoder_name) != "Dummy Video Provider"
		|| ToString(info.selected_provider) != "Dummy"
		|| info.frame_count != 2
		|| info.width != 16
		|| info.height != 8
		|| info.fps != 24.0
		|| info.should_set_video_properties != 0
		|| info.keyframe_count != aegisub_core_video_keyframe_count(session))
		throw std::runtime_error("dummy video info snapshot was not stable");
	RequireStatus(aegisub_core_video_info_get(session, nullptr),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"dummy video null info get");
	RequireStatus(aegisub_core_video_frame_bgra_info_get(session, 0, nullptr),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"dummy video null frame info output");
	RequireStatus(aegisub_core_video_frame_bgra_info_get(session, info.frame_count, &frame_info),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"dummy video out-of-range frame info get");
	RequireStatus(aegisub_core_video_frame_bgra_info_get(session, 0, &frame_info),
		AEGISUB_CORE_STATUS_OK,
		"dummy video frame info get");
	if (frame_info.width != 16
		|| frame_info.height != 8
		|| frame_info.pitch != 64
		|| frame_info.data_size != 512
		|| frame_info.flipped != 0)
		throw std::runtime_error("dummy video frame info was not stable");

	std::vector<unsigned char> frame_bytes(frame_info.data_size);
	aegisub_core_video_frame_info copied_frame_info{};
	RequireStatus(aegisub_core_video_frame_bgra_get(session, 0, nullptr, frame_bytes.size(), &copied_frame_info),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"dummy video null frame buffer");
	RequireStatus(aegisub_core_video_frame_bgra_get(session, 0, frame_bytes.data(), frame_bytes.size(), nullptr),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"dummy video null copied frame info output");
	RequireStatus(aegisub_core_video_frame_bgra_get(session, 0, frame_bytes.data(), frame_bytes.size() - 1, &copied_frame_info),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"dummy video too-small frame buffer");
	RequireStatus(aegisub_core_video_frame_bgra_get(session, info.frame_count, frame_bytes.data(), frame_bytes.size(), &copied_frame_info),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"dummy video out-of-range frame copy");
	RequireStatus(aegisub_core_video_frame_bgra_get(session, 0, frame_bytes.data(), frame_bytes.size(), &copied_frame_info),
		AEGISUB_CORE_STATUS_OK,
		"dummy video frame copy");
	if (copied_frame_info.width != frame_info.width
		|| copied_frame_info.height != frame_info.height
		|| copied_frame_info.pitch != frame_info.pitch
		|| copied_frame_info.data_size != frame_info.data_size
		|| copied_frame_info.flipped != frame_info.flipped
		|| frame_bytes[0] != 30
		|| frame_bytes[1] != 20
		|| frame_bytes[2] != 10
		|| frame_bytes[3] != 255)
		throw std::runtime_error("dummy video frame copy did not return expected BGRA bytes");
	RequireStatus(aegisub_core_video_keyframe_get_at(session, info.keyframe_count, &keyframe),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"dummy video out-of-range keyframe get");
	RequireStatus(aegisub_core_video_keyframe_get_at(session, 0, nullptr),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"dummy video null keyframe output");
	RequireStatus(aegisub_core_video_keyframes_get(session, 0, info.keyframe_count, keyframes, &keyframes_written),
		AEGISUB_CORE_STATUS_OK,
		"dummy video keyframe window get");
	if (keyframes_written != info.keyframe_count)
		throw std::runtime_error("dummy video keyframe window count was not stable");
	RequireStatus(aegisub_core_video_keyframes_get(session, info.keyframe_count, 1, keyframes, &keyframes_written),
		AEGISUB_CORE_STATUS_OK,
		"dummy video empty tail keyframe window get");
	if (keyframes_written != 0)
		throw std::runtime_error("dummy video empty tail keyframe window wrote rows");
	RequireStatus(aegisub_core_video_keyframes_get(session, info.keyframe_count + 1, 1, keyframes, &keyframes_written),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"dummy video out-of-range keyframe window get");
	if (keyframes_written != 0)
		throw std::runtime_error("dummy video invalid keyframe window did not clear written count");
	RequireStatus(aegisub_core_video_keyframes_get(session, 0, 1, nullptr, &keyframes_written),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"dummy video null keyframe window output");
	RequireStatus(aegisub_core_video_keyframes_get(session, 0, 0, nullptr, &keyframes_written),
		AEGISUB_CORE_STATUS_OK,
		"dummy video zero keyframe window with null output");
	if (keyframes_written != 0)
		throw std::runtime_error("dummy video zero keyframe window wrote rows");
	RequireStatus(aegisub_core_video_keyframes_get(session, 0, 1, keyframes, nullptr),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"dummy video null keyframe window written output");
	aegisub_core_video_destroy(session);
	RequireProviderOpenReport(context, AEGISUB_CORE_PROVIDER_VIDEO, "Dummy", "dummy video provider open report");
}

void RequireDummyAudioOpen(aegisub_core_context *context) {
	aegisub_core_audio_session *bad_session = reinterpret_cast<aegisub_core_audio_session *>(0x1);
	RequireStatus(aegisub_core_audio_open(nullptr, nullptr, &bad_session),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"null context audio open");
	if (bad_session)
		throw std::runtime_error("null context audio open did not clear output session");
	RequireStatus(aegisub_core_audio_open(context, nullptr, &bad_session),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"null options audio open");
	if (bad_session)
		throw std::runtime_error("null options audio open did not clear output session");
	RequireStatus(aegisub_core_audio_open(context, nullptr, nullptr),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"null output audio open");
	RequireStatus(aegisub_core_audio_info_get(nullptr, nullptr),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"null audio info get");

	aegisub_core_audio_open_options invalid_options{};
	invalid_options.abi_version = AEGISUB_CORE_ABI_VERSION + 1;
	RequireStatus(aegisub_core_audio_open(context, &invalid_options, &bad_session),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"invalid audio open options ABI");
	if (bad_session)
		throw std::runtime_error("invalid audio ABI unexpectedly returned a session");

	aegisub_core_audio_open_options bad_string_options{};
	bad_string_options.abi_version = AEGISUB_CORE_ABI_VERSION;
	bad_string_options.path = {nullptr, 1};
	RequireStatus(aegisub_core_audio_open(context, &bad_string_options, &bad_session),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"invalid audio path string view");
	if (bad_session)
		throw std::runtime_error("invalid audio path string view returned a session");
	if (TakeString(aegisub_core_context_last_error(context)).empty())
		throw std::runtime_error("invalid audio path string view did not set last_error");

	aegisub_core_audio_open_options options{};
	options.abi_version = AEGISUB_CORE_ABI_VERSION;
	char const *path = "dummy-audio:";
	options.path = {path, std::strlen(path)};
	options.preferred_provider = {"Dummy", 5};

	aegisub_core_audio_session *session = nullptr;
	RequireStatus(aegisub_core_audio_open(context, &options, &session),
		AEGISUB_CORE_STATUS_OK,
		"dummy audio open");
	if (!session)
		throw std::runtime_error("dummy audio open returned null session");
	if (!TakeString(aegisub_core_context_last_error(context)).empty())
		throw std::runtime_error("dummy audio open left stale last_error after success");

	aegisub_core_audio_info info{};
	RequireStatus(aegisub_core_audio_info_get(session, &info),
		AEGISUB_CORE_STATUS_OK,
		"dummy audio info get");
	if (ToString(info.selected_provider) != "Dummy"
		|| info.num_samples != 396900000
		|| info.decoded_samples != 396900000
		|| info.sample_rate != 44100
		|| info.bytes_per_sample != 2
		|| info.channels != 1
		|| info.float_samples != 0
		|| info.source_needs_cache != 0
		|| info.logical_bytes != 793800000
		|| info.decoded_bytes != 793800000)
		throw std::runtime_error("dummy audio info snapshot was not stable");
	RequireStatus(aegisub_core_audio_info_get(session, nullptr),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"dummy audio null info get");
	aegisub_core_audio_destroy(session);
	RequireProviderOpenReport(context, AEGISUB_CORE_PROVIDER_AUDIO, "Dummy", "dummy audio provider open report");
}

void RequireSubtitleOpen(aegisub_core_context *context) {
	aegisub_core_subtitle_session *bad_session = reinterpret_cast<aegisub_core_subtitle_session *>(0x1);
	RequireStatus(aegisub_core_subtitle_open(nullptr, nullptr, &bad_session),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"null context subtitle open");
	if (bad_session)
		throw std::runtime_error("null context subtitle open did not clear output session");
	RequireStatus(aegisub_core_subtitle_open(context, nullptr, &bad_session),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"null options subtitle open");
	if (bad_session)
		throw std::runtime_error("null options subtitle open did not clear output session");
	RequireStatus(aegisub_core_subtitle_open(context, nullptr, nullptr),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"null output subtitle open");
	RequireStatus(aegisub_core_subtitle_info_get(nullptr, nullptr),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"null subtitle info get");
	if (aegisub_core_subtitle_row_count(nullptr) != 0)
		throw std::runtime_error("null subtitle session reported rows");
	RequireStatus(aegisub_core_subtitle_row_get_at(nullptr, 0, nullptr),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"null subtitle row get");

	aegisub_core_subtitle_open_options invalid_options{};
	invalid_options.abi_version = AEGISUB_CORE_ABI_VERSION + 1;
	RequireStatus(aegisub_core_subtitle_open(context, &invalid_options, &bad_session),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"invalid subtitle open options ABI");
	if (bad_session)
		throw std::runtime_error("invalid subtitle ABI unexpectedly returned a session");

	aegisub_core_subtitle_open_options bad_string_options{};
	bad_string_options.abi_version = AEGISUB_CORE_ABI_VERSION;
	bad_string_options.path = {nullptr, 1};
	RequireStatus(aegisub_core_subtitle_open(context, &bad_string_options, &bad_session),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"invalid subtitle path string view");
	if (bad_session)
		throw std::runtime_error("invalid subtitle path string view returned a session");
	if (TakeString(aegisub_core_context_last_error(context)).empty())
		throw std::runtime_error("invalid subtitle path string view did not set last_error");

	ScopedFile ass_path(MakeTempAssPath());
	WriteSmokeAss(ass_path.get());
	auto path = ass_path.get().string();

	aegisub_core_subtitle_open_options options{};
	options.abi_version = AEGISUB_CORE_ABI_VERSION;
	options.path = {path.data(), path.size()};
	options.encoding = {"utf-8", 5};

	aegisub_core_subtitle_session *session = nullptr;
	RequireStatus(aegisub_core_subtitle_open(context, &options, &session),
		AEGISUB_CORE_STATUS_OK,
		"subtitle open");
	if (!session)
		throw std::runtime_error("subtitle open returned null session");
	if (!TakeString(aegisub_core_context_last_error(context)).empty())
		throw std::runtime_error("subtitle open left stale last_error after success");

	aegisub_core_subtitle_info info{};
	RequireStatus(aegisub_core_subtitle_info_get(session, &info),
		AEGISUB_CORE_STATUS_OK,
		"subtitle info get");
	if (ToString(info.format_name) != "Advanced SubStation Alpha"
		|| info.row_count != 2
		|| info.style_count != 1
		|| info.width != 1280
		|| info.height != 720
		|| info.row_count != static_cast<int32_t>(aegisub_core_subtitle_row_count(session)))
		throw std::runtime_error("subtitle info snapshot was not stable");
	aegisub_core_subtitle_state state{};
	RequireStatus(aegisub_core_subtitle_state_get(nullptr, &state),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"subtitle state get null session");
	RequireStatus(aegisub_core_subtitle_state_get(session, nullptr),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"subtitle state get null output");
	RequireStatus(aegisub_core_subtitle_state_get(session, &state),
		AEGISUB_CORE_STATUS_OK,
		"subtitle state get");
	if (state.revision != 0
		|| state.row_change_revision != 0
		|| state.row_change_first_row != 0
		|| state.row_change_row_count != 0
		|| state.row_change_fields != AEGISUB_CORE_SUBTITLE_CHANGE_NONE
		|| state.dirty != 0)
		throw std::runtime_error("new subtitle session state was not clean");

	aegisub_core_subtitle_row first{};
	aegisub_core_subtitle_row second{};
	RequireStatus(aegisub_core_subtitle_row_get_at(session, 0, &first),
		AEGISUB_CORE_STATUS_OK,
		"subtitle first row get");
	RequireStatus(aegisub_core_subtitle_row_get_at(session, 1, &second),
		AEGISUB_CORE_STATUS_OK,
		"subtitle second row get");
	if (first.row_index != 0
		|| first.comment != 0
		|| first.layer != 0
		|| first.start_ms != 1000
		|| first.end_ms != 2500
		|| first.margin_left != 10
		|| first.margin_right != 20
		|| first.margin_vertical != 30
		|| ToString(first.actor) != "Actor"
		|| ToString(first.text) != "Hello core"
		|| second.comment != 1
		|| second.layer != 1
		|| ToString(second.effect) != "fx"
		|| ToString(second.text) != "Hidden note")
		throw std::runtime_error("subtitle row snapshots were not stable");

	aegisub_core_subtitle_row window[3]{};
	size_t written = 999;
	RequireStatus(aegisub_core_subtitle_rows_get(session, 0, 2, window, &written),
		AEGISUB_CORE_STATUS_OK,
		"subtitle row window get");
	if (written != 2
		|| ToString(window[0].text) != "Hello core"
		|| ToString(window[1].text) != "Hidden note")
		throw std::runtime_error("subtitle row window was not stable");
	RequireStatus(aegisub_core_subtitle_rows_get(session, 1, 3, window, &written),
		AEGISUB_CORE_STATUS_OK,
		"subtitle clipped row window get");
	if (written != 1 || ToString(window[0].text) != "Hidden note")
		throw std::runtime_error("subtitle clipped row window was not stable");
	RequireStatus(aegisub_core_subtitle_rows_get(session, aegisub_core_subtitle_row_count(session), 1, window, &written),
		AEGISUB_CORE_STATUS_OK,
		"subtitle empty tail row window get");
	if (written != 0)
		throw std::runtime_error("subtitle empty tail row window wrote rows");
	RequireStatus(aegisub_core_subtitle_rows_get(session, aegisub_core_subtitle_row_count(session) + 1, 1, window, &written),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"subtitle out-of-range row window get");
	if (written != 0)
		throw std::runtime_error("subtitle invalid row window did not clear written count");
	RequireStatus(aegisub_core_subtitle_rows_get(session, 0, 1, nullptr, &written),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"subtitle null row window output");
	RequireStatus(aegisub_core_subtitle_rows_get(session, 0, 0, nullptr, &written),
		AEGISUB_CORE_STATUS_OK,
		"subtitle zero row window with null output");
	if (written != 0)
		throw std::runtime_error("subtitle zero row window wrote rows");
	RequireStatus(aegisub_core_subtitle_rows_get(session, 0, 1, window, nullptr),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"subtitle null row window written output");
	RequireStatus(aegisub_core_subtitle_info_get(session, nullptr),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"subtitle null info get");
	RequireStatus(aegisub_core_subtitle_row_get_at(session, aegisub_core_subtitle_row_count(session), &first),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"subtitle out-of-range row get");
	RequireStatus(aegisub_core_subtitle_row_get_at(session, 0, nullptr),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"subtitle null row output");

	RequireStatus(aegisub_core_subtitle_row_text_set(nullptr, 0, {"x", 1}),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"subtitle row text set null session");
	RequireStatus(aegisub_core_subtitle_row_text_set(session, aegisub_core_subtitle_row_count(session), {"x", 1}),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"subtitle row text set out of range");
	RequireStatus(aegisub_core_subtitle_row_text_set(session, 0, {nullptr, 1}),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"subtitle row text set invalid string view");
	char const *edited_text = "Edited over C ABI";
	RequireStatus(aegisub_core_subtitle_row_text_set(session, 0, {edited_text, std::strlen(edited_text)}),
		AEGISUB_CORE_STATUS_OK,
		"subtitle row text set");
	RequireStatus(aegisub_core_subtitle_row_get_at(session, 0, &first),
		AEGISUB_CORE_STATUS_OK,
		"subtitle edited first row get");
	if (ToString(first.text) != "Edited over C ABI"
		|| aegisub_core_subtitle_row_count(session) != 2)
		throw std::runtime_error("subtitle row text set did not update the row snapshot");
	RequireStatus(aegisub_core_subtitle_state_get(session, &state),
		AEGISUB_CORE_STATUS_OK,
		"subtitle state after row text set");
	if (state.revision != 1
		|| state.row_change_revision != 1
		|| state.row_change_first_row != 0
		|| state.row_change_row_count != 1
		|| state.row_change_fields != AEGISUB_CORE_SUBTITLE_CHANGE_TEXT
		|| state.dirty != 1)
		throw std::runtime_error("subtitle state did not track row text set");
	RequireStatus(aegisub_core_subtitle_row_text_set(session, 0, {edited_text, std::strlen(edited_text)}),
		AEGISUB_CORE_STATUS_OK,
		"subtitle same row text set");
	RequireStatus(aegisub_core_subtitle_state_get(session, &state),
		AEGISUB_CORE_STATUS_OK,
		"subtitle state after same row text set");
	if (state.revision != 1
		|| state.row_change_revision != 1
		|| state.row_change_first_row != 0
		|| state.row_change_row_count != 1
		|| state.row_change_fields != AEGISUB_CORE_SUBTITLE_CHANGE_TEXT
		|| state.dirty != 1)
		throw std::runtime_error("subtitle same row text set changed state");

	size_t applied = 999;
	aegisub_core_subtitle_row_text_edit batch_edits[2] = {
		{0, {"Batch edit zero", std::strlen("Batch edit zero")}},
		{1, {"Batch edit one", std::strlen("Batch edit one")}},
	};
	RequireStatus(aegisub_core_subtitle_row_texts_set(session, batch_edits, 2, nullptr),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"subtitle row texts set null applied output");
	RequireStatus(aegisub_core_subtitle_row_texts_set(session, nullptr, 1, &applied),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"subtitle row texts set null non-empty edit array");
	if (applied != 0)
		throw std::runtime_error("subtitle invalid batch edit did not clear applied count");

	aegisub_core_subtitle_row_text_edit invalid_string_edits[2] = {
		{0, {"Should not apply", std::strlen("Should not apply")}},
		{1, {nullptr, 1}},
	};
	RequireStatus(aegisub_core_subtitle_row_texts_set(session, invalid_string_edits, 2, &applied),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"subtitle row texts set invalid string view");
	if (applied != 0)
		throw std::runtime_error("subtitle invalid string batch edit did not clear applied count");
	RequireStatus(aegisub_core_subtitle_row_get_at(session, 0, &first),
		AEGISUB_CORE_STATUS_OK,
		"subtitle first row get after invalid string batch");
	RequireStatus(aegisub_core_subtitle_row_get_at(session, 1, &second),
		AEGISUB_CORE_STATUS_OK,
		"subtitle second row get after invalid string batch");
	if (ToString(first.text) != "Edited over C ABI"
		|| ToString(second.text) != "Hidden note")
		throw std::runtime_error("subtitle invalid string batch edit was not transactional");
	RequireStatus(aegisub_core_subtitle_state_get(session, &state),
		AEGISUB_CORE_STATUS_OK,
		"subtitle state after invalid string batch");
	if (state.revision != 1
		|| state.row_change_revision != 1
		|| state.row_change_first_row != 0
		|| state.row_change_row_count != 1
		|| state.row_change_fields != AEGISUB_CORE_SUBTITLE_CHANGE_TEXT
		|| state.dirty != 1)
		throw std::runtime_error("subtitle invalid string batch edit changed state");

	aegisub_core_subtitle_row_text_edit invalid_row_edits[2] = {
		{0, {"Should not apply", std::strlen("Should not apply")}},
		{aegisub_core_subtitle_row_count(session), {"Invalid row", std::strlen("Invalid row")}},
	};
	RequireStatus(aegisub_core_subtitle_row_texts_set(session, invalid_row_edits, 2, &applied),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"subtitle row texts set out-of-range row");
	if (applied != 0)
		throw std::runtime_error("subtitle invalid row batch edit did not clear applied count");
	RequireStatus(aegisub_core_subtitle_row_get_at(session, 0, &first),
		AEGISUB_CORE_STATUS_OK,
		"subtitle first row get after invalid row batch");
	RequireStatus(aegisub_core_subtitle_row_get_at(session, 1, &second),
		AEGISUB_CORE_STATUS_OK,
		"subtitle second row get after invalid row batch");
	if (ToString(first.text) != "Edited over C ABI"
		|| ToString(second.text) != "Hidden note")
		throw std::runtime_error("subtitle invalid row batch edit was not transactional");
	RequireStatus(aegisub_core_subtitle_state_get(session, &state),
		AEGISUB_CORE_STATUS_OK,
		"subtitle state after invalid row batch");
	if (state.revision != 1
		|| state.row_change_revision != 1
		|| state.row_change_first_row != 0
		|| state.row_change_row_count != 1
		|| state.row_change_fields != AEGISUB_CORE_SUBTITLE_CHANGE_TEXT
		|| state.dirty != 1)
		throw std::runtime_error("subtitle invalid row batch edit changed state");
	aegisub_core_subtitle_row_text_edit duplicate_row_edits[2] = {
		{0, {"Duplicate row first", std::strlen("Duplicate row first")}},
		{0, {"Duplicate row second", std::strlen("Duplicate row second")}},
	};
	RequireStatus(aegisub_core_subtitle_row_texts_set(session, duplicate_row_edits, 2, &applied),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"subtitle row texts set duplicate row");
	if (applied != 0)
		throw std::runtime_error("subtitle duplicate row batch edit did not clear applied count");
	RequireStatus(aegisub_core_subtitle_row_get_at(session, 0, &first),
		AEGISUB_CORE_STATUS_OK,
		"subtitle first row get after duplicate row batch");
	if (ToString(first.text) != "Edited over C ABI")
		throw std::runtime_error("subtitle duplicate row batch edit was not transactional");
	RequireStatus(aegisub_core_subtitle_state_get(session, &state),
		AEGISUB_CORE_STATUS_OK,
		"subtitle state after duplicate row batch");
	if (state.revision != 1
		|| state.row_change_revision != 1
		|| state.row_change_first_row != 0
		|| state.row_change_row_count != 1
		|| state.row_change_fields != AEGISUB_CORE_SUBTITLE_CHANGE_TEXT
		|| state.dirty != 1)
		throw std::runtime_error("subtitle duplicate row batch edit changed state");

	RequireStatus(aegisub_core_subtitle_row_texts_set(session, nullptr, 0, &applied),
		AEGISUB_CORE_STATUS_OK,
		"subtitle zero row texts set with null edits");
	if (applied != 0)
		throw std::runtime_error("subtitle zero row texts set applied edits");
	RequireStatus(aegisub_core_subtitle_state_get(session, &state),
		AEGISUB_CORE_STATUS_OK,
		"subtitle state after zero row texts set");
	if (state.revision != 1
		|| state.row_change_revision != 1
		|| state.row_change_first_row != 0
		|| state.row_change_row_count != 1
		|| state.row_change_fields != AEGISUB_CORE_SUBTITLE_CHANGE_TEXT
		|| state.dirty != 1)
		throw std::runtime_error("subtitle zero row texts set changed state");
	RequireStatus(aegisub_core_subtitle_row_texts_set(session, batch_edits, 2, &applied),
		AEGISUB_CORE_STATUS_OK,
		"subtitle row texts set");
	if (applied != 2)
		throw std::runtime_error("subtitle batch edit reported the wrong applied count");
	RequireStatus(aegisub_core_subtitle_row_get_at(session, 0, &first),
		AEGISUB_CORE_STATUS_OK,
		"subtitle first row get after batch");
	RequireStatus(aegisub_core_subtitle_row_get_at(session, 1, &second),
		AEGISUB_CORE_STATUS_OK,
		"subtitle second row get after batch");
	if (ToString(first.text) != "Batch edit zero"
		|| ToString(second.text) != "Batch edit one")
		throw std::runtime_error("subtitle batch row text set did not update both rows");
	RequireStatus(aegisub_core_subtitle_state_get(session, &state),
		AEGISUB_CORE_STATUS_OK,
		"subtitle state after row texts set");
	if (state.revision != 2
		|| state.row_change_revision != 2
		|| state.row_change_first_row != 0
		|| state.row_change_row_count != 2
		|| state.row_change_fields != AEGISUB_CORE_SUBTITLE_CHANGE_TEXT
		|| state.dirty != 1)
		throw std::runtime_error("subtitle state did not track row texts set");
	RequireStatus(aegisub_core_subtitle_row_texts_set(session, batch_edits, 2, &applied),
		AEGISUB_CORE_STATUS_OK,
		"subtitle same row texts set");
	if (applied != 0)
		throw std::runtime_error("subtitle same batch edit reported applied rows");
	RequireStatus(aegisub_core_subtitle_state_get(session, &state),
		AEGISUB_CORE_STATUS_OK,
		"subtitle state after same row texts set");
	if (state.revision != 2
		|| state.row_change_revision != 2
		|| state.row_change_first_row != 0
		|| state.row_change_row_count != 2
		|| state.row_change_fields != AEGISUB_CORE_SUBTITLE_CHANGE_TEXT
		|| state.dirty != 1)
		throw std::runtime_error("subtitle same row texts set changed state");

	aegisub_core_subtitle_row_patch patches[2]{};
	patches[0].row = 0;
	patches[0].fields = AEGISUB_CORE_SUBTITLE_ROW_PATCH_START
		| AEGISUB_CORE_SUBTITLE_ROW_PATCH_END
		| AEGISUB_CORE_SUBTITLE_ROW_PATCH_TEXT;
	patches[0].start_ms = 1200;
	patches[0].end_ms = 2800;
	patches[0].text = {"Patch edit zero", std::strlen("Patch edit zero")};
	patches[1].row = 1;
	patches[1].fields = AEGISUB_CORE_SUBTITLE_ROW_PATCH_COMMENT
		| AEGISUB_CORE_SUBTITLE_ROW_PATCH_LAYER
		| AEGISUB_CORE_SUBTITLE_ROW_PATCH_MARGINS
		| AEGISUB_CORE_SUBTITLE_ROW_PATCH_EFFECT
		| AEGISUB_CORE_SUBTITLE_ROW_PATCH_TEXT;
	patches[1].comment = 0;
	patches[1].layer = 3;
	patches[1].margin_left = 11;
	patches[1].margin_right = 22;
	patches[1].margin_vertical = 33;
	patches[1].effect = {"patched-effect", std::strlen("patched-effect")};
	patches[1].text = {"Patch edit one", std::strlen("Patch edit one")};
	RequireStatus(aegisub_core_subtitle_row_patches_apply(session, patches, 2, nullptr),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"subtitle row patches null applied output");
	RequireStatus(aegisub_core_subtitle_row_patches_apply(session, nullptr, 1, &applied),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"subtitle row patches null non-empty patch array");
	if (applied != 0)
		throw std::runtime_error("subtitle null patch array did not clear applied count");
	aegisub_core_subtitle_row_patch invalid_patch = patches[0];
	invalid_patch.fields = 1u << 30;
	RequireStatus(aegisub_core_subtitle_row_patches_apply(session, &invalid_patch, 1, &applied),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"subtitle row patch unknown field");
	if (applied != 0)
		throw std::runtime_error("subtitle unknown field patch did not clear applied count");
	invalid_patch = patches[0];
	invalid_patch.start_ms = 5000;
	RequireStatus(aegisub_core_subtitle_row_patches_apply(session, &invalid_patch, 1, &applied),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"subtitle row patch invalid time range");
	if (applied != 0)
		throw std::runtime_error("subtitle invalid time patch did not clear applied count");
	invalid_patch = patches[0];
	invalid_patch.text = {nullptr, 1};
	RequireStatus(aegisub_core_subtitle_row_patches_apply(session, &invalid_patch, 1, &applied),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"subtitle row patch invalid string");
	if (applied != 0)
		throw std::runtime_error("subtitle invalid string patch did not clear applied count");
	RequireStatus(aegisub_core_subtitle_row_get_at(session, 0, &first),
		AEGISUB_CORE_STATUS_OK,
		"subtitle first row get after invalid patches");
	if (ToString(first.text) != "Batch edit zero" || first.start_ms != 1000 || first.end_ms != 2500)
		throw std::runtime_error("subtitle invalid row patch was not transactional");
	RequireStatus(aegisub_core_subtitle_state_get(session, &state),
		AEGISUB_CORE_STATUS_OK,
		"subtitle state after invalid row patches");
	if (state.revision != 2
		|| state.row_change_revision != 2
		|| state.row_change_first_row != 0
		|| state.row_change_row_count != 2
		|| state.row_change_fields != AEGISUB_CORE_SUBTITLE_CHANGE_TEXT
		|| state.dirty != 1)
		throw std::runtime_error("subtitle invalid row patch changed state");
	RequireStatus(aegisub_core_subtitle_row_patches_apply(session, nullptr, 0, &applied),
		AEGISUB_CORE_STATUS_OK,
		"subtitle zero row patches with null patches");
	if (applied != 0)
		throw std::runtime_error("subtitle zero row patches applied patches");
	RequireStatus(aegisub_core_subtitle_state_get(session, &state),
		AEGISUB_CORE_STATUS_OK,
		"subtitle state after zero row patches");
	if (state.revision != 2
		|| state.row_change_revision != 2
		|| state.row_change_first_row != 0
		|| state.row_change_row_count != 2
		|| state.row_change_fields != AEGISUB_CORE_SUBTITLE_CHANGE_TEXT
		|| state.dirty != 1)
		throw std::runtime_error("subtitle zero row patches changed state");
	aegisub_core_subtitle_row_patch zero_field_patch = patches[0];
	zero_field_patch.fields = 0;
	zero_field_patch.start_ms = 5000;
	zero_field_patch.text = {nullptr, 1};
	RequireStatus(aegisub_core_subtitle_row_patches_apply(session, &zero_field_patch, 1, &applied),
		AEGISUB_CORE_STATUS_OK,
		"subtitle zero-field row patch");
	if (applied != 0)
		throw std::runtime_error("subtitle zero-field row patch reported applied patches");
	RequireStatus(aegisub_core_subtitle_row_get_at(session, 0, &first),
		AEGISUB_CORE_STATUS_OK,
		"subtitle first row get after zero-field patch");
	if (ToString(first.text) != "Batch edit zero" || first.start_ms != 1000 || first.end_ms != 2500)
		throw std::runtime_error("subtitle zero-field row patch changed row data");
	RequireStatus(aegisub_core_subtitle_state_get(session, &state),
		AEGISUB_CORE_STATUS_OK,
		"subtitle state after zero-field row patch");
	if (state.revision != 2
		|| state.row_change_revision != 2
		|| state.row_change_first_row != 0
		|| state.row_change_row_count != 2
		|| state.row_change_fields != AEGISUB_CORE_SUBTITLE_CHANGE_TEXT
		|| state.dirty != 1)
		throw std::runtime_error("subtitle zero-field row patch changed state");
	zero_field_patch.row = aegisub_core_subtitle_row_count(session);
	RequireStatus(aegisub_core_subtitle_row_patches_apply(session, &zero_field_patch, 1, &applied),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"subtitle zero-field row patch out-of-range row");
	if (applied != 0)
		throw std::runtime_error("subtitle zero-field invalid row patch did not clear applied count");
	aegisub_core_subtitle_row_patch duplicate_patches[2] = {patches[0], patches[0]};
	duplicate_patches[1].text = {"Duplicate patch", std::strlen("Duplicate patch")};
	RequireStatus(aegisub_core_subtitle_row_patches_apply(session, duplicate_patches, 2, &applied),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"subtitle row patches duplicate row");
	if (applied != 0)
		throw std::runtime_error("subtitle duplicate row patches did not clear applied count");
	RequireStatus(aegisub_core_subtitle_row_get_at(session, 0, &first),
		AEGISUB_CORE_STATUS_OK,
		"subtitle first row get after duplicate row patches");
	if (ToString(first.text) != "Batch edit zero" || first.start_ms != 1000 || first.end_ms != 2500)
		throw std::runtime_error("subtitle duplicate row patch was not transactional");
	RequireStatus(aegisub_core_subtitle_state_get(session, &state),
		AEGISUB_CORE_STATUS_OK,
		"subtitle state after duplicate row patches");
	if (state.revision != 2
		|| state.row_change_revision != 2
		|| state.row_change_first_row != 0
		|| state.row_change_row_count != 2
		|| state.row_change_fields != AEGISUB_CORE_SUBTITLE_CHANGE_TEXT
		|| state.dirty != 1)
		throw std::runtime_error("subtitle duplicate row patch changed state");
	RequireStatus(aegisub_core_subtitle_row_patches_apply(session, patches, 2, &applied),
		AEGISUB_CORE_STATUS_OK,
		"subtitle row patches apply");
	if (applied != 2)
		throw std::runtime_error("subtitle row patches reported the wrong applied count");
	RequireStatus(aegisub_core_subtitle_row_get_at(session, 0, &first),
		AEGISUB_CORE_STATUS_OK,
		"subtitle first row get after patch");
	RequireStatus(aegisub_core_subtitle_row_get_at(session, 1, &second),
		AEGISUB_CORE_STATUS_OK,
		"subtitle second row get after patch");
	if (first.start_ms != 1200
		|| first.end_ms != 2800
		|| ToString(first.text) != "Patch edit zero"
		|| second.comment != 0
		|| second.layer != 3
		|| second.margin_left != 11
		|| second.margin_right != 22
		|| second.margin_vertical != 33
		|| ToString(second.effect) != "patched-effect"
		|| ToString(second.text) != "Patch edit one")
		throw std::runtime_error("subtitle row patch did not update requested fields");
	RequireStatus(aegisub_core_subtitle_state_get(session, &state),
		AEGISUB_CORE_STATUS_OK,
		"subtitle state after row patches");
	if (state.revision != 3
		|| state.row_change_revision != 3
		|| state.row_change_first_row != 0
		|| state.row_change_row_count != 2
		|| state.row_change_fields != (AEGISUB_CORE_SUBTITLE_CHANGE_TEXT
			| AEGISUB_CORE_SUBTITLE_CHANGE_TIME
			| AEGISUB_CORE_SUBTITLE_CHANGE_METADATA)
		|| state.dirty != 1)
		throw std::runtime_error("subtitle state did not track row patches");
	RequireStatus(aegisub_core_subtitle_row_patches_apply(session, patches, 2, &applied),
		AEGISUB_CORE_STATUS_OK,
		"subtitle same row patches apply");
	if (applied != 0)
		throw std::runtime_error("subtitle same row patches reported applied patches");
	RequireStatus(aegisub_core_subtitle_state_get(session, &state),
		AEGISUB_CORE_STATUS_OK,
		"subtitle state after same row patches");
	if (state.revision != 3
		|| state.row_change_revision != 3
		|| state.row_change_first_row != 0
		|| state.row_change_row_count != 2
		|| state.row_change_fields != (AEGISUB_CORE_SUBTITLE_CHANGE_TEXT
			| AEGISUB_CORE_SUBTITLE_CHANGE_TIME
			| AEGISUB_CORE_SUBTITLE_CHANGE_METADATA)
		|| state.dirty != 1)
		throw std::runtime_error("subtitle same row patches changed state");

	size_t inserted_row = 999;
	RequireStatus(aegisub_core_subtitle_row_insert(nullptr, 1, &inserted_row),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"subtitle row insert null session");
	RequireStatus(aegisub_core_subtitle_row_insert(session, 1, nullptr),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"subtitle row insert null output");
	RequireStatus(aegisub_core_subtitle_row_insert(session, aegisub_core_subtitle_row_count(session) + 1, &inserted_row),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"subtitle row insert out-of-range position");
	if (inserted_row != 0)
		throw std::runtime_error("subtitle invalid row insert did not clear inserted row");
	RequireStatus(aegisub_core_subtitle_state_get(session, &state),
		AEGISUB_CORE_STATUS_OK,
		"subtitle state after invalid row insert");
	if (state.revision != 3
		|| state.row_change_revision != 3
		|| state.row_change_first_row != 0
		|| state.row_change_row_count != 2
		|| state.row_change_fields != (AEGISUB_CORE_SUBTITLE_CHANGE_TEXT
			| AEGISUB_CORE_SUBTITLE_CHANGE_TIME
			| AEGISUB_CORE_SUBTITLE_CHANGE_METADATA)
		|| state.dirty != 1)
		throw std::runtime_error("subtitle invalid row insert changed state");
	RequireStatus(aegisub_core_subtitle_row_insert(session, 1, &inserted_row),
		AEGISUB_CORE_STATUS_OK,
		"subtitle row insert");
	if (inserted_row != 1
		|| aegisub_core_subtitle_row_count(session) != 3)
		throw std::runtime_error("subtitle row insert reported the wrong row");
	RequireStatus(aegisub_core_subtitle_info_get(session, &info),
		AEGISUB_CORE_STATUS_OK,
		"subtitle info after row insert");
	RequireStatus(aegisub_core_subtitle_rows_get(session, 0, 3, window, &written),
		AEGISUB_CORE_STATUS_OK,
		"subtitle rows after row insert");
	if (info.row_count != 3
		|| written != 3
		|| window[0].row_index != 0
		|| window[1].row_index != 1
		|| window[2].row_index != 2
		|| ToString(window[1].text) != "")
		throw std::runtime_error("subtitle row insert did not update row snapshots");
	RequireStatus(aegisub_core_subtitle_state_get(session, &state),
		AEGISUB_CORE_STATUS_OK,
		"subtitle state after row insert");
	if (state.revision != 4
		|| state.row_change_revision != 4
		|| state.row_change_first_row != 1
		|| state.row_change_row_count != 2
		|| state.row_change_fields != AEGISUB_CORE_SUBTITLE_CHANGE_STRUCTURE
		|| state.dirty != 1)
		throw std::runtime_error("subtitle state did not track row insert");

	size_t deleted = 999;
	RequireStatus(aegisub_core_subtitle_rows_delete(nullptr, 1, 1, &deleted),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"subtitle rows delete null session");
	RequireStatus(aegisub_core_subtitle_rows_delete(session, 1, 1, nullptr),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"subtitle rows delete null output");
	RequireStatus(aegisub_core_subtitle_rows_delete(session, 3, 1, &deleted),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"subtitle rows delete out-of-range range");
	if (deleted != 0)
		throw std::runtime_error("subtitle invalid rows delete did not clear deleted count");
	RequireStatus(aegisub_core_subtitle_rows_delete(session, 2, 0, &deleted),
		AEGISUB_CORE_STATUS_OK,
		"subtitle zero rows delete");
	if (deleted != 0)
		throw std::runtime_error("subtitle zero rows delete deleted rows");
	RequireStatus(aegisub_core_subtitle_rows_delete(session, aegisub_core_subtitle_row_count(session), 0, &deleted),
		AEGISUB_CORE_STATUS_OK,
		"subtitle tail zero rows delete");
	if (deleted != 0)
		throw std::runtime_error("subtitle tail zero rows delete deleted rows");
	RequireStatus(aegisub_core_subtitle_rows_delete(session, aegisub_core_subtitle_row_count(session) + 1, 0, &deleted),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"subtitle out-of-range zero rows delete");
	if (deleted != 0)
		throw std::runtime_error("subtitle out-of-range zero rows delete did not clear deleted count");
	RequireStatus(aegisub_core_subtitle_state_get(session, &state),
		AEGISUB_CORE_STATUS_OK,
		"subtitle state after invalid/zero row delete");
	if (state.revision != 4
		|| state.row_change_revision != 4
		|| state.row_change_first_row != 1
		|| state.row_change_row_count != 2
		|| state.row_change_fields != AEGISUB_CORE_SUBTITLE_CHANGE_STRUCTURE
		|| state.dirty != 1)
		throw std::runtime_error("subtitle invalid/zero row delete changed state");
	RequireStatus(aegisub_core_subtitle_rows_delete(session, 1, 1, &deleted),
		AEGISUB_CORE_STATUS_OK,
		"subtitle rows delete");
	if (deleted != 1
		|| aegisub_core_subtitle_row_count(session) != 2)
		throw std::runtime_error("subtitle rows delete reported the wrong row count");
	RequireStatus(aegisub_core_subtitle_info_get(session, &info),
		AEGISUB_CORE_STATUS_OK,
		"subtitle info after row delete");
	RequireStatus(aegisub_core_subtitle_rows_get(session, 0, 2, window, &written),
		AEGISUB_CORE_STATUS_OK,
		"subtitle rows after row delete");
	if (info.row_count != 2
		|| written != 2
		|| window[0].row_index != 0
		|| window[1].row_index != 1
		|| ToString(window[0].text) != "Patch edit zero"
		|| ToString(window[1].text) != "Patch edit one")
		throw std::runtime_error("subtitle rows delete did not update row snapshots");
	RequireStatus(aegisub_core_subtitle_state_get(session, &state),
		AEGISUB_CORE_STATUS_OK,
		"subtitle state after rows delete");
	if (state.revision != 5
		|| state.row_change_revision != 5
		|| state.row_change_first_row != 1
		|| state.row_change_row_count != 1
		|| state.row_change_fields != AEGISUB_CORE_SUBTITLE_CHANGE_STRUCTURE
		|| state.dirty != 1)
		throw std::runtime_error("subtitle state did not track rows delete");

	int32_t selected_rows[] = {1};
	int32_t moved_selected_rows[] = {-1};
	size_t moved_selected_row_count = 99;
	RequireStatus(aegisub_core_subtitle_selected_rows_move(nullptr, selected_rows, 1, -1, moved_selected_rows, &moved_selected_row_count),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"subtitle selected rows move null session");
	if (moved_selected_row_count != 0)
		throw std::runtime_error("subtitle null selected rows move did not clear moved count");
	RequireStatus(aegisub_core_subtitle_selected_rows_move(session, selected_rows, 1, -1, moved_selected_rows, nullptr),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"subtitle selected rows move null count output");
	RequireStatus(aegisub_core_subtitle_selected_rows_move(session, nullptr, 1, -1, moved_selected_rows, &moved_selected_row_count),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"subtitle selected rows move null selected rows");
	RequireStatus(aegisub_core_subtitle_selected_rows_move(session, selected_rows, 1, -1, nullptr, &moved_selected_row_count),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"subtitle selected rows move null moved rows output");
	int32_t duplicate_selected_rows[] = {1, 1};
	int32_t duplicate_moved_rows[] = {-1, -1};
	RequireStatus(aegisub_core_subtitle_selected_rows_move(session, duplicate_selected_rows, 2, -1, duplicate_moved_rows, &moved_selected_row_count),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"subtitle selected rows move duplicate selected rows");
	int32_t invalid_selected_rows[] = {2};
	RequireStatus(aegisub_core_subtitle_selected_rows_move(session, invalid_selected_rows, 1, -1, moved_selected_rows, &moved_selected_row_count),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"subtitle selected rows move out-of-range selected row");
	RequireStatus(aegisub_core_subtitle_selected_rows_move(session, selected_rows, 1, 0, moved_selected_rows, &moved_selected_row_count),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"subtitle selected rows move invalid direction");
	RequireStatus(aegisub_core_subtitle_state_get(session, &state),
		AEGISUB_CORE_STATUS_OK,
		"subtitle state after invalid selected rows move");
	if (state.revision != 5
		|| state.row_change_revision != 5
		|| state.row_change_first_row != 1
		|| state.row_change_row_count != 1
		|| state.row_change_fields != AEGISUB_CORE_SUBTITLE_CHANGE_STRUCTURE
		|| state.dirty != 1)
		throw std::runtime_error("subtitle invalid selected rows move changed state");
	RequireStatus(aegisub_core_subtitle_selected_rows_move(session, selected_rows, 1, -1, moved_selected_rows, &moved_selected_row_count),
		AEGISUB_CORE_STATUS_OK,
		"subtitle selected rows move up");
	if (moved_selected_row_count != 1 || moved_selected_rows[0] != 0)
		throw std::runtime_error("subtitle selected rows move did not return moved selection");
	RequireStatus(aegisub_core_subtitle_rows_get(session, 0, 2, window, &written),
		AEGISUB_CORE_STATUS_OK,
		"subtitle rows after selected rows move");
	if (written != 2
		|| window[0].row_index != 0
		|| window[1].row_index != 1
		|| ToString(window[0].text) != "Patch edit one"
		|| ToString(window[1].text) != "Patch edit zero")
		throw std::runtime_error("subtitle selected rows move did not reorder row snapshots");
	RequireStatus(aegisub_core_subtitle_state_get(session, &state),
		AEGISUB_CORE_STATUS_OK,
		"subtitle state after selected rows move");
	if (state.revision != 6
		|| state.row_change_revision != 6
		|| state.row_change_first_row != 0
		|| state.row_change_row_count != 2
		|| state.row_change_fields != AEGISUB_CORE_SUBTITLE_CHANGE_STRUCTURE
		|| state.dirty != 1)
		throw std::runtime_error("subtitle state did not track selected rows move");
	int32_t edge_selected_rows[] = {0};
	RequireStatus(aegisub_core_subtitle_selected_rows_move(session, edge_selected_rows, 1, -1, moved_selected_rows, &moved_selected_row_count),
		AEGISUB_CORE_STATUS_OK,
		"subtitle selected rows move edge no-op");
	if (moved_selected_row_count != 0)
		throw std::runtime_error("subtitle selected rows move edge no-op returned moved rows");
	RequireStatus(aegisub_core_subtitle_selected_rows_move(session, nullptr, 0, 1, nullptr, &moved_selected_row_count),
		AEGISUB_CORE_STATUS_OK,
		"subtitle selected rows move empty no-op");
	if (moved_selected_row_count != 0)
		throw std::runtime_error("subtitle selected rows move empty no-op returned moved rows");
	RequireStatus(aegisub_core_subtitle_state_get(session, &state),
		AEGISUB_CORE_STATUS_OK,
		"subtitle state after selected rows move no-ops");
	if (state.revision != 6
		|| state.row_change_revision != 6
		|| state.row_change_first_row != 0
		|| state.row_change_row_count != 2
		|| state.row_change_fields != AEGISUB_CORE_SUBTITLE_CHANGE_STRUCTURE
		|| state.dirty != 1)
		throw std::runtime_error("subtitle selected rows move no-op changed state");

	int32_t duplicated_rows[] = {-1};
	size_t duplicated_row_count = 99;
	RequireStatus(aegisub_core_subtitle_selected_rows_duplicate(nullptr, edge_selected_rows, 1, duplicated_rows, &duplicated_row_count),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"subtitle selected rows duplicate null session");
	if (duplicated_row_count != 0)
		throw std::runtime_error("subtitle null selected rows duplicate did not clear duplicated count");
	RequireStatus(aegisub_core_subtitle_selected_rows_duplicate(session, edge_selected_rows, 1, duplicated_rows, nullptr),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"subtitle selected rows duplicate null count output");
	RequireStatus(aegisub_core_subtitle_selected_rows_duplicate(session, nullptr, 1, duplicated_rows, &duplicated_row_count),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"subtitle selected rows duplicate null selected rows");
	RequireStatus(aegisub_core_subtitle_selected_rows_duplicate(session, edge_selected_rows, 1, nullptr, &duplicated_row_count),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"subtitle selected rows duplicate null duplicated rows output");
	RequireStatus(aegisub_core_subtitle_selected_rows_duplicate(session, duplicate_selected_rows, 2, duplicate_moved_rows, &duplicated_row_count),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"subtitle selected rows duplicate duplicate selected rows");
	RequireStatus(aegisub_core_subtitle_selected_rows_duplicate(session, invalid_selected_rows, 1, duplicated_rows, &duplicated_row_count),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"subtitle selected rows duplicate out-of-range selected row");
	RequireStatus(aegisub_core_subtitle_state_get(session, &state),
		AEGISUB_CORE_STATUS_OK,
		"subtitle state after invalid selected rows duplicate");
	if (state.revision != 6
		|| state.row_change_revision != 6
		|| state.row_change_first_row != 0
		|| state.row_change_row_count != 2
		|| state.row_change_fields != AEGISUB_CORE_SUBTITLE_CHANGE_STRUCTURE
		|| state.dirty != 1)
		throw std::runtime_error("subtitle invalid selected rows duplicate changed state");
	RequireStatus(aegisub_core_subtitle_selected_rows_duplicate(session, edge_selected_rows, 1, duplicated_rows, &duplicated_row_count),
		AEGISUB_CORE_STATUS_OK,
		"subtitle selected rows duplicate");
	if (duplicated_row_count != 1
		|| duplicated_rows[0] != 1
		|| aegisub_core_subtitle_row_count(session) != 3)
		throw std::runtime_error("subtitle selected rows duplicate did not return duplicated rows");
	RequireStatus(aegisub_core_subtitle_rows_get(session, 0, 3, window, &written),
		AEGISUB_CORE_STATUS_OK,
		"subtitle rows after selected rows duplicate");
	if (written != 3
		|| window[0].row_index != 0
		|| window[1].row_index != 1
		|| window[2].row_index != 2
		|| ToString(window[0].text) != "Patch edit one"
		|| ToString(window[1].text) != "Patch edit one"
		|| ToString(window[2].text) != "Patch edit zero")
		throw std::runtime_error("subtitle selected rows duplicate did not update row snapshots");
	RequireStatus(aegisub_core_subtitle_selected_rows_duplicate(session, nullptr, 0, nullptr, &duplicated_row_count),
		AEGISUB_CORE_STATUS_OK,
		"subtitle selected rows duplicate empty no-op");
	if (duplicated_row_count != 0)
		throw std::runtime_error("subtitle selected rows duplicate empty no-op returned rows");
	RequireStatus(aegisub_core_subtitle_state_get(session, &state),
		AEGISUB_CORE_STATUS_OK,
		"subtitle state after selected rows duplicate");
	if (state.revision != 7
		|| state.row_change_revision != 7
		|| state.row_change_first_row != 1
		|| state.row_change_row_count != 2
		|| state.row_change_fields != AEGISUB_CORE_SUBTITLE_CHANGE_STRUCTURE
		|| state.dirty != 1)
		throw std::runtime_error("subtitle state did not track selected rows duplicate");

	int32_t sort_selection[] = {0, 1, 2};
	int32_t sorted_selection[] = {-1, -1, -1};
	size_t sorted_selection_count = 99;
	RequireStatus(aegisub_core_subtitle_rows_sort(nullptr, AEGISUB_CORE_SUBTITLE_SORT_START, 1, sort_selection, 3, sorted_selection, &sorted_selection_count),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"subtitle rows sort null session");
	if (sorted_selection_count != 0)
		throw std::runtime_error("subtitle null rows sort did not clear sorted count");
	RequireStatus(aegisub_core_subtitle_rows_sort(session, AEGISUB_CORE_SUBTITLE_SORT_START, 1, sort_selection, 3, sorted_selection, nullptr),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"subtitle rows sort null count output");
	RequireStatus(aegisub_core_subtitle_rows_sort(session, static_cast<aegisub_core_subtitle_sort_key>(99), 1, sort_selection, 3, sorted_selection, &sorted_selection_count),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"subtitle rows sort invalid key");
	RequireStatus(aegisub_core_subtitle_rows_sort(session, AEGISUB_CORE_SUBTITLE_SORT_START, 1, invalid_selected_rows, 1, nullptr, &sorted_selection_count),
		AEGISUB_CORE_STATUS_OK,
		"subtitle rows sort single selection is no-op");
	if (sorted_selection_count != 0)
		throw std::runtime_error("subtitle rows sort single no-op returned rows");
	int32_t invalid_sort_selection[] = {3, 0};
	RequireStatus(aegisub_core_subtitle_rows_sort(session, AEGISUB_CORE_SUBTITLE_SORT_START, 1, invalid_sort_selection, 2, sorted_selection, &sorted_selection_count),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"subtitle rows sort out-of-range selected row");
	RequireStatus(aegisub_core_subtitle_rows_sort(session, AEGISUB_CORE_SUBTITLE_SORT_START, 1, duplicate_selected_rows, 2, duplicate_moved_rows, &sorted_selection_count),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"subtitle rows sort duplicate selected rows");
	RequireStatus(aegisub_core_subtitle_rows_sort(session, AEGISUB_CORE_SUBTITLE_SORT_START, 1, sort_selection, 3, nullptr, &sorted_selection_count),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"subtitle rows sort null selected output");
	RequireStatus(aegisub_core_subtitle_state_get(session, &state),
		AEGISUB_CORE_STATUS_OK,
		"subtitle state after invalid rows sort");
	if (state.revision != 7
		|| state.row_change_revision != 7
		|| state.row_change_first_row != 1
		|| state.row_change_row_count != 2
		|| state.row_change_fields != AEGISUB_CORE_SUBTITLE_CHANGE_STRUCTURE
		|| state.dirty != 1)
		throw std::runtime_error("subtitle invalid rows sort changed state");
	RequireStatus(aegisub_core_subtitle_rows_sort(session, AEGISUB_CORE_SUBTITLE_SORT_START, 1, sort_selection, 3, sorted_selection, &sorted_selection_count),
		AEGISUB_CORE_STATUS_OK,
		"subtitle selected rows sort by start");
	if (sorted_selection_count != 3
		|| sorted_selection[0] != 0
		|| sorted_selection[1] != 1
		|| sorted_selection[2] != 2)
		throw std::runtime_error("subtitle selected rows sort did not return sorted selection");
	RequireStatus(aegisub_core_subtitle_rows_get(session, 0, 3, window, &written),
		AEGISUB_CORE_STATUS_OK,
		"subtitle rows after selected rows sort");
	if (written != 3
		|| ToString(window[0].text) != "Patch edit zero"
		|| ToString(window[1].text) != "Patch edit one"
		|| ToString(window[2].text) != "Patch edit one")
		throw std::runtime_error("subtitle selected rows sort did not update row snapshots");
	RequireStatus(aegisub_core_subtitle_state_get(session, &state),
		AEGISUB_CORE_STATUS_OK,
		"subtitle state after selected rows sort");
	if (state.revision != 8
		|| state.row_change_revision != 8
		|| state.row_change_first_row != 0
		|| state.row_change_row_count != 3
		|| state.row_change_fields != AEGISUB_CORE_SUBTITLE_CHANGE_STRUCTURE
		|| state.dirty != 1)
		throw std::runtime_error("subtitle state did not track selected rows sort");
	RequireStatus(aegisub_core_subtitle_rows_sort(session, AEGISUB_CORE_SUBTITLE_SORT_ACTOR, 0, nullptr, 0, nullptr, &sorted_selection_count),
		AEGISUB_CORE_STATUS_OK,
		"subtitle all rows sort by actor");
	if (sorted_selection_count != 0)
		throw std::runtime_error("subtitle all rows sort returned selected rows");
	RequireStatus(aegisub_core_subtitle_rows_get(session, 0, 3, window, &written),
		AEGISUB_CORE_STATUS_OK,
		"subtitle rows after all rows sort");
	if (written != 3
		|| ToString(window[0].text) != "Patch edit one"
		|| ToString(window[1].text) != "Patch edit one"
		|| ToString(window[2].text) != "Patch edit zero")
		throw std::runtime_error("subtitle all rows sort did not update row snapshots");
	RequireStatus(aegisub_core_subtitle_state_get(session, &state),
		AEGISUB_CORE_STATUS_OK,
		"subtitle state after all rows sort");
	if (state.revision != 9
		|| state.row_change_revision != 9
		|| state.row_change_first_row != 0
		|| state.row_change_row_count != 3
		|| state.row_change_fields != AEGISUB_CORE_SUBTITLE_CHANGE_STRUCTURE
		|| state.dirty != 1)
		throw std::runtime_error("subtitle state did not track all rows sort");

	int32_t paste_targets[] = {0};
	aegisub_core_subtitle_paste_over_source paste_sources[1]{};
	paste_sources[0].start_ms = 1500;
	paste_sources[0].end_ms = 2600;
	paste_sources[0].margin_left = 44;
	paste_sources[0].actor = {"PasteActor", 10};
	paste_sources[0].text = {"Paste over C API", 16};
	auto const paste_fields =
		static_cast<aegisub_core_subtitle_paste_over_fields>(
			AEGISUB_CORE_SUBTITLE_PASTE_OVER_START
			| AEGISUB_CORE_SUBTITLE_PASTE_OVER_END
			| AEGISUB_CORE_SUBTITLE_PASTE_OVER_ACTOR
			| AEGISUB_CORE_SUBTITLE_PASTE_OVER_MARGIN_LEFT
			| AEGISUB_CORE_SUBTITLE_PASTE_OVER_TEXT);
	size_t paste_applied = 99;
	RequireStatus(aegisub_core_subtitle_paste_over_apply(nullptr, paste_targets, paste_sources, 1, paste_fields, &paste_applied),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"subtitle paste-over null session");
	if (paste_applied != 0)
		throw std::runtime_error("subtitle paste-over null session did not clear applied count");
	RequireStatus(aegisub_core_subtitle_paste_over_apply(session, paste_targets, paste_sources, 1, paste_fields, nullptr),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"subtitle paste-over null applied output");
	RequireStatus(aegisub_core_subtitle_paste_over_apply(session, nullptr, paste_sources, 1, paste_fields, &paste_applied),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"subtitle paste-over null targets");
	RequireStatus(aegisub_core_subtitle_paste_over_apply(session, paste_targets, nullptr, 1, paste_fields, &paste_applied),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"subtitle paste-over null sources");
	aegisub_core_subtitle_paste_over_source invalid_paste_sources[1] = {paste_sources[0]};
	invalid_paste_sources[0].start_ms = 5000;
	invalid_paste_sources[0].end_ms = 1000;
	RequireStatus(aegisub_core_subtitle_paste_over_apply(session, paste_targets, invalid_paste_sources, 1, paste_fields, &paste_applied),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"subtitle paste-over invalid time range");
	int32_t duplicate_paste_targets[] = {0, 0};
	aegisub_core_subtitle_paste_over_source duplicate_paste_sources[2] = {paste_sources[0], paste_sources[0]};
	RequireStatus(aegisub_core_subtitle_paste_over_apply(session, duplicate_paste_targets, duplicate_paste_sources, 2, paste_fields, &paste_applied),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"subtitle paste-over duplicate targets");
	RequireStatus(aegisub_core_subtitle_paste_over_apply(session, paste_targets, paste_sources, 1, static_cast<aegisub_core_subtitle_paste_over_fields>(1u << 30), &paste_applied),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"subtitle paste-over unknown fields");
	RequireStatus(aegisub_core_subtitle_paste_over_apply(session, nullptr, nullptr, 0, static_cast<aegisub_core_subtitle_paste_over_fields>(1u << 30), &paste_applied),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"subtitle paste-over empty unknown fields");
	RequireStatus(aegisub_core_subtitle_paste_over_apply(session, nullptr, nullptr, 0, paste_fields, &paste_applied),
		AEGISUB_CORE_STATUS_OK,
		"subtitle paste-over empty no-op");
	if (paste_applied != 0)
		throw std::runtime_error("subtitle paste-over empty no-op applied rows");
	RequireStatus(aegisub_core_subtitle_state_get(session, &state),
		AEGISUB_CORE_STATUS_OK,
		"subtitle state after invalid paste-over");
	if (state.revision != 9
		|| state.row_change_revision != 9
		|| state.row_change_first_row != 0
		|| state.row_change_row_count != 3
		|| state.row_change_fields != AEGISUB_CORE_SUBTITLE_CHANGE_STRUCTURE
		|| state.dirty != 1)
		throw std::runtime_error("subtitle invalid paste-over changed state");
	RequireStatus(aegisub_core_subtitle_paste_over_apply(session, paste_targets, paste_sources, 1, paste_fields, &paste_applied),
		AEGISUB_CORE_STATUS_OK,
		"subtitle paste-over apply");
	if (paste_applied != 1)
		throw std::runtime_error("subtitle paste-over applied wrong row count");
	RequireStatus(aegisub_core_subtitle_row_get_at(session, 0, &first),
		AEGISUB_CORE_STATUS_OK,
		"subtitle first row after paste-over");
	if (first.start_ms != 1500
		|| first.end_ms != 2600
		|| first.margin_left != 44
		|| ToString(first.actor) != "PasteActor"
		|| ToString(first.text) != "Paste over C API")
		throw std::runtime_error("subtitle paste-over did not update selected fields");
	RequireStatus(aegisub_core_subtitle_state_get(session, &state),
		AEGISUB_CORE_STATUS_OK,
		"subtitle state after paste-over");
	if (state.revision != 10
		|| state.row_change_revision != 10
		|| state.row_change_first_row != 0
		|| state.row_change_row_count != 1
		|| state.row_change_fields != (AEGISUB_CORE_SUBTITLE_CHANGE_TEXT
			| AEGISUB_CORE_SUBTITLE_CHANGE_TIME
			| AEGISUB_CORE_SUBTITLE_CHANGE_METADATA)
		|| state.dirty != 1)
		throw std::runtime_error("subtitle state did not track paste-over");
	RequireStatus(aegisub_core_subtitle_paste_over_apply(session, paste_targets, paste_sources, 1, paste_fields, &paste_applied),
		AEGISUB_CORE_STATUS_OK,
		"subtitle same paste-over apply");
	if (paste_applied != 0)
		throw std::runtime_error("subtitle same paste-over applied rows");
	RequireStatus(aegisub_core_subtitle_state_get(session, &state),
		AEGISUB_CORE_STATUS_OK,
		"subtitle state after same paste-over");
	if (state.revision != 10
		|| state.row_change_revision != 10
		|| state.row_change_first_row != 0
		|| state.row_change_row_count != 1
		|| state.row_change_fields != (AEGISUB_CORE_SUBTITLE_CHANGE_TEXT
			| AEGISUB_CORE_SUBTITLE_CHANGE_TIME
			| AEGISUB_CORE_SUBTITLE_CHANGE_METADATA)
		|| state.dirty != 1)
		throw std::runtime_error("subtitle same paste-over changed state");

	ScopedFile saved_path(MakeTempAssPath());
	auto saved_path_text = saved_path.get().string();
	aegisub_core_subtitle_save_options save_options{};
	save_options.abi_version = AEGISUB_CORE_ABI_VERSION;
	save_options.path = {saved_path_text.data(), saved_path_text.size()};
	save_options.encoding = {"utf-8", 5};
	aegisub_core_subtitle_save_options invalid_save_options = save_options;
	invalid_save_options.abi_version = AEGISUB_CORE_ABI_VERSION + 1;
	RequireStatus(aegisub_core_subtitle_save(nullptr, session, &save_options),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"subtitle save null context");
	RequireStatus(aegisub_core_subtitle_save(context, nullptr, &save_options),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"subtitle save null session");
	RequireStatus(aegisub_core_subtitle_save(context, session, nullptr),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"subtitle save null options");
	RequireStatus(aegisub_core_subtitle_save(context, session, &invalid_save_options),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"subtitle save invalid options ABI");
	aegisub_core_subtitle_save_options bad_save_string_options = save_options;
	bad_save_string_options.path = {nullptr, 1};
	RequireStatus(aegisub_core_subtitle_save(context, session, &bad_save_string_options),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"subtitle save invalid path string view");
	if (TakeString(aegisub_core_context_last_error(context)).empty())
		throw std::runtime_error("subtitle save invalid path string view did not set last_error");
	RequireStatus(aegisub_core_subtitle_save(context, session, &save_options),
		AEGISUB_CORE_STATUS_OK,
		"subtitle save");
	if (!TakeString(aegisub_core_context_last_error(context)).empty())
		throw std::runtime_error("subtitle save left stale last_error after success");
	RequireStatus(aegisub_core_subtitle_state_get(session, &state),
		AEGISUB_CORE_STATUS_OK,
		"subtitle state after save");
	if (state.revision != 10
		|| state.row_change_revision != 10
		|| state.row_change_first_row != 0
		|| state.row_change_row_count != 1
		|| state.row_change_fields != (AEGISUB_CORE_SUBTITLE_CHANGE_TEXT
			| AEGISUB_CORE_SUBTITLE_CHANGE_TIME
			| AEGISUB_CORE_SUBTITLE_CHANGE_METADATA)
		|| state.dirty != 0)
		throw std::runtime_error("subtitle save did not clear dirty state while preserving revision");

	aegisub_core_subtitle_session *saved_session = nullptr;
	aegisub_core_subtitle_open_options reopen_options{};
	reopen_options.abi_version = AEGISUB_CORE_ABI_VERSION;
	reopen_options.path = {saved_path_text.data(), saved_path_text.size()};
	reopen_options.encoding = {"utf-8", 5};
	RequireStatus(aegisub_core_subtitle_open(context, &reopen_options, &saved_session),
		AEGISUB_CORE_STATUS_OK,
		"subtitle reopen saved file");
	if (!saved_session)
		throw std::runtime_error("subtitle reopen saved file returned null session");
	RequireStatus(aegisub_core_subtitle_state_get(saved_session, &state),
		AEGISUB_CORE_STATUS_OK,
		"saved subtitle state get");
	if (state.revision != 0
		|| state.row_change_revision != 0
		|| state.row_change_first_row != 0
		|| state.row_change_row_count != 0
		|| state.row_change_fields != AEGISUB_CORE_SUBTITLE_CHANGE_NONE
		|| state.dirty != 0)
		throw std::runtime_error("reopened subtitle session state was not clean");
	RequireStatus(aegisub_core_subtitle_row_get_at(saved_session, 0, &first),
		AEGISUB_CORE_STATUS_OK,
		"saved subtitle first row get");
	RequireStatus(aegisub_core_subtitle_row_get_at(saved_session, 1, &second),
		AEGISUB_CORE_STATUS_OK,
		"saved subtitle second row get");
	aegisub_core_subtitle_row third{};
	RequireStatus(aegisub_core_subtitle_row_get_at(saved_session, 2, &third),
		AEGISUB_CORE_STATUS_OK,
		"saved subtitle third row get");
	if (aegisub_core_subtitle_row_count(saved_session) != 3
		|| first.start_ms != 1500
		|| first.end_ms != 2600
		|| first.margin_left != 44
		|| ToString(first.actor) != "PasteActor"
		|| ToString(first.text) != "Paste over C API"
		|| second.layer != 3
		|| ToString(second.effect) != "patched-effect"
		|| ToString(second.text) != "Patch edit one"
		|| third.start_ms != 1200
		|| third.end_ms != 2800
		|| ToString(third.text) != "Patch edit zero")
		throw std::runtime_error("subtitle save did not round trip edited rows");
	aegisub_core_subtitle_destroy(saved_session);

	aegisub_core_subtitle_destroy(session);
}

void RequireSubtitleEditorBlockOperations(aegisub_core_context *context) {
	ScopedFile ass_path(MakeTempAssPath());
	WriteEditorOpsAss(ass_path.get());

	{
		auto *session = OpenSubtitleForSmoke(context, ass_path.get(), "subtitle tail delete open");
		auto const original_count = aegisub_core_subtitle_row_count(session);
		size_t deleted = 99;
		RequireStatus(aegisub_core_subtitle_rows_delete(session, original_count - 1, 1, &deleted),
			AEGISUB_CORE_STATUS_OK,
			"subtitle tail row delete");
		if (deleted != 1 || aegisub_core_subtitle_row_count(session) != original_count - 1)
			throw std::runtime_error("subtitle tail delete reported the wrong row count");

		aegisub_core_subtitle_info info{};
		RequireStatus(aegisub_core_subtitle_info_get(session, &info),
			AEGISUB_CORE_STATUS_OK,
			"subtitle info after tail delete");
		if (info.row_count != static_cast<int32_t>(original_count - 1))
			throw std::runtime_error("subtitle tail delete did not update info row count");

		aegisub_core_subtitle_state state{};
		RequireStatus(aegisub_core_subtitle_state_get(session, &state),
			AEGISUB_CORE_STATUS_OK,
			"subtitle state after tail delete");
		if (state.revision != 1
			|| state.row_change_revision != 1
			|| state.row_change_first_row != original_count - 1
			|| state.row_change_row_count != 0
			|| state.row_change_fields != AEGISUB_CORE_SUBTITLE_CHANGE_STRUCTURE
			|| state.dirty != 1)
			throw std::runtime_error("subtitle state did not track tail delete empty invalidation window");

		aegisub_core_subtitle_row tail_window[1]{};
		size_t written = 99;
		RequireStatus(aegisub_core_subtitle_rows_get(session, original_count - 1, 0, tail_window, &written),
			AEGISUB_CORE_STATUS_OK,
			"subtitle empty tail row window after tail delete");
		if (written != 0)
			throw std::runtime_error("subtitle empty tail row window after tail delete wrote rows");
		aegisub_core_subtitle_destroy(session);
	}

	{
		auto *session = OpenSubtitleForSmoke(context, ass_path.get(), "subtitle block duplicate open");
		int32_t selected_rows[] = {0, 2, 3};
		int32_t duplicated_rows[] = {-1, -1, -1};
		size_t duplicated_row_count = 99;
		RequireStatus(aegisub_core_subtitle_selected_rows_duplicate(
				session,
				selected_rows,
				3,
				duplicated_rows,
				&duplicated_row_count),
			AEGISUB_CORE_STATUS_OK,
			"subtitle disjoint selected rows duplicate");
		if (duplicated_row_count != 3
			|| duplicated_rows[0] != 1
			|| duplicated_rows[1] != 5
			|| duplicated_rows[2] != 6)
			throw std::runtime_error("subtitle disjoint duplicate returned wrong copied rows");
		RequireTexts(session,
			{"Alpha", "Alpha", "Bravo", "Charlie", "Delta", "Charlie", "Delta", "Echo"},
			"subtitle disjoint duplicate");

		aegisub_core_subtitle_state state{};
		RequireStatus(aegisub_core_subtitle_state_get(session, &state),
			AEGISUB_CORE_STATUS_OK,
			"subtitle state after disjoint duplicate");
		if (state.revision != 1
			|| state.row_change_revision != 1
			|| state.row_change_first_row != 1
			|| state.row_change_row_count != 7
			|| state.row_change_fields != AEGISUB_CORE_SUBTITLE_CHANGE_STRUCTURE
			|| state.dirty != 1)
			throw std::runtime_error("subtitle state did not track disjoint duplicate");
		aegisub_core_subtitle_destroy(session);
	}

	{
		auto *session = OpenSubtitleForSmoke(context, ass_path.get(), "subtitle disjoint sort open");
		int32_t selected_rows[] = {0, 1, 3, 4};
		int32_t sorted_rows[] = {-1, -1, -1, -1};
		size_t sorted_row_count = 99;
		RequireStatus(aegisub_core_subtitle_rows_sort(
				session,
				AEGISUB_CORE_SUBTITLE_SORT_START,
				1,
				selected_rows,
				4,
				sorted_rows,
				&sorted_row_count),
			AEGISUB_CORE_STATUS_OK,
			"subtitle disjoint selected rows sort");
		if (sorted_row_count != 4
			|| sorted_rows[0] != 0
			|| sorted_rows[1] != 1
			|| sorted_rows[2] != 3
			|| sorted_rows[3] != 4)
			throw std::runtime_error("subtitle disjoint selected sort returned wrong selected rows");
		RequireTexts(session,
			{"Bravo", "Alpha", "Charlie", "Echo", "Delta"},
			"subtitle disjoint selected sort");

		aegisub_core_subtitle_state state{};
		RequireStatus(aegisub_core_subtitle_state_get(session, &state),
			AEGISUB_CORE_STATUS_OK,
			"subtitle state after disjoint selected sort");
		if (state.revision != 1
			|| state.row_change_revision != 1
			|| state.row_change_first_row != 0
			|| state.row_change_row_count != 5
			|| state.row_change_fields != AEGISUB_CORE_SUBTITLE_CHANGE_STRUCTURE
			|| state.dirty != 1)
			throw std::runtime_error("subtitle state did not track disjoint selected sort");
		aegisub_core_subtitle_destroy(session);
	}

	{
		auto *session = OpenSubtitleForSmoke(context, ass_path.get(), "subtitle paste-over partial open");
		int32_t targets[] = {0};
		aegisub_core_subtitle_paste_over_source ignored_source{};
		ignored_source.start_ms = 9000;
		ignored_source.end_ms = 1000;
		ignored_source.text = {nullptr, 1};
		size_t applied = 99;
		RequireStatus(aegisub_core_subtitle_paste_over_apply(
				session,
				targets,
				&ignored_source,
				1,
				0,
				&applied),
			AEGISUB_CORE_STATUS_OK,
			"subtitle paste-over zero fields");
		if (applied != 0)
			throw std::runtime_error("subtitle paste-over zero fields applied rows");
		RequireTexts(session,
			{"Alpha", "Bravo", "Charlie", "Delta", "Echo"},
			"subtitle paste-over zero fields");

		int32_t invalid_zero_field_targets[] = {5};
		RequireStatus(aegisub_core_subtitle_paste_over_apply(
				session,
				invalid_zero_field_targets,
				&ignored_source,
				1,
				0,
				&applied),
			AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
			"subtitle paste-over zero fields out-of-range target");
		if (applied != 0)
			throw std::runtime_error("subtitle paste-over zero fields invalid target applied rows");
		int32_t duplicate_zero_field_targets[] = {0, 0};
		aegisub_core_subtitle_paste_over_source ignored_sources[2]{};
		RequireStatus(aegisub_core_subtitle_paste_over_apply(
				session,
				duplicate_zero_field_targets,
				ignored_sources,
				2,
				0,
				&applied),
			AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
			"subtitle paste-over zero fields duplicate targets");
		if (applied != 0)
			throw std::runtime_error("subtitle paste-over zero fields duplicate targets applied rows");

		aegisub_core_subtitle_state state{};
		RequireStatus(aegisub_core_subtitle_state_get(session, &state),
			AEGISUB_CORE_STATUS_OK,
			"subtitle state after paste-over zero fields");
		if (state.revision != 0
			|| state.row_change_revision != 0
			|| state.row_change_first_row != 0
			|| state.row_change_row_count != 0
			|| state.row_change_fields != AEGISUB_CORE_SUBTITLE_CHANGE_NONE
			|| state.dirty != 0)
			throw std::runtime_error("subtitle paste-over zero fields changed state");

		int32_t partial_targets[] = {1, 3};
		aegisub_core_subtitle_paste_over_source sources[2]{};
		sources[0].start_ms = 9000;
		sources[0].end_ms = 1000;
		sources[0].margin_vertical = 77;
		sources[0].actor = {"LeftActor", 9};
		sources[1].start_ms = 8000;
		sources[1].end_ms = 1000;
		sources[1].margin_vertical = 88;
		sources[1].actor = {"RightActor", 10};
		auto const partial_fields = static_cast<aegisub_core_subtitle_paste_over_fields>(
			AEGISUB_CORE_SUBTITLE_PASTE_OVER_ACTOR
			| AEGISUB_CORE_SUBTITLE_PASTE_OVER_MARGIN_VERTICAL);
		RequireStatus(aegisub_core_subtitle_paste_over_apply(
				session,
				partial_targets,
				sources,
				2,
				partial_fields,
				&applied),
			AEGISUB_CORE_STATUS_OK,
			"subtitle paste-over partial metadata fields");
		if (applied != 2)
			throw std::runtime_error("subtitle paste-over partial fields applied wrong row count");

		aegisub_core_subtitle_row row{};
		RequireStatus(aegisub_core_subtitle_row_get_at(session, 1, &row),
			AEGISUB_CORE_STATUS_OK,
			"subtitle paste-over first partial row get");
		if (row.start_ms != 1000
			|| row.end_ms != 2000
			|| row.margin_vertical != 77
			|| ToString(row.actor) != "LeftActor"
			|| ToString(row.text) != "Bravo")
			throw std::runtime_error("subtitle paste-over partial fields changed wrong first row data");
		RequireStatus(aegisub_core_subtitle_row_get_at(session, 3, &row),
			AEGISUB_CORE_STATUS_OK,
			"subtitle paste-over second partial row get");
		if (row.start_ms != 3000
			|| row.end_ms != 4000
			|| row.margin_vertical != 88
			|| ToString(row.actor) != "RightActor"
			|| ToString(row.text) != "Delta")
			throw std::runtime_error("subtitle paste-over partial fields changed wrong second row data");

		RequireStatus(aegisub_core_subtitle_state_get(session, &state),
			AEGISUB_CORE_STATUS_OK,
			"subtitle state after paste-over partial fields");
		if (state.revision != 1
			|| state.row_change_revision != 1
			|| state.row_change_first_row != 1
			|| state.row_change_row_count != 3
			|| state.row_change_fields != AEGISUB_CORE_SUBTITLE_CHANGE_METADATA
			|| state.dirty != 1)
			throw std::runtime_error("subtitle state did not track paste-over partial fields");
		aegisub_core_subtitle_destroy(session);
	}
}

void RequireGridSelectionPolicy() {
	aegisub_core_grid_selection_plan *bad_plan = reinterpret_cast<aegisub_core_grid_selection_plan *>(0x1);
	RequireStatus(aegisub_core_grid_selection_plan_mouse(nullptr, &bad_plan),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"null mouse grid selection input");
	if (bad_plan)
		throw std::runtime_error("null mouse grid selection input did not clear output plan");
	RequireStatus(aegisub_core_grid_selection_plan_mouse(nullptr, nullptr),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"null mouse grid selection output");
	RequireStatus(aegisub_core_grid_selection_plan_info_get(nullptr, nullptr),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"null grid selection plan info get");
	if (aegisub_core_grid_selection_plan_selected_row_count(nullptr) != 0)
		throw std::runtime_error("null grid selection plan reported selected rows");
	RequireStatus(aegisub_core_grid_selection_plan_selected_row_get_at(nullptr, 0, nullptr),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"null grid selection plan row get");
	int32_t selected_window[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
	size_t selected_written = 99;
	RequireStatus(aegisub_core_grid_selection_plan_selected_rows_get(nullptr, 0, 1, selected_window, &selected_written),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"null grid selection plan selected row window get");
	if (selected_written != 0)
		throw std::runtime_error("null grid selection selected row window did not clear written count");

	int32_t selected[] = {1, 2};
	aegisub_core_grid_mouse_selection_input invalid_mouse{};
	invalid_mouse.abi_version = AEGISUB_CORE_ABI_VERSION + 1;
	RequireStatus(aegisub_core_grid_selection_plan_mouse(&invalid_mouse, &bad_plan),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"invalid mouse grid selection ABI");
	if (bad_plan)
		throw std::runtime_error("invalid mouse grid selection ABI returned a plan");

	aegisub_core_grid_mouse_selection_input bad_rows_mouse{};
	bad_rows_mouse.abi_version = AEGISUB_CORE_ABI_VERSION;
	bad_rows_mouse.row_count = 5;
	bad_rows_mouse.target_row = 3;
	bad_rows_mouse.selected_row_count = 1;
	RequireStatus(aegisub_core_grid_selection_plan_mouse(&bad_rows_mouse, &bad_plan),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"null selected rows with non-zero count");

	aegisub_core_grid_mouse_selection_input mouse{};
	mouse.abi_version = AEGISUB_CORE_ABI_VERSION;
	mouse.row_count = 5;
	mouse.target_row = 3;
	mouse.anchor_row = 1;
	mouse.selected_rows = selected;
	mouse.selected_row_count = 2;
	mouse.click = 1;
	mouse.modifiers.shift = 1;
	mouse.modifiers.ctrl = 1;

	aegisub_core_grid_selection_plan *plan = nullptr;
	RequireStatus(aegisub_core_grid_selection_plan_mouse(&mouse, &plan),
		AEGISUB_CORE_STATUS_OK,
		"mouse grid selection plan");
	if (!plan)
		throw std::runtime_error("mouse grid selection returned null plan");

	aegisub_core_grid_selection_plan_info info{};
	RequireStatus(aegisub_core_grid_selection_plan_info_get(plan, &info),
		AEGISUB_CORE_STATUS_OK,
		"mouse grid selection plan info get");
	if (!info.handled
		|| !info.set_active
		|| !info.set_selection
		|| info.active_row != 3
		|| info.anchor_row != 1
		|| info.selected_row_count != 3
		|| info.selected_row_count != aegisub_core_grid_selection_plan_selected_row_count(plan))
		throw std::runtime_error("mouse grid selection plan info was not stable");

	int32_t row = -1;
	RequireStatus(aegisub_core_grid_selection_plan_selected_row_get_at(plan, 0, &row),
		AEGISUB_CORE_STATUS_OK,
		"mouse grid selection selected row get");
	if (row != 1)
		throw std::runtime_error("mouse grid selection selected rows were not stable");
	RequireStatus(aegisub_core_grid_selection_plan_selected_rows_get(plan, 0, 3, selected_window, &selected_written),
		AEGISUB_CORE_STATUS_OK,
		"mouse grid selection selected row window get");
	if (selected_written != 3 || selected_window[0] != 1 || selected_window[1] != 2 || selected_window[2] != 3)
		throw std::runtime_error("mouse grid selection selected row window was not stable");
	RequireStatus(aegisub_core_grid_selection_plan_selected_rows_get(plan, 1, 8, selected_window, &selected_written),
		AEGISUB_CORE_STATUS_OK,
		"mouse grid selection clipped selected row window get");
	if (selected_written != 2 || selected_window[0] != 2 || selected_window[1] != 3)
		throw std::runtime_error("mouse grid selection clipped selected row window was not stable");
	RequireStatus(aegisub_core_grid_selection_plan_selected_rows_get(plan, info.selected_row_count, 1, selected_window, &selected_written),
		AEGISUB_CORE_STATUS_OK,
		"mouse grid selection empty tail selected row window get");
	if (selected_written != 0)
		throw std::runtime_error("mouse grid selection empty tail selected row window wrote rows");
	RequireStatus(aegisub_core_grid_selection_plan_selected_rows_get(plan, info.selected_row_count + 1, 1, selected_window, &selected_written),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"mouse grid selection out-of-range selected row window get");
	if (selected_written != 0)
		throw std::runtime_error("mouse grid selection invalid selected row window did not clear written count");
	RequireStatus(aegisub_core_grid_selection_plan_selected_rows_get(plan, 0, 1, nullptr, &selected_written),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"mouse grid selection null selected row window output");
	RequireStatus(aegisub_core_grid_selection_plan_selected_rows_get(plan, 0, 0, nullptr, &selected_written),
		AEGISUB_CORE_STATUS_OK,
		"mouse grid selection zero selected row window with null output");
	if (selected_written != 0)
		throw std::runtime_error("mouse grid selection zero selected row window wrote rows");
	RequireStatus(aegisub_core_grid_selection_plan_selected_rows_get(plan, 0, 1, selected_window, nullptr),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"mouse grid selection null selected row window written output");
	RequireStatus(aegisub_core_grid_selection_plan_selected_row_get_at(plan, info.selected_row_count, &row),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"mouse grid selection out-of-range selected row get");
	RequireStatus(aegisub_core_grid_selection_plan_selected_row_get_at(plan, 0, nullptr),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"mouse grid selection null selected row output");
	aegisub_core_grid_selection_plan_destroy(plan);

	aegisub_core_grid_keyboard_selection_input keyboard{};
	keyboard.abi_version = AEGISUB_CORE_ABI_VERSION;
	keyboard.row_count = 10;
	keyboard.active_row = 4;
	keyboard.anchor_row = 2;
	keyboard.direction = 1;
	keyboard.step = 3;
	keyboard.modifiers.shift = 1;
	RequireStatus(aegisub_core_grid_selection_plan_keyboard(&keyboard, &plan),
		AEGISUB_CORE_STATUS_OK,
		"keyboard grid selection plan");
	RequireStatus(aegisub_core_grid_selection_plan_info_get(plan, &info),
		AEGISUB_CORE_STATUS_OK,
		"keyboard grid selection plan info get");
	if (!info.handled
		|| info.active_row != 7
		|| info.anchor_row != 2
		|| !info.make_active_visible
		|| info.selected_row_count != 6)
		throw std::runtime_error("keyboard grid selection plan info was not stable");
	RequireStatus(aegisub_core_grid_selection_plan_selected_rows_get(plan, 0, 6, selected_window, &selected_written),
		AEGISUB_CORE_STATUS_OK,
		"keyboard grid selection selected row window get");
	if (selected_written != 6
		|| selected_window[0] != 2
		|| selected_window[1] != 3
		|| selected_window[2] != 4
		|| selected_window[3] != 5
		|| selected_window[4] != 6
		|| selected_window[5] != 7)
		throw std::runtime_error("keyboard grid selection selected row window was not stable");
	aegisub_core_grid_selection_plan_destroy(plan);

	aegisub_core_grid_row_insert_selection_input invalid_insert{};
	invalid_insert.abi_version = AEGISUB_CORE_ABI_VERSION + 1;
	RequireStatus(aegisub_core_grid_selection_plan_row_insert(&invalid_insert, &plan),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"invalid row insert grid selection ABI");
	if (plan)
		throw std::runtime_error("invalid row insert grid selection ABI returned a plan");

	aegisub_core_grid_row_insert_selection_input row_insert{};
	row_insert.abi_version = AEGISUB_CORE_ABI_VERSION;
	row_insert.row_count = 4;
	row_insert.inserted_row = 2;
	RequireStatus(aegisub_core_grid_selection_plan_row_insert(&row_insert, &plan),
		AEGISUB_CORE_STATUS_OK,
		"row insert grid selection plan");
	RequireStatus(aegisub_core_grid_selection_plan_info_get(plan, &info),
		AEGISUB_CORE_STATUS_OK,
		"row insert grid selection plan info get");
	if (!info.handled
		|| !info.set_active
		|| !info.set_selection
		|| !info.make_active_visible
		|| info.active_row != 2
		|| info.anchor_row != 2
		|| info.selected_row_count != 1)
		throw std::runtime_error("row insert grid selection plan info was not stable");
	RequireStatus(aegisub_core_grid_selection_plan_selected_row_get_at(plan, 0, &row),
		AEGISUB_CORE_STATUS_OK,
		"row insert grid selection selected row get");
	if (row != 2)
		throw std::runtime_error("row insert grid selection selected row was not stable");
	aegisub_core_grid_selection_plan_destroy(plan);

	aegisub_core_grid_rows_delete_selection_input invalid_delete{};
	invalid_delete.abi_version = AEGISUB_CORE_ABI_VERSION;
	invalid_delete.row_count_before = 3;
	invalid_delete.first_deleted_row = 3;
	invalid_delete.deleted_row_count = 1;
	RequireStatus(aegisub_core_grid_selection_plan_rows_delete(&invalid_delete, &plan),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"invalid rows delete grid selection range");
	if (plan)
		throw std::runtime_error("invalid rows delete grid selection range returned a plan");

	aegisub_core_grid_rows_delete_selection_input rows_delete{};
	rows_delete.abi_version = AEGISUB_CORE_ABI_VERSION;
	rows_delete.row_count_before = 5;
	rows_delete.first_deleted_row = 3;
	rows_delete.deleted_row_count = 2;
	RequireStatus(aegisub_core_grid_selection_plan_rows_delete(&rows_delete, &plan),
		AEGISUB_CORE_STATUS_OK,
		"rows delete grid selection plan");
	RequireStatus(aegisub_core_grid_selection_plan_info_get(plan, &info),
		AEGISUB_CORE_STATUS_OK,
		"rows delete grid selection plan info get");
	if (!info.handled
		|| !info.set_active
		|| !info.set_selection
		|| !info.make_active_visible
		|| info.active_row != 2
		|| info.anchor_row != 2
		|| info.selected_row_count != 1)
		throw std::runtime_error("rows delete grid selection plan info was not stable");
	aegisub_core_grid_selection_plan_destroy(plan);

	rows_delete = {};
	rows_delete.abi_version = AEGISUB_CORE_ABI_VERSION;
	rows_delete.row_count_before = 2;
	rows_delete.first_deleted_row = 0;
	rows_delete.deleted_row_count = 2;
	RequireStatus(aegisub_core_grid_selection_plan_rows_delete(&rows_delete, &plan),
		AEGISUB_CORE_STATUS_OK,
		"rows delete all grid selection plan");
	RequireStatus(aegisub_core_grid_selection_plan_info_get(plan, &info),
		AEGISUB_CORE_STATUS_OK,
		"rows delete all grid selection plan info get");
	if (!info.handled
		|| !info.set_active
		|| !info.set_selection
		|| info.make_active_visible
		|| info.active_row != -1
		|| info.anchor_row != -1
		|| info.selected_row_count != 0)
		throw std::runtime_error("rows delete all grid selection plan info was not stable");
	aegisub_core_grid_selection_plan_destroy(plan);
}

uint8_t ImmediatePost(void *, aegisub_core_host_task task, void *task_userdata) {
	task(task_userdata);
	return 1;
}

uint8_t IsMainThread(void *) {
	return 1;
}

size_t FlushNoJobs(void *) {
	return 0;
}

// Validates the per-struct struct_size evolution contract:
//   - struct_size == 0 (legacy zero-initialized caller): accepted
//   - struct_size == sizeof(struct): accepted
//   - struct_size < minimum known size: rejected (InvalidArgument)
//   - struct_size > current sizeof (newer caller): accepted, trailing ignored
// Video open options are the representative case; the same ValidateStructSize
// helper guards every options/input struct.
void RequireStructSizeEvolution(aegisub_core_context *context) {
	aegisub_core_video_session *session = nullptr;

	// Legacy zero-initialized caller: struct_size left at 0 must still work.
	aegisub_core_video_open_options legacy_options{};
	legacy_options.abi_version = AEGISUB_CORE_ABI_VERSION;
	char const *dummy_path = "?dummy:24:2:16:8:10:20:30:";
	legacy_options.path = {dummy_path, std::strlen(dummy_path)};
	RequireStatus(aegisub_core_video_open(context, &legacy_options, &session),
		AEGISUB_CORE_STATUS_OK,
		"legacy struct_size==0 video open");
	if (!session)
		throw std::runtime_error("legacy struct_size==0 video open returned no session");
	aegisub_core_video_destroy(session);
	session = nullptr;

	// Explicit struct_size == sizeof must work.
	aegisub_core_video_open_options exact_options{};
	exact_options.abi_version = AEGISUB_CORE_ABI_VERSION;
	exact_options.struct_size = sizeof(aegisub_core_video_open_options);
	exact_options.path = {dummy_path, std::strlen(dummy_path)};
	RequireStatus(aegisub_core_video_open(context, &exact_options, &session),
		AEGISUB_CORE_STATUS_OK,
		"exact struct_size video open");
	if (!session)
		throw std::runtime_error("exact struct_size video open returned no session");
	aegisub_core_video_destroy(session);
	session = nullptr;

	// struct_size smaller than the minimum known size is rejected.
	aegisub_core_video_open_options too_small_options{};
	too_small_options.abi_version = AEGISUB_CORE_ABI_VERSION;
	too_small_options.struct_size = sizeof(uint32_t) + sizeof(size_t) - 1;
	too_small_options.path = {dummy_path, std::strlen(dummy_path)};
	RequireStatus(aegisub_core_video_open(context, &too_small_options, &session),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"too-small struct_size video open");
	if (session)
		throw std::runtime_error("too-small struct_size video open unexpectedly returned a session");

	// struct_size larger than current sizeof is accepted (newer caller); core
	// reads only its known fields and ignores the trailing bytes. We simulate a
	// newer caller by claiming a larger struct_size than core knows.
	aegisub_core_video_open_options newer_options{};
	newer_options.abi_version = AEGISUB_CORE_ABI_VERSION;
	newer_options.struct_size = sizeof(aegisub_core_video_open_options) + 64;
	newer_options.path = {dummy_path, std::strlen(dummy_path)};
	RequireStatus(aegisub_core_video_open(context, &newer_options, &session),
		AEGISUB_CORE_STATUS_OK,
		"newer struct_size video open");
	if (!session)
		throw std::runtime_error("newer struct_size video open returned no session");
	aegisub_core_video_destroy(session);

	// The same matrix must hold for context creation options.
	aegisub_core_context *struct_size_context = nullptr;
	aegisub_core_context_options legacy_ctx{};
	legacy_ctx.abi_version = AEGISUB_CORE_ABI_VERSION;
	RequireStatus(aegisub_core_context_create_with_options(&legacy_ctx, &struct_size_context),
		AEGISUB_CORE_STATUS_OK,
		"legacy struct_size==0 context creation");
	if (!struct_size_context)
		throw std::runtime_error("legacy struct_size==0 context creation returned null");
	aegisub_core_context_destroy(struct_size_context);
	struct_size_context = nullptr;

	aegisub_core_context_options exact_ctx{};
	exact_ctx.abi_version = AEGISUB_CORE_ABI_VERSION;
	exact_ctx.struct_size = sizeof(aegisub_core_context_options);
	RequireStatus(aegisub_core_context_create_with_options(&exact_ctx, &struct_size_context),
		AEGISUB_CORE_STATUS_OK,
		"exact struct_size context creation");
	if (!struct_size_context)
		throw std::runtime_error("exact struct_size context creation returned null");
	aegisub_core_context_destroy(struct_size_context);

	aegisub_core_context_options too_small_ctx{};
	too_small_ctx.abi_version = AEGISUB_CORE_ABI_VERSION;
	too_small_ctx.struct_size = sizeof(uint32_t);
	RequireStatus(aegisub_core_context_create_with_options(&too_small_ctx, &struct_size_context),
		AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
		"too-small struct_size context creation");
	if (struct_size_context)
		throw std::runtime_error("too-small struct_size context creation unexpectedly created a context");
}

} // namespace

int main() {
	try {
		if (aegisub_core_c_header_smoke() != 0)
			throw std::runtime_error("C header smoke failed");
		if (aegisub_core_abi_version() != AEGISUB_CORE_ABI_VERSION)
			throw std::runtime_error("C ABI version mismatch");
		if (AEGISUB_CORE_STATUS_TIMED_OUT != 8 || AEGISUB_CORE_STATUS_ERROR != 100)
			throw std::runtime_error("C ABI status values drifted");
		RequireGridSelectionPolicy();

		RequireStatus(aegisub_core_context_create(nullptr),
			AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
			"null output context creation");
		RequireStatus(aegisub_core_register_builtin_provider_factories(nullptr),
			AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
			"null context builtin provider registration");
		RequireStatus(aegisub_core_finalize_provider_registry(nullptr),
			AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
			"null context provider registry finalization");
		if (aegisub_core_provider_registry_is_finalized(nullptr))
			throw std::runtime_error("null context reported finalized registry");
		if (!TakeString(aegisub_core_context_last_error(nullptr)).empty())
			throw std::runtime_error("null context returned a non-empty last_error");
		if (aegisub_core_provider_catalog_count(nullptr) != 0)
			throw std::runtime_error("null provider catalog reported non-zero count");
		RequireStatus(aegisub_core_provider_catalog_get_at(nullptr, 0, nullptr),
			AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
			"null catalog get_at");
		aegisub_core_provider_descriptor descriptor_window[8]{};
		size_t descriptors_written = 99;
		RequireStatus(aegisub_core_provider_catalog_descriptors_get(nullptr, 0, 1, descriptor_window, &descriptors_written),
			AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
			"null catalog descriptor window get");
		if (descriptors_written != 0)
			throw std::runtime_error("null catalog descriptor window did not clear written count");

		aegisub_core_context *invalid_context = nullptr;
		aegisub_core_context_options invalid_options{};
		invalid_options.abi_version = AEGISUB_CORE_ABI_VERSION + 1;
		RequireStatus(aegisub_core_context_create_with_options(&invalid_options, &invalid_context),
			AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
			"invalid ABI version context creation");
		if (invalid_context)
			throw std::runtime_error("invalid ABI version unexpectedly created a C context");

		aegisub_core_context_options invalid_hook_options{};
		invalid_hook_options.abi_version = AEGISUB_CORE_ABI_VERSION;
		invalid_hook_options.thread_hooks.synchronous_invoke_timeout_ms = 10;
		RequireStatus(aegisub_core_context_create_with_options(&invalid_hook_options, &invalid_context),
			AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
			"host hooks without post_to_main context creation");
		if (invalid_context)
			throw std::runtime_error("invalid host hooks unexpectedly created a C context");

		aegisub_core_context *hook_context = nullptr;
		aegisub_core_context_options hook_options{};
		hook_options.abi_version = AEGISUB_CORE_ABI_VERSION;
		hook_options.thread_hooks.post_to_main = ImmediatePost;
		hook_options.thread_hooks.is_main_thread = IsMainThread;
		hook_options.thread_hooks.flush_main_jobs = FlushNoJobs;
		hook_options.thread_hooks.synchronous_invoke_timeout_ms = 10;
		RequireStatus(aegisub_core_context_create_with_options(&hook_options, &hook_context),
			AEGISUB_CORE_STATUS_OK,
			"custom host hook context creation");
		aegisub_core_context_destroy(hook_context);

		aegisub_core_context *context = nullptr;
		RequireStatus(aegisub_core_context_create(&context),
			AEGISUB_CORE_STATUS_OK,
			"default context creation");
		if (!context)
			throw std::runtime_error("default context creation returned null");
		if (aegisub_core_provider_registry_is_finalized(context))
			throw std::runtime_error("provider registry was finalized before host startup completed");
		RequireStatus(aegisub_core_register_builtin_provider_factories(context),
			AEGISUB_CORE_STATUS_OK,
			"builtin provider factory registration");

		aegisub_core_provider_catalog *bad_catalog = reinterpret_cast<aegisub_core_provider_catalog *>(0x1);
		RequireStatus(aegisub_core_provider_catalog_get(
			nullptr,
			AEGISUB_CORE_PROVIDER_AUDIO,
			{},
			&bad_catalog),
			AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
			"null context provider catalog get");
		if (bad_catalog)
			throw std::runtime_error("null context provider catalog get did not clear output catalog");
		RequireStatus(aegisub_core_provider_catalog_get(
			context,
			AEGISUB_CORE_PROVIDER_AUDIO,
			{},
			nullptr),
			AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
			"null output provider catalog get");

		RequireCatalog(context, AEGISUB_CORE_PROVIDER_AUDIO, "Dummy", "Dummy", 1, "audio provider catalog");
		RequireCatalog(context, AEGISUB_CORE_PROVIDER_VIDEO, "Dummy", "Dummy", 1, "video provider catalog");
		RequireCatalog(context, AEGISUB_CORE_PROVIDER_SUBTITLES, "libass", "libass", 0, "subtitle provider catalog");
		RequireStructSizeEvolution(context);
		RequireDummyVideoOpen(context);
		RequireDummyAudioOpen(context);
		RequireSubtitleOpen(context);
		RequireSubtitleEditorBlockOperations(context);

		aegisub_core_provider_catalog *audio_catalog = nullptr;
		RequireStatus(aegisub_core_provider_catalog_get(
			context,
			AEGISUB_CORE_PROVIDER_AUDIO,
			{"Dummy", 5},
			&audio_catalog),
			AEGISUB_CORE_STATUS_OK,
			"audio catalog for get_at boundary checks");
		aegisub_core_provider_descriptor descriptor{};
		auto const audio_catalog_count = aegisub_core_provider_catalog_count(audio_catalog);
		RequireStatus(aegisub_core_provider_catalog_get_at(audio_catalog, aegisub_core_provider_catalog_count(audio_catalog), &descriptor),
			AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
			"provider catalog out-of-range get_at");
		RequireStatus(aegisub_core_provider_catalog_get_at(audio_catalog, 0, nullptr),
			AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
			"provider catalog null descriptor get_at");
		std::vector<aegisub_core_provider_descriptor> descriptors(audio_catalog_count);
		RequireStatus(aegisub_core_provider_catalog_descriptors_get(
			audio_catalog,
			0,
			audio_catalog_count,
			descriptors.data(),
			&descriptors_written),
			AEGISUB_CORE_STATUS_OK,
			"provider catalog descriptor window get");
		if (descriptors_written != audio_catalog_count || descriptors.empty())
			throw std::runtime_error("provider catalog descriptor window count was not stable");
		RequireStatus(aegisub_core_provider_catalog_descriptors_get(
			audio_catalog,
			audio_catalog_count - 1,
			8,
			descriptor_window,
			&descriptors_written),
			AEGISUB_CORE_STATUS_OK,
			"provider catalog clipped descriptor window get");
		if (descriptors_written != 1)
			throw std::runtime_error("provider catalog clipped descriptor window was not stable");
		RequireStatus(aegisub_core_provider_catalog_descriptors_get(
			audio_catalog,
			audio_catalog_count,
			1,
			descriptor_window,
			&descriptors_written),
			AEGISUB_CORE_STATUS_OK,
			"provider catalog empty tail descriptor window get");
		if (descriptors_written != 0)
			throw std::runtime_error("provider catalog empty tail descriptor window wrote descriptors");
		RequireStatus(aegisub_core_provider_catalog_descriptors_get(
			audio_catalog,
			audio_catalog_count + 1,
			1,
			descriptor_window,
			&descriptors_written),
			AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
			"provider catalog out-of-range descriptor window get");
		if (descriptors_written != 0)
			throw std::runtime_error("provider catalog invalid descriptor window did not clear written count");
		RequireStatus(aegisub_core_provider_catalog_descriptors_get(audio_catalog, 0, 1, nullptr, &descriptors_written),
			AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
			"provider catalog null descriptor window output");
		RequireStatus(aegisub_core_provider_catalog_descriptors_get(audio_catalog, 0, 0, nullptr, &descriptors_written),
			AEGISUB_CORE_STATUS_OK,
			"provider catalog zero descriptor window with null output");
		if (descriptors_written != 0)
			throw std::runtime_error("provider catalog zero descriptor window wrote descriptors");
		RequireStatus(aegisub_core_provider_catalog_descriptors_get(audio_catalog, 0, 1, descriptor_window, nullptr),
			AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
			"provider catalog null descriptor window written output");
		aegisub_core_provider_catalog_destroy(audio_catalog);

		aegisub_core_provider_catalog *invalid_catalog = nullptr;
		RequireStatus(aegisub_core_provider_catalog_get(
			context,
			static_cast<aegisub_core_provider_kind>(255),
			{},
			&invalid_catalog),
			AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
			"invalid provider kind");
		if (invalid_catalog)
			throw std::runtime_error("invalid provider kind returned a catalog");
		if (TakeString(aegisub_core_context_last_error(context)).empty())
			throw std::runtime_error("invalid provider kind did not set last_error");

		aegisub_core_provider_catalog *bad_string_catalog = nullptr;
		RequireStatus(aegisub_core_provider_catalog_get(
			context,
			AEGISUB_CORE_PROVIDER_AUDIO,
			{nullptr, 1},
			&bad_string_catalog),
			AEGISUB_CORE_STATUS_INVALID_ARGUMENT,
			"invalid preferred-provider string view");
		if (bad_string_catalog)
			throw std::runtime_error("invalid string view returned a catalog");
		if (TakeString(aegisub_core_context_last_error(context)).empty())
			throw std::runtime_error("invalid string view did not set last_error");

		RequireStatus(aegisub_core_finalize_provider_registry(context),
			AEGISUB_CORE_STATUS_OK,
			"provider registry finalization");
		if (!aegisub_core_provider_registry_is_finalized(context))
			throw std::runtime_error("provider registry did not report finalized after finalization");
		RequireStatus(aegisub_core_register_builtin_provider_factories(context),
			AEGISUB_CORE_STATUS_REGISTRY_FINALIZED,
			"post-finalize builtin provider factory registration");
		RequireCatalog(context, AEGISUB_CORE_PROVIDER_AUDIO, "Dummy", "Dummy", 1, "post-finalize audio provider catalog");

		aegisub_core_context_destroy(context);
	}
	catch (std::exception const& err) {
		std::cerr << "core_c_api_smoke failed: " << err.what() << "\n";
		return 1;
	}

	std::cout << "core_c_api_smoke: C ABI context, strings, catalogs, grid selection policy, open diagnostics, audio/video/subtitle metadata sessions, and finalization are stable\n";
	return 0;
}
