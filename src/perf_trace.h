#pragma once

#include <libaegisub/fs_fwd.h>

#include "video_memory_stats.h"

#include <string>

namespace perf_trace {

bool IsEnabled();
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
void ObserveVideoPlaybackTick(int frame);

void TraceLuaDialogOpenBegin();
void ObserveLuaDialogPhase(char const* phase, int control_count, int button_count, double duration_ms);
void ObserveLuaDialogControlTypeSummary(char const* control_type, int control_count, int button_count, int instance_count, int item_count_total, int item_count_max, double duration_ms);
void ObserveLuaDialogControlStepSummary(char const* control_type, char const* step, int control_count, int button_count, int instance_count, int item_count_total, int item_count_max, double duration_ms);
void TraceLuaDialogOpenEnd(int control_count, int button_count, double duration_ms, bool succeeded);
void ObserveVideoMemorySnapshot(char const* reason, VideoMemorySnapshot const& snapshot, bool force = false);

}
