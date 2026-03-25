#pragma once

#include <libaegisub/fs_fwd.h>

#include "video_memory_stats.h"

#include <cstdint>
#include <string>

namespace perf_trace {

enum class Category {
	Ops,
	Video,
	Audio,
	Memory,
	LuaDialog,
	Log,
};

struct AudioOutputSnapshot {
	std::string backend_name;
	std::string reason;
	int64_t queued_buffers = -1;
	double queued_ms = -1.0;
	int64_t submitted_buffers = -1;
	int64_t submitted_frames = -1;
	int64_t submitted_bytes = -1;
	double submitted_ms = -1.0;
	double fill_duration_ms = -1.0;
	bool low_water = false;
	bool starved = false;
	bool recovered = false;
	bool end_of_stream = false;
};

bool IsEnabled();
bool IsCategoryEnabled(Category category);
bool ShouldSampleVideoMemory(bool force = false);
agi::fs::path GetSessionDirectory();

void Initialize(std::string const& build_label = {});
void InitializeAt(agi::fs::path const& session_dir, std::string const& build_label = {}, std::string const& source_tag = {});
void Shutdown();

void ResetAudioPlaybackInterval();
void ResetVideoPlaybackInterval();

void TraceVideoOpen(agi::fs::path const& path, int width, int height, int frame_count, bool has_audio, std::string const& decoder_name, double duration_ms);
void TracePlayStart(int frame, int start_ms);
void TracePlayStop(int frame);
void TraceSeek(int frame, bool was_playing);

void ObserveFrameRequest(int frame, double time, bool immediate);
void ObserveFrameResult(int frame, double time, bool delivered, bool immediate);
void ObserveAudioPlaybackPosition(int ms);
void ObserveAudioOutputSnapshot(AudioOutputSnapshot const& snapshot);
void ObserveVideoPlaybackTick(int frame);

void TraceLuaDialogOpenBegin();
void TraceLuaDialogOpenEnd(int control_count, int button_count, double duration_ms, bool succeeded);
void ObserveVideoMemorySnapshot(char const* reason, VideoMemorySnapshot const& snapshot, bool force = false);

}
