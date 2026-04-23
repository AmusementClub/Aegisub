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
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

#include "media_inspect_service.h"

#include "headless_playback_session_host.h"
#include "include/aegisub/context.h"
#include "mkv_wrap.h"
#include "provider_selection_diagnostics.h"

#include <libaegisub/fs.h>
#include <libaegisub/io.h>

#include <fstream>
#include <utility>

namespace aegisub::media_inspect_service {
namespace {

std::string Sanitize(std::string const& value) {
	return provider_selection_diagnostics::SanitizeText(value);
}

std::string ToGenericString(agi::fs::path const& path) {
	return agi::fs::PathToGenericString(path);
}

void AppendTrackChoices(std::vector<TrackChoiceInfo>& target, std::vector<std::string> const& labels) {
	target.reserve(labels.size());
	for (size_t i = 0; i < labels.size(); ++i) {
		target.push_back({
			static_cast<int>(i),
			labels[i]
		});
	}
}

void PopulateMatroskaTrackChoices(agi::fs::path const& path, MediaInspectResult& result) {
	if (path.empty())
		return;
	if (!agi::fs::HasExtension(path, "mkv")
		&& !agi::fs::HasExtension(path, "mka")
		&& !agi::fs::HasExtension(path, "mks")
		&& !agi::fs::HasExtension(path, "mk3d")
		&& !agi::fs::HasExtension(path, "webm")) {
		return;
	}

	try {
		auto scan = MatroskaWrapper::ScanTracks(path);
		std::vector<std::string> video_choices;
		std::vector<std::string> audio_choices;
		std::vector<std::string> subtitle_choices;
		for (auto const& track : scan.tracks) {
			auto label = DescribeMkvTrack(track);
			switch (track.type) {
			case MkvTrackType::Video:
				video_choices.push_back(std::move(label));
				break;
			case MkvTrackType::Audio:
				audio_choices.push_back(std::move(label));
				break;
			case MkvTrackType::Subtitle:
				if (IsImportableMkvSubtitleTrack(track))
					subtitle_choices.push_back(std::move(label));
				break;
			default:
				break;
			}
		}

		AppendTrackChoices(result.video_track_choices, video_choices);
		AppendTrackChoices(result.audio_track_choices, audio_choices);
		AppendTrackChoices(result.subtitle_track_choices, subtitle_choices);
	}
	catch (...) {
	}
}

void WriteManifest(MediaInspectRequest const& request, MediaInspectResult const& result) {
	if (result.trace_dir.empty())
		return;

	auto out = agi::io::OpenOutputFileStream(result.trace_dir / "manifest.txt", std::ios::out | std::ios::app);
	if (!out)
		return;

	out << "command=inspect media\n";
	out << "video=" << Sanitize(ToGenericString(request.video_path)) << "\n";
	out << "audio=" << Sanitize(ToGenericString(request.audio_path)) << "\n";
	out << "skip_audio=" << headless_playback_session_host::BoolString(request.skip_audio) << "\n";
}

void WriteSummary(MediaInspectResult const& result) {
	if (result.trace_dir.empty())
		return;

	auto out = agi::io::OpenOutputFileStream(result.trace_dir / "summary.txt", std::ios::out | std::ios::app);
	if (!out)
		return;

	auto write_value = [&](char const* key, std::string const& value) {
		out << key << "=" << Sanitize(value) << "\n";
	};
	auto write_bool = [&](char const* key, bool value) {
		out << key << "=" << headless_playback_session_host::BoolString(value) << "\n";
	};

	write_bool("opened", result.opened);
	write_value("selected.video_provider", result.selected_video_provider);
	write_value("selected.audio_provider", result.selected_audio_provider);
	write_value("actual.video_provider", result.actual_video_provider);
	write_value("actual.video_decoder", result.actual_video_decoder);
	write_bool("video.provider_fallback", result.video_provider_fallback);
	write_value("video.provider_fallback_reason", result.video_provider_fallback_reason);
	write_value("video.provider_attempts", result.video_provider_attempts);
	write_value("actual.audio_provider_factory", result.actual_audio_provider_factory);
	write_value("actual.audio_provider", result.actual_audio_provider);
	write_bool("audio.provider_fallback", result.audio_provider_fallback);
	write_value("audio.provider_fallback_reason", result.audio_provider_fallback_reason);
	write_value("audio.provider_attempts", result.audio_provider_attempts);
	for (auto const& track : result.video_track_choices)
		write_value(("track.video.choice." + std::to_string(track.choice_index)).c_str(), track.display_name);
	for (auto const& track : result.audio_track_choices)
		write_value(("track.audio.choice." + std::to_string(track.choice_index)).c_str(), track.display_name);
	for (auto const& track : result.subtitle_track_choices)
		write_value(("track.subtitle.choice." + std::to_string(track.choice_index)).c_str(), track.display_name);
	write_bool("media.has_video", result.media.has_video);
	write_bool("media.has_audio", result.media.has_audio);
	out << "media.video_width=" << result.media.video_width << "\n";
	out << "media.video_height=" << result.media.video_height << "\n";
	out << "media.video_frame_count=" << result.media.video_frame_count << "\n";
	out << "media.video_duration_ms=" << result.media.video_duration_ms << "\n";
	write_value("media.video_decoder_name", result.media.video_decoder_name);
	out << "media.audio_sample_rate=" << result.media.audio_sample_rate << "\n";
	out << "media.audio_num_samples=" << result.media.audio_num_samples << "\n";
	out << "media.audio_duration_ms=" << result.media.audio_duration_ms << "\n";
	write_value("media.audio_provider_name", result.media.audio_provider_name);
	write_bool("playback.has_video", result.playback.has_video);
	write_bool("playback.has_audio", result.playback.has_audio);
	write_bool("playback.video_playing", result.playback.video_playing);
	write_bool("playback.audio_playing", result.playback.audio_playing);
	write_bool("playback.uses_audio_authority", result.playback.playback_uses_audio_authority);
	out << "playback.current_frame=" << result.playback.current_frame << "\n";
	out << "playback.current_video_time_ms=" << result.playback.current_video_time_ms << "\n";
	out << "playback.current_audio_time_ms=" << result.playback.current_audio_time_ms << "\n";
	out << "playback.primary_begin_ms=" << result.playback.primary_playback_begin_ms << "\n";
	out << "playback.primary_end_ms=" << result.playback.primary_playback_end_ms << "\n";
	out << "exit_code=" << result.exit_code << "\n";
	out << "result=" << (result.exit_code == 0 ? "PASS" : "FAIL") << "\n";
	write_value("message", result.message);
}

}

MediaInspectResult Inspect(MediaInspectRequest const& request) {
	using headless_playback_session_host::DescribeProviderFallback;
	using headless_playback_session_host::FormatProviderAttempts;
	using headless_playback_session_host::PlaybackSessionHost;
	using headless_playback_session_host::PlaybackSessionHostOptions;
	using headless_playback_session_host::UsedProviderFallback;

	MediaInspectResult result;
	PlaybackSessionHost host(PlaybackSessionHostOptions{
		request.video_provider,
		request.audio_provider,
		request.trace_dir,
		request.audio_rate_scale,
		request.audio_quantum_ms,
		"headless-inspect-media-%%%%%%%%",
		{
			request.video_track_index,
			request.audio_track_index,
			request.subtitle_track_index,
			true
		}
	});

	int start_error_code = 0;
	std::string start_error_message;
	if (!host.Start(start_error_code, start_error_message)) {
		result.trace_dir = host.TraceDir();
		result.exit_code = start_error_code ? start_error_code : 2;
		result.message = start_error_message.empty() ? "failed to start media inspect host" : start_error_message;
		host.ShutdownTrace();
		WriteManifest(request, result);
		WriteSummary(result);
		host.ReleaseResources();
		return result;
	}

	auto open_result = host.OpenMedia({
		request.video_path,
		request.skip_audio ? std::optional<agi::fs::path>{} : std::make_optional(request.audio_path),
		request.skip_audio,
	});

	result.trace_dir = host.TraceDir();
	result.selected_video_provider = host.SelectedVideoProvider();
	result.selected_audio_provider = host.SelectedAudioProvider();
	result.actual_video_provider = host.ActualVideoProvider();
	result.actual_video_decoder = host.ActualVideoDecoder();
	result.video_provider_fallback = UsedProviderFallback(host.VideoProviderReport());
	result.video_provider_fallback_reason = DescribeProviderFallback(host.VideoProviderReport());
	result.video_provider_attempts = FormatProviderAttempts(host.VideoProviderReport());
	result.actual_audio_provider_factory = host.ActualAudioProviderFactory();
	result.actual_audio_provider = host.ActualAudioProvider();
	result.audio_provider_fallback = UsedProviderFallback(host.AudioProviderReport());
	result.audio_provider_fallback_reason = DescribeProviderFallback(host.AudioProviderReport());
	result.audio_provider_attempts = FormatProviderAttempts(host.AudioProviderReport());
	PopulateMatroskaTrackChoices(request.video_path.empty() ? request.audio_path : request.video_path, result);

	if (!open_result.opened) {
		result.exit_code = open_result.error_code ? open_result.error_code : 8;
		result.message = open_result.error.empty() ? "failed to open project media" : open_result.error;
		host.CloseMedia();
		host.ShutdownTrace();
		WriteManifest(request, result);
		WriteSummary(result);
		host.ReleaseResources();
		return result;
	}

	auto core = host.GetCore();
	result.media = playback_query_service::QueryProjectMedia(core);
	result.playback = playback_query_service::QueryPlaybackState(core);
	result.opened = true;
	result.exit_code = 0;

	host.CloseMedia();
	host.ShutdownTrace();
	WriteManifest(request, result);
	WriteSummary(result);
	host.ReleaseResources();
	return result;
}

}
