// Copyright (c) 2008-2009, Karl Blomster
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the name of the Aegisub Group nor the names of its contributors
//     may be used to endorse or promote products derived from this software
//     without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// Aegisub Project http://www.aegisub.org/

/// @file ffmpegsource_common.cpp
/// @brief Shared code for ffms video and audio providers
/// @ingroup video_input audio_input ffms
///

#ifdef WITH_FFMS2
#include "ffmpegsource_common.h"

#include "mkv_wrap.h"
#include <libaegisub/native_library.h>
#include "options.h"
#include "provider_index_cache.h"
#include "track_choice.h"
#include "translation_service.h"
#include "ui_services.h"

#include <libaegisub/background_runner.h>
#include <libaegisub/log.h>
#include <libaegisub/exception.h>
#include <libaegisub/fs.h>
#include <libaegisub/string_utils.h>

#include <filesystem>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#ifdef CreateDirectory
#undef CreateDirectory
#endif

#if FFMS_VERSION < ((2 << 24) | (22 << 16) | (0 << 8) | 0)
enum {
	FFMS_LOG_QUIET = -8,
	FFMS_LOG_PANIC = 0,
	FFMS_LOG_FATAL = 8,
	FFMS_LOG_ERROR = 16,
	FFMS_LOG_WARNING = 24,
	FFMS_LOG_INFO = 32,
	FFMS_LOG_VERBOSE = 40,
	FFMS_LOG_DEBUG = 48,
	FFMS_LOG_TRACE = 56
};
#endif

namespace ffms {
#ifdef WITH_FFMS2_RUNTIME_LOADING
	#define AGI_FFMS2_FN(name) decltype(&FFMS_##name) name = nullptr;
	#include "ffms2_functions.inc"
	#undef AGI_FFMS2_FN

namespace {
	int loaded_version = -1;

	std::string GetLibraryName() {
#ifdef FFMS2_SO
		return FFMS2_SO;
#else
		return "ffms2";
#endif
	}

	std::string FormatFFMSVersion(int version) {
		if (version < 0)
			return "unknown";
		return std::to_string((version >> 24) & 0xFF)
			+ "." + std::to_string((version >> 16) & 0xFF)
			+ "." + std::to_string((version >> 8) & 0xFF)
			+ "." + std::to_string(version & 0xFF);
	}

	std::string GetVersionContext() {
		return "headers=" + FormatFFMSVersion(FFMS_VERSION) + ", dll=" + FormatFFMSVersion(loaded_version);
	}

	std::string GetVersionContextForCache() {
		return loaded_version >= 0 ? GetVersionContext() : std::string();
	}

	agi::native::LibraryLoadOptions GetRuntimeLoadOptions() {
		return agi::native::DefaultAppLocalLoadOptions(false);
	}

	template <typename T>
	void LoadSymbol(agi::native::Library& library, T& target, const char *name) {
		target = library.ResolveSymbol<T>(name);
	}

	void ResolveSymbols(agi::native::Library& library) {
		#define AGI_FFMS2_FN(name) LoadSymbol(library, ffms::name, "FFMS_" #name);
		#include "ffms2_functions.inc"
		#undef AGI_FFMS2_FN
	}

	void InitializeFFMS2Runtime(agi::native::Library& library) {
		loaded_version = library.ResolveSymbol<int (FFMS_CC*)()>("FFMS_GetVersion")();
		ResolveSymbols(library);
	}

	agi::native::CachedLibrary runtime_library(GetLibraryName(), "FFMS2", "provider/ffms2/runtime",
		InitializeFFMS2Runtime, GetVersionContextForCache, GetRuntimeLoadOptions());
}

	void EnsureLoaded() {
		runtime_library.EnsureLoaded();
		if (loaded_version >= 0 && loaded_version != FFMS_VERSION)
			LOG_W("provider/ffms2/runtime") << "FFMS2 header/DLL version mismatch: " << GetVersionContext();
	}

	bool IsAvailable() noexcept {
		return runtime_library.IsAvailable();
	}

	int GetLoadedVersionNumber() noexcept {
		return loaded_version;
	}

	std::string GetLoadError() {
		return runtime_library.GetLoadError();
	}

	std::string GetLoadedLibrary() {
		return runtime_library.GetLoadedLibrary();
	}
#endif
}

