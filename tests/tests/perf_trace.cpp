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
	perf_trace::ResetAudioPlaybackInterval();
	perf_trace::ObserveAudioPlaybackPosition(100);
	perf_trace::ObserveAudioPlaybackPosition(120);
	perf_trace::ResetVideoPlaybackInterval();
	perf_trace::ObserveVideoPlaybackTick(12);
	perf_trace::ObserveVideoPlaybackTick(13);
	VideoMemorySnapshot memory_snapshot;
	memory_snapshot.async.provider.cache_native_bytes = 4096;
	memory_snapshot.async.provider.cache_native_frames = 1;
	memory_snapshot.async.source_pool_bytes = 2048;
	memory_snapshot.async.source_pool_buffers = 1;
	memory_snapshot.display.displayed_packet_ref_bytes = 1024;
	memory_snapshot.display.primary_renderer_texture_bytes = 8192;
	memory_snapshot.display.primary_renderer_name = "libplacebo";
	perf_trace::ObserveVideoMemorySnapshot("unit_test", memory_snapshot, true);
	perf_trace::TraceLuaDialogOpenBegin();
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
	EXPECT_NE(std::string::npos, trace.find("\"name\":\"video_memory_snapshot\""));
	EXPECT_NE(std::string::npos, trace.find("\"name\":\"lua_dialog_open_duration\""));
	EXPECT_NE(std::string::npos, trace.find("\"kind\":\"log\""));

	EXPECT_NE(std::string::npos, summary.find("frame.request.total=1"));
	EXPECT_NE(std::string::npos, summary.find("frame.delivered.total=1"));
	EXPECT_NE(std::string::npos, summary.find("frame.dropped.total=1"));
	EXPECT_NE(std::string::npos, summary.find("lua_dialog.success=1"));
	EXPECT_NE(std::string::npos, summary.find("video_memory.samples=1"));
	EXPECT_NE(std::string::npos, summary.find("provider_cache_native.max_bytes=4096"));
	EXPECT_NE(std::string::npos, summary.find("op.seek=1"));
	EXPECT_NE(std::string::npos, summary.find("log.warning=1"));

	EXPECT_NE(std::string::npos, manifest.find("build=test-build"));
	EXPECT_NE(std::string::npos, manifest.find("source=unit-test"));
}
