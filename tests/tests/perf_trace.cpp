#include <main.h>

#include "../../src/perf_trace.h"

#include <libaegisub/fs.h>
#include <libaegisub/log.h>
#include <libaegisub/path.h>

#include <fstream>
#include <sstream>

namespace {
std::string ReadAll(agi::fs::path const& path) {
	std::ifstream in(path, std::ios::in | std::ios::binary);
	std::ostringstream out;
	out << in.rdbuf();
	return out.str();
}

std::string Utf8PathSegment() {
	return "\xE8\xB7\xAF\xE5\xBE\x84";
}
}

TEST(PerfTrace, WritesExpectedSessionFiles) {
	agi::Path path_helper;
	auto const session_dir = agi::fs::UniquePath(path_helper.Decode("?temp/perf_trace_session_%%%%%%%%"));

	perf_trace::InitializeAt(session_dir, "test-build", "unit-test");
	ASSERT_TRUE(perf_trace::IsEnabled());

	perf_trace::TraceSeek(12, false);
	perf_trace::ObserveFrameRequest(12, 0.5, false);
	perf_trace::ObserveFrameResult(12, 0.5, true, false);
	perf_trace::ObserveFrameResult(11, 0.458333, false, false);
	perf_trace::ResetAudioUiTimerInterval();
	perf_trace::ObserveAudioUiTimerPosition(100);
	perf_trace::ObserveAudioUiTimerPosition(120);
	perf_trace::ObserveAudioUiDuration("audio_display.paint", 3.5, 640, 2, false);
	perf_trace::AudioOutputSnapshot output_snapshot;
	output_snapshot.backend_name = "xaudio2";
	output_snapshot.reason = "unit_test";
	output_snapshot.queued_buffers = 2;
	output_snapshot.queued_ms = 40.0;
	output_snapshot.submitted_buffers = 1;
	output_snapshot.submitted_frames = 960;
	output_snapshot.submitted_bytes = 3840;
	output_snapshot.submitted_ms = 20.0;
	output_snapshot.fill_duration_ms = 1.25;
	output_snapshot.low_water = true;
	perf_trace::ObserveAudioOutputSnapshot(output_snapshot);
	perf_trace::ResetVideoPlaybackInterval();
	perf_trace::ObserveVideoPlaybackTick(12);
	perf_trace::ObserveVideoPlaybackTick(13);
	perf_trace::TraceWindowOpenBegin("main");
	perf_trace::ObserveWindowOpenPhase("main", "startup.runtime.paths_and_options", 4.25);
	perf_trace::TraceWindowOpenEnd("main", 8.5, true);
	VideoMemorySnapshot memory_snapshot;
	memory_snapshot.async.provider.cache_native_bytes = 4096;
	memory_snapshot.async.provider.cache_native_frames = 1;
	memory_snapshot.async.source_pool_bytes = 2048;
	memory_snapshot.async.source_pool_buffers = 1;
	memory_snapshot.display.displayed_packet_ref_bytes = 1024;
	memory_snapshot.display.primary_renderer_texture_bytes = 8192;
	memory_snapshot.display.primary_renderer_name = "libplacebo";
	memory_snapshot.audio.provider_name = "RAM";
	memory_snapshot.audio.storage_kind = "memory";
	memory_snapshot.audio.storage_bytes = 16384;
	memory_snapshot.audio.logical_bytes = 12288;
	memory_snapshot.audio.decoded_bytes = 8192;
	perf_trace::ObserveVideoMemorySnapshot("unit_test", memory_snapshot, true);
	perf_trace::TraceLuaDialogOpenBegin();
	perf_trace::ObserveLuaDialogPhase("build_model", 3, 2, 4.5);
	perf_trace::ObserveLuaDialogControlTypeSummary("dropdown", 3, 2, 1, 18, 18, 2.75);
	perf_trace::ObserveLuaDialogControlStepSummary("dropdown", "native_construct", 3, 2, 1, 18, 18, 1.25);
	perf_trace::TraceLuaDialogOpenEnd(3, 2, 12.5, true);
	LOG_W("perf_trace/test") << "warning event";
	perf_trace::Shutdown();

	EXPECT_FALSE(perf_trace::IsEnabled());
	EXPECT_TRUE(agi::fs::FileExists(session_dir / "trace.ndjson"));
	EXPECT_TRUE(agi::fs::FileExists(session_dir / "summary.txt"));
	EXPECT_TRUE(agi::fs::FileExists(session_dir / "manifest.txt"));

	auto const trace = ReadAll(session_dir / "trace.ndjson");
	auto const summary = ReadAll(session_dir / "summary.txt");
	auto const manifest = ReadAll(session_dir / "manifest.txt");

	EXPECT_NE(std::string::npos, trace.find("\"name\":\"seek\""));
	EXPECT_NE(std::string::npos, trace.find("\"name\":\"video_frame_request\""));
	EXPECT_NE(std::string::npos, trace.find("\"name\":\"video_frame_delivered\""));
	EXPECT_NE(std::string::npos, trace.find("\"name\":\"video_frame_dropped\""));
	EXPECT_NE(std::string::npos, trace.find("\"name\":\"audio_ui_timer_interval\""));
	EXPECT_NE(std::string::npos, trace.find("\"name\":\"audio_ui_duration\""));
	EXPECT_NE(std::string::npos, trace.find("\"phase\":\"audio_display.paint\""));
	EXPECT_NE(std::string::npos, trace.find("\"name\":\"audio_output_snapshot\""));
	EXPECT_NE(std::string::npos, trace.find("\"name\":\"video_memory_snapshot\""));
	EXPECT_NE(std::string::npos, trace.find("\"name\":\"window_open_phase_duration\""));
	EXPECT_NE(std::string::npos, trace.find("\"name\":\"lua_dialog_phase_duration\""));
	EXPECT_NE(std::string::npos, trace.find("\"name\":\"lua_dialog_control_type_duration\""));
	EXPECT_NE(std::string::npos, trace.find("\"name\":\"lua_dialog_control_step_duration\""));
	EXPECT_NE(std::string::npos, trace.find("\"name\":\"lua_dialog_open_duration\""));
	EXPECT_NE(std::string::npos, trace.find("\"kind\":\"log\""));

	EXPECT_NE(std::string::npos, summary.find("frame.request.total=1"));
	EXPECT_NE(std::string::npos, summary.find("frame.delivered.total=1"));
	EXPECT_NE(std::string::npos, summary.find("frame.dropped.total=1"));
	EXPECT_NE(std::string::npos, summary.find("window_open.success=1"));
	EXPECT_NE(std::string::npos, summary.find("window_phase.main.startup.runtime.paths_and_options.count=1"));
	EXPECT_NE(std::string::npos, summary.find("window_phase.main.startup.runtime.paths_and_options.total_ms="));
	EXPECT_NE(std::string::npos, summary.find("lua_dialog.success=1"));
	EXPECT_NE(std::string::npos, summary.find("video_memory.samples=1"));
	EXPECT_NE(std::string::npos, summary.find("provider_cache_native.max_bytes=4096"));
	EXPECT_NE(std::string::npos, summary.find("audio_storage.max_bytes=16384"));
	EXPECT_NE(std::string::npos, summary.find("audio_ui_timer_interval.requested_ms=20"));
	EXPECT_NE(std::string::npos, summary.find("audio_ui_timer_interval.jitter_target_ms="));
	EXPECT_NE(std::string::npos, summary.find("audio_ui_timer_interval.count=1"));
	EXPECT_NE(std::string::npos, summary.find("audio_ui_phase.audio_display.paint.count=1"));
	EXPECT_NE(std::string::npos, summary.find("audio_ui_phase.audio_display.paint.total_ms=3.5"));
	EXPECT_NE(std::string::npos, summary.find("audio_output.samples=1"));
	EXPECT_NE(std::string::npos, summary.find("audio_output.low_water.count=1"));
	EXPECT_NE(std::string::npos, summary.find("audio_output_backend=xaudio2"));
	EXPECT_NE(std::string::npos, summary.find("audio_provider=RAM"));
	EXPECT_NE(std::string::npos, summary.find("op.seek=1"));
	EXPECT_NE(std::string::npos, summary.find("log.warning=1"));

	EXPECT_NE(std::string::npos, manifest.find("build=test-build"));
	EXPECT_NE(std::string::npos, manifest.find("source=unit-test"));
	EXPECT_NE(std::string::npos, manifest.find("trace_selection=all"));
}