FFmpegSourceProvider::FFmpegSourceProvider(agi::BackgroundRunner *br, std::shared_ptr<agi::SingleChoiceInteractionSink> choice_sink)
: br(br)
, choice_sink(std::move(choice_sink))
{
	ffms::EnsureLoaded();
	ffms::Init(0, 0);
}

namespace {
std::string GetConfiguredFFmpegSourceLogLevel() {
	return config::GetStringOptionOrDefault("Provider/FFmpegSource/Log Level", "quiet");
}

std::string GetConfiguredFFmpegSourceDecodeErrorHandling() {
	return config::GetStringOptionOrDefault("Provider/Audio/FFmpegSource/Decode Error Handling", "ignore");
}

std::string FormatTrackLabel(int ffms_track_index, std::string const& codec_name, std::string const& channels = {}, std::string const& language = {}, std::string const& title = {}) {
	aegisub::track_choice::TrackLabel label;
	label.index = ffms_track_index;
	label.codec = codec_name;
	label.details = { channels, language };
	label.title = title;
	return aegisub::track_choice::FormatTrackLabel(label);
}

bool IsMatroskaLikePath(agi::fs::path const& filename) {
	return agi::fs::HasExtension(filename, "mkv")
		|| agi::fs::HasExtension(filename, "mka")
		|| agi::fs::HasExtension(filename, "mks")
		|| agi::fs::HasExtension(filename, "mk3d")
		|| agi::fs::HasExtension(filename, "webm");
}

std::string to_lower_copy(std::string value) {
	agi::util::strings::to_lower_inplace(value);
	return value;
}

std::string NormalizeMkvAudioCodecId(std::string_view codec_id) {
	if (codec_id.empty())
		return {};
	if (codec_id == "A_AAC" || agi::util::strings::starts_with(codec_id, "A_AAC/"))
		return "aac";
	if (codec_id == "A_AC3")
		return "ac3";
	if (codec_id == "A_EAC3")
		return "eac3";
	if (codec_id == "A_OPUS")
		return "opus";
	if (codec_id == "A_FLAC")
		return "flac";
	if (codec_id == "A_VORBIS")
		return "vorbis";
	if (codec_id == "A_MPEG/L3")
		return "mp3";
	if (codec_id == "A_MPEG/L2")
		return "mp2";
	if (codec_id == "A_TRUEHD")
		return "truehd";
	if (codec_id == "A_DTS")
		return "dts";
	if (codec_id == "A_PCM/INT/LIT")
		return "pcm_s16le";
	if (codec_id == "A_PCM/INT/BIG")
		return "pcm_s16be";
	return {};
}

bool CodecNamesSeemCompatible(std::string const& ffms_codec_name, std::string_view mkv_codec_id) {
	auto const normalized_mkv = NormalizeMkvAudioCodecId(mkv_codec_id);
	if (normalized_mkv.empty())
		return true;
	return normalized_mkv == to_lower_copy(ffms_codec_name);
}

void TryEnrichAudioTracksFromMkv(agi::fs::path const& filename, std::vector<FFmpegSourceProvider::TrackChoice> &track_list) {
#if AEGISUB_MATROSKA_PARSING
	if (track_list.empty() || !IsMatroskaLikePath(filename))
		return;

	try {
		auto scan = MatroskaWrapper::ScanTracks(filename);
		std::vector<MkvTrackInfo const*> mkv_audio_tracks;
		mkv_audio_tracks.reserve(scan.tracks.size());
		for (auto const& track : scan.tracks) {
			if (track.type == MkvTrackType::Audio)
				mkv_audio_tracks.push_back(&track);
		}

		if (mkv_audio_tracks.size() != track_list.size()) {
			LOG_D("provider/ffms2/mkv") << "Skipping MKV audio metadata enrichment for " << agi::fs::PathToString(filename)
				<< ": FFMS audio tracks=" << track_list.size() << ", MKV audio tracks=" << mkv_audio_tracks.size();
			return;
		}

		for (size_t i = 0; i < track_list.size(); ++i) {
			if (!CodecNamesSeemCompatible(track_list[i].codec_name, mkv_audio_tracks[i]->codec_id)) {
				LOG_D("provider/ffms2/mkv") << "Skipping MKV audio metadata enrichment for " << agi::fs::PathToString(filename)
					<< ": codec mismatch at audio ordinal " << i
					<< " (ffms=" << track_list[i].codec_name
					<< ", mkv=" << mkv_audio_tracks[i]->codec_id << ")";
				return;
			}
		}

		for (size_t i = 0; i < track_list.size(); ++i) {
			auto const& mkv_track = *mkv_audio_tracks[i];
			track_list[i].display_name = FormatTrackLabel(
				track_list[i].ffms_track_index,
				track_list[i].codec_name,
				FormatMkvAudioChannelCount(mkv_track.audio_channels),
				GetPreferredMkvTrackLanguage(mkv_track),
				mkv_track.name);
		}
	}
	catch (agi::Exception const& e) {
		LOG_D("provider/ffms2/mkv") << "Failed to enrich MKV audio track metadata for " << agi::fs::PathToString(filename) << ": " << e.GetMessage();
	}
	catch (std::exception const& e) {
		LOG_D("provider/ffms2/mkv") << "Failed to enrich MKV audio track metadata for " << agi::fs::PathToString(filename) << ": " << e.what();
	}
#else
	(void)filename;
	(void)track_list;
#endif
}
}

/// @brief Does indexing of a source file
/// @param Indexer		A pointer to the indexer object representing the file to be indexed
/// @param CacheName    The filename of the output index file
/// @param Trackmask    A binary mask of the track numbers to index
FFMS_Index *FFmpegSourceProvider::DoIndexing(FFMS_Indexer *Indexer,
	                                         agi::fs::path const& CacheName,
	                                         TrackSelection Track,
	                                         FFMS_IndexErrorHandling IndexEH) {
	char FFMSErrMsg[1024];
	FFMS_ErrorInfo ErrInfo;
	ErrInfo.Buffer		= FFMSErrMsg;
	ErrInfo.BufferSize	= sizeof(FFMSErrMsg);
	ErrInfo.ErrorType	= FFMS_ERROR_SUCCESS;
	ErrInfo.SubType		= FFMS_ERROR_SUCCESS;

	// index all audio tracks
	FFMS_Index *Index;
	br->Run([&](agi::ProgressSink *ps) {
		ps->SetTitle(_("Indexing"));
		ps->SetMessage(_("Reading timecodes and frame/sample data"));
		TIndexCallback callback = [](int64_t Current, int64_t Total, void *Private) -> int {
			auto ps = static_cast<agi::ProgressSink *>(Private);
			ps->SetProgress(Current, Total);
			return ps->IsCancelled();
		};
#if FFMS_VERSION >= ((2 << 24) | (21 << 16) | (0 << 8) | 0)
		if (Track == TrackSelection::All)
			ffms::TrackTypeIndexSettings(Indexer, FFMS_TYPE_AUDIO, 1, 0);
		else if (Track != TrackSelection::None)
			ffms::TrackIndexSettings(Indexer, static_cast<int>(Track), 1, 0);
		ffms::SetProgressCallback(Indexer, callback, ps);
		Index = ffms::DoIndexing2(Indexer, IndexEH, &ErrInfo);
#else
		int Trackmask = 0;
		if (Track == TrackSelection::All)
			Trackmask = std::numeric_limits<int>::max();
		else if (Track != TrackSelection::None)
			Trackmask = 1 << static_cast<int>(Track);
		Index = ffms::DoIndexing(Indexer, Trackmask, 0,
			nullptr, nullptr, IndexEH, callback, ps, &ErrInfo);
#endif
	});

	if (Index == nullptr)
		throw agi::EnvironmentError(std::string("Failed to index: ") + ErrInfo.Buffer);

	// write index to disk for later use
	auto const cache_name_utf8 = agi::fs::PathToString(CacheName);
	ffms::WriteIndex(cache_name_utf8.c_str(), Index, &ErrInfo);

	return Index;
}