TEST(PerfTrace, SupportsNamedTraceSelection) {
	agi::Path path_helper;
	auto const session_dir = agi::fs::UniquePath(path_helper.Decode("?temp/perf_trace_selection_%%%%%%%%"));

	perf_trace::InitializeAt(session_dir, "test-build", "audio,lua-dialog");
	ASSERT_TRUE(perf_trace::IsEnabled());
	EXPECT_TRUE(perf_trace::IsCategoryEnabled(perf_trace::Category::Audio));
	EXPECT_TRUE(perf_trace::IsCategoryEnabled(perf_trace::Category::LuaDialog));
	EXPECT_FALSE(perf_trace::IsCategoryEnabled(perf_trace::Category::Video));
	EXPECT_FALSE(perf_trace::IsCategoryEnabled(perf_trace::Category::Log));

	perf_trace::ObserveFrameRequest(12, 0.5, false);
	perf_trace::TraceLuaDialogOpenBegin();
	perf_trace::TraceLuaDialogOpenEnd(1, 1, 3.5, true);
	perf_trace::AudioOutputSnapshot output_snapshot;
	output_snapshot.backend_name = "directsound2";
	output_snapshot.reason = "selection_test";
	output_snapshot.queued_ms = 20.0;
	output_snapshot.submitted_buffers = 1;
	output_snapshot.submitted_frames = 480;
	output_snapshot.submitted_bytes = 960;
	output_snapshot.submitted_ms = 10.0;
	output_snapshot.fill_duration_ms = 0.5;
	output_snapshot.starved = true;
	output_snapshot.recovered = true;
	perf_trace::ObserveAudioUiDuration("audio_display.scroll", 1.25, 128, 1, false);
	perf_trace::ObserveAudioOutputSnapshot(output_snapshot);
	LOG_W("perf_trace/test") << "warning event";
	perf_trace::Shutdown();

	auto const trace = ReadAll(session_dir / "trace.ndjson");
	auto const summary = ReadAll(session_dir / "summary.txt");
	auto const manifest = ReadAll(session_dir / "manifest.txt");

	EXPECT_EQ(std::string::npos, trace.find("\"name\":\"video_frame_request\""));
	EXPECT_EQ(std::string::npos, trace.find("\"kind\":\"log\""));
	EXPECT_NE(std::string::npos, trace.find("\"name\":\"lua_dialog_open_duration\""));
	EXPECT_NE(std::string::npos, trace.find("\"name\":\"audio_ui_duration\""));
	EXPECT_NE(std::string::npos, trace.find("\"name\":\"audio_output_snapshot\""));

	EXPECT_NE(std::string::npos, summary.find("trace.selection=audio,lua-dialog"));
	EXPECT_NE(std::string::npos, summary.find("audio_output.samples=1"));
	EXPECT_NE(std::string::npos, summary.find("audio_ui_phase.audio_display.scroll.count=1"));
	EXPECT_NE(std::string::npos, summary.find("audio_output.starved.count=1"));
	EXPECT_NE(std::string::npos, summary.find("audio_output.recovered.count=1"));
	EXPECT_EQ(std::string::npos, summary.find("frame.request.total=1"));
	EXPECT_EQ(std::string::npos, summary.find("log.warning=1"));

	EXPECT_NE(std::string::npos, manifest.find("source=audio,lua-dialog"));
	EXPECT_NE(std::string::npos, manifest.find("trace_selection=audio,lua-dialog"));
}

TEST(PerfTrace, ManifestWritesUtf8Paths) {
	agi::Path path_helper;
	auto const session_dir = agi::fs::UniquePath(path_helper.Decode(std::string("?temp/perf_trace_") + Utf8PathSegment() + "_%%%%%%%%"));

	perf_trace::InitializeAt(session_dir, "test-build", "utf8-paths");
	ASSERT_TRUE(perf_trace::IsEnabled());
	perf_trace::Shutdown();

	auto const manifest = ReadAll(session_dir / "manifest.txt");
	auto const session_dir_text = agi::fs::PathToString(session_dir);

	EXPECT_NE(std::string::npos, session_dir_text.find(Utf8PathSegment()));
	EXPECT_NE(std::string::npos, manifest.find(Utf8PathSegment()));
	EXPECT_NE(std::string::npos, manifest.find("session_dir=" + session_dir_text));
	EXPECT_NE(std::string::npos, manifest.find("trace_file=" + agi::fs::PathToString(session_dir / "trace.ndjson")));
	EXPECT_NE(std::string::npos, manifest.find("summary_file=" + agi::fs::PathToString(session_dir / "summary.txt")));
}