/// @brief Finds all tracks of the given type and return their FFMS indices and display strings
/// @param Indexer	The indexer object representing the source file
/// @param Type		The track type to look for
std::vector<FFmpegSourceProvider::TrackChoice> FFmpegSourceProvider::GetTracksOfType(agi::fs::path const& filename, FFMS_Indexer *Indexer, FFMS_TrackType Type) {
	std::vector<TrackChoice> TrackList;
	int NumTracks = ffms::GetNumTracksI(Indexer);

	// older versions of ffms2 can't index audio tracks past 31
#if FFMS_VERSION < ((2 << 24) | (21 << 16) | (0 << 8) | 0)
	if (Type == FFMS_TYPE_AUDIO)
		NumTracks = std::min(NumTracks, std::numeric_limits<int>::digits);
#endif

	for (int i = 0; i < NumTracks; i++) {
		if (ffms::GetTrackTypeI(Indexer, i) == Type) {
			if (auto CodecName = ffms::GetCodecNameI(Indexer, i)) {
				TrackChoice choice;
				choice.ffms_track_index = i;
				choice.codec_name = CodecName;
				choice.display_name = FormatTrackLabel(i, choice.codec_name);
				TrackList.emplace_back(std::move(choice));
			}
		}
	}

	if (Type == FFMS_TYPE_AUDIO)
		TryEnrichAudioTracksFromMkv(filename, TrackList);

	return TrackList;
}

FFmpegSourceProvider::TrackSelection
FFmpegSourceProvider::AskForTrackSelection(std::vector<TrackChoice> const& TrackList,
                                           FFMS_TrackType Type) {
	std::vector<std::string> choices;
	choices.reserve(TrackList.size());
	for (auto const& track : TrackList) {
		choices.push_back(track.display_name);
	}

	if (!choice_sink)
		return TrackSelection::None;

	auto choice = choice_sink->RequestSingleChoice(aegisub::track_choice::BuildRequest(
		Type == FFMS_TYPE_VIDEO ? aegisub::track_choice::DialogKind::Video : aegisub::track_choice::DialogKind::Audio,
		choices));
	auto resolved = aegisub::track_choice::ResolveSelection(TrackList.size(), choice);
	if (!resolved)
		return TrackSelection::None;
	return static_cast<TrackSelection>(TrackList[*resolved].ffms_track_index);
}

/// @brief Set ffms2 log level according to setting in config.dat
void FFmpegSourceProvider::SetLogLevel() {
	auto LogLevel = GetConfiguredFFmpegSourceLogLevel();
	agi::util::strings::to_lower_inplace(LogLevel);

	if (LogLevel == "panic")
		ffms::SetLogLevel(FFMS_LOG_PANIC);
	else if (LogLevel == "fatal")
		ffms::SetLogLevel(FFMS_LOG_FATAL);
	else if (LogLevel == "error")
		ffms::SetLogLevel(FFMS_LOG_ERROR);
	else if (LogLevel == "warning")
		ffms::SetLogLevel(FFMS_LOG_WARNING);
	else if (LogLevel == "info")
		ffms::SetLogLevel(FFMS_LOG_INFO);
	else if (LogLevel == "verbose")
		ffms::SetLogLevel(FFMS_LOG_VERBOSE);
	else if (LogLevel == "debug")
		ffms::SetLogLevel(FFMS_LOG_DEBUG);
	else
		ffms::SetLogLevel(FFMS_LOG_QUIET);
}

FFMS_IndexErrorHandling FFmpegSourceProvider::GetErrorHandlingMode() {
	auto Mode = GetConfiguredFFmpegSourceDecodeErrorHandling();
	agi::util::strings::to_lower_inplace(Mode);

	if (Mode == "ignore")
		return FFMS_IEH_IGNORE;
	if (Mode == "clear")
		return FFMS_IEH_CLEAR_TRACK;
	if (Mode == "stop")
		return FFMS_IEH_STOP_TRACK;
	if (Mode == "abort")
		return FFMS_IEH_ABORT;
	return FFMS_IEH_STOP_TRACK; // questionable default?
}

/// @brief	Generates an unique name for the ffms2 index file and prepares the cache folder if it doesn't exist
/// @param filename	The name of the source file
/// @return			Returns the generated filename.
agi::fs::path FFmpegSourceProvider::GetCacheFilename(agi::fs::path const& filename) {
	return aegisub::provider_index_cache::BuildFilename(filename, "?local/ffms2cache/", ".ffindex");
}

void FFmpegSourceProvider::CleanCache() {
	aegisub::provider_index_cache::Clean("?local/ffms2cache/",
		"*.ffindex",
		"Provider/FFmpegSource/Cache/Size",
		"Provider/FFmpegSource/Cache/Files");
}

#endif // WITH_FFMS2
