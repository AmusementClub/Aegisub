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
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

#include "perf_trace.h"

#include "options.h"

#include <libaegisub/fs.h>
#include <libaegisub/log.h>
#include <libaegisub/make_unique.h>
#include <libaegisub/path.h>
#include <libaegisub/util.h>

#include <wx/utils.h>

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <map>
#include <mutex>
#include <sstream>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#undef CreateDirectory
#endif

namespace perf_trace {
namespace {

using Clock = std::chrono::steady_clock;

constexpr size_t kBufferedEntryLimit = 64;
constexpr size_t kBufferedByteLimit = 64 * 1024;
constexpr auto kBufferedFlushInterval = std::chrono::milliseconds(250);
constexpr auto kVideoMemorySampleInterval = std::chrono::milliseconds(500);
constexpr double kAudioPlaybackTargetMs = 20.0;
constexpr double kVideoPlaybackTargetMs = 10.0;

std::atomic<bool> trace_active{ false };

int64_t NowNs() {
	using namespace std::chrono;
	return duration_cast<nanoseconds>(Clock::now().time_since_epoch()).count();
}

std::string Trim(std::string value) {
	auto const begin = value.find_first_not_of(" \t\r\n");
	if (begin == std::string::npos)
		return {};
	auto const end = value.find_last_not_of(" \t\r\n");
	return value.substr(begin, end - begin + 1);
}

std::string ToLower(std::string value) {
	for (char& ch : value) {
		if (ch >= 'A' && ch <= 'Z')
			ch = static_cast<char>(ch - 'A' + 'a');
	}
	return value;
}

bool IsTruthyEnvValue(std::string const& value) {
	auto lowered = ToLower(Trim(value));
	if (lowered.empty()) return false;
	return lowered != "0" && lowered != "false" && lowered != "off" && lowered != "no";
}

std::string ReadEnvValue(char const* name) {
	if (auto* value = std::getenv(name))
		return value;
	return {};
}

std::string EscapeJson(std::string_view value) {
	std::string escaped;
	escaped.reserve(value.size() + 8);
	for (unsigned char ch : value) {
		switch (ch) {
			case '\\': escaped += "\\\\"; break;
			case '"': escaped += "\\\""; break;
			case '\b': escaped += "\\b"; break;
			case '\f': escaped += "\\f"; break;
			case '\n': escaped += "\\n"; break;
			case '\r': escaped += "\\r"; break;
			case '\t': escaped += "\\t"; break;
			default:
				if (ch < 0x20) {
					char buffer[7];
					snprintf(buffer, sizeof(buffer), "\\u%04x", ch);
					escaped += buffer;
				}
				else {
					escaped += static_cast<char>(ch);
				}
				break;
		}
	}
	return escaped;
}

template <typename T>
std::string ToString(T value) {
	std::ostringstream out;
	out.imbue(std::locale::classic());
	out << value;
	return out.str();
}

std::string ToStringDouble(double value) {
	std::ostringstream out;
	out.imbue(std::locale::classic());
	out << std::setprecision(6) << value;
	return out.str();
}

class JsonObjectBuilder {
	std::string value = "{";
	bool first = true;

	void AddKey(std::string_view key) {
		if (!first)
			value += ",";
		first = false;
		value += "\"";
		value += EscapeJson(key);
		value += "\":";
	}

public:
	void AddString(std::string_view key, std::string_view field_value) {
		AddKey(key);
		value += "\"";
		value += EscapeJson(field_value);
		value += "\"";
	}

	void AddBool(std::string_view key, bool field_value) {
		AddKey(key);
		value += field_value ? "true" : "false";
	}

	void AddInt(std::string_view key, int64_t field_value) {
		AddKey(key);
		value += ToString(field_value);
	}

	void AddDouble(std::string_view key, double field_value) {
		AddKey(key);
		value += ToStringDouble(field_value);
	}

	std::string Finish() {
		value += "}";
		return std::move(value);
	}
};

struct IntervalSummary {
	uint64_t count = 0;
	double min_ms = 0.0;
	double max_ms = 0.0;
	double total_ms = 0.0;
	double total_abs_jitter_ms = 0.0;
	bool has_last = false;
	int64_t last_ns = 0;

	void Reset() {
		has_last = false;
		last_ns = 0;
	}

	bool Observe(int64_t now_ns, double target_ms, double& observed_ms) {
		if (!has_last) {
			has_last = true;
			last_ns = now_ns;
			return false;
		}

		observed_ms = static_cast<double>(now_ns - last_ns) / 1000000.0;
		last_ns = now_ns;
		++count;
		total_ms += observed_ms;
		total_abs_jitter_ms += std::abs(observed_ms - target_ms);
		if (count == 1) {
			min_ms = observed_ms;
			max_ms = observed_ms;
		}
		else {
			min_ms = std::min(min_ms, observed_ms);
			max_ms = std::max(max_ms, observed_ms);
		}
		return true;
	}
};

struct DurationSummary {
	uint64_t count = 0;
	double min_ms = 0.0;
	double max_ms = 0.0;
	double total_ms = 0.0;

	void Observe(double duration_ms) {
		++count;
		total_ms += duration_ms;
		if (count == 1) {
			min_ms = duration_ms;
			max_ms = duration_ms;
		}
		else {
			min_ms = std::min(min_ms, duration_ms);
			max_ms = std::max(max_ms, duration_ms);
		}
	}
};

struct Summary {
	uint64_t trace_entries = 0;
	uint64_t buffered_flushes = 0;
	uint64_t immediate_flushes = 0;
	uint64_t frame_requests = 0;
	uint64_t frame_requests_immediate = 0;
	uint64_t frame_delivered = 0;
	uint64_t frame_delivered_immediate = 0;
	uint64_t frame_dropped = 0;
	uint64_t lua_dialog_success = 0;
	uint64_t lua_dialog_failure = 0;
	uint64_t video_memory_samples = 0;
	std::array<uint64_t, 5> log_counts = { 0, 0, 0, 0, 0 };
	std::map<std::string, uint64_t> op_counts;
	size_t process_working_set_max_bytes = 0;
	size_t process_private_max_bytes = 0;
	size_t provider_cache_bgra_max_bytes = 0;
	size_t provider_cache_native_max_bytes = 0;
	size_t async_source_pool_max_bytes = 0;
	size_t async_composited_pool_max_bytes = 0;
	size_t async_overlay_pool_max_bytes = 0;
	size_t async_compatibility_overlay_pool_max_bytes = 0;
	size_t display_pending_packet_ref_max_bytes = 0;
	size_t display_displayed_packet_ref_max_bytes = 0;
	size_t renderer_primary_texture_max_bytes = 0;
	size_t renderer_secondary_texture_max_bytes = 0;
	size_t audio_storage_max_bytes = 0;
	size_t audio_logical_max_bytes = 0;
	size_t audio_decoded_max_bytes = 0;
	std::string audio_provider_name;
	std::string audio_storage_kind;
	IntervalSummary audio_playback_interval;
	IntervalSummary video_playback_tick_interval;
	DurationSummary lua_dialog_duration;
};

class TraceLogEmitter final : public agi::log::Emitter {
public:
	void log(agi::log::SinkMessage const& sm) override;
};

struct Session {
	std::mutex mutex;
	bool enabled = false;
	bool closing = false;
	bool has_pending_lua_dialog_open = false;
	int64_t pending_lua_dialog_open_started_ns = 0;
	int64_t last_video_memory_sample_ns = 0;
	agi::fs::path directory;
	std::ofstream trace_stream;
	std::vector<std::string> buffered_lines;
	size_t buffered_bytes = 0;
	Clock::time_point last_flush = Clock::now();
	std::string session_id;
	std::string build_label;
	std::string source_tag;
	std::string started_local;
	TraceLogEmitter* log_emitter = nullptr;
	Summary summary;
};

Session& GetSession() {
	static Session session;
	return session;
}

char const* SeverityName(agi::log::Severity severity) {
	switch (severity) {
		case agi::log::Exception: return "exception";
		case agi::log::Assert: return "assert";
		case agi::log::Warning: return "warning";
		case agi::log::Info: return "info";
		case agi::log::Debug: return "debug";
	}
	return "unknown";
}

void FlushLocked(Session& session, bool immediate) {
	if (!session.trace_stream.is_open() || session.buffered_lines.empty())
		return;

	for (auto const& line : session.buffered_lines)
		session.trace_stream << line << '\n';
	session.trace_stream.flush();
	session.buffered_lines.clear();
	session.buffered_bytes = 0;
	session.last_flush = Clock::now();
	if (immediate)
		++session.summary.immediate_flushes;
	else
		++session.summary.buffered_flushes;
}

void AppendEntryLocked(Session& session, char const* kind, std::string const& name, std::string payload, bool immediate, int64_t timestamp_ns) {
	if (!session.trace_stream.is_open())
		return;

	std::string line;
	line.reserve(session.session_id.size() + name.size() + payload.size() + 96);
	line += "{\"session\":\"";
	line += EscapeJson(session.session_id);
	line += "\",\"t_monotonic_ns\":";
	line += ToString(timestamp_ns);
	line += ",\"kind\":\"";
	line += kind;
	line += "\",\"name\":\"";
	line += EscapeJson(name);
	line += "\",\"payload\":";
	line += payload;
	line += "}";

	++session.summary.trace_entries;
	session.buffered_bytes += line.size() + 1;
	session.buffered_lines.emplace_back(std::move(line));

	bool const should_flush =
		immediate
		|| session.buffered_lines.size() >= kBufferedEntryLimit
		|| session.buffered_bytes >= kBufferedByteLimit
		|| Clock::now() - session.last_flush >= kBufferedFlushInterval;
	if (should_flush)
		FlushLocked(session, immediate);
}

template <typename PayloadBuilder>
void RecordEntry(char const* kind, std::string const& name, bool immediate, PayloadBuilder&& fill_payload, int64_t timestamp_ns = NowNs()) {
	if (!trace_active.load(std::memory_order_relaxed))
		return;

	auto& session = GetSession();
	std::lock_guard<std::mutex> lock(session.mutex);
	if (!session.enabled || session.closing)
		return;

	if (kind[0] == 'o')
		++session.summary.op_counts[name];

	JsonObjectBuilder payload;
	fill_payload(payload);
	AppendEntryLocked(session, kind, name, payload.Finish(), immediate, timestamp_ns);
}

void WriteManifest(Session const& session) {
	std::ofstream out(session.directory / "manifest.txt", std::ios::out | std::ios::trunc);
	if (!out.is_open())
		return;

	out.imbue(std::locale::classic());
	out << "session=" << session.session_id << "\n";
	out << "build=" << session.build_label << "\n";
	out << "source=" << session.source_tag << "\n";
	out << "started_local=" << session.started_local << "\n";
	out << "pid=" << wxGetProcessId() << "\n";
	out << "platform=" << wxGetOsDescription().ToStdString() << "\n";
	out << "cwd=" << std::filesystem::current_path().string() << "\n";
	out << "session_dir=" << session.directory.string() << "\n";
	out << "trace_file=" << (session.directory / "trace.ndjson").string() << "\n";
	out << "summary_file=" << (session.directory / "summary.txt").string() << "\n";
	out.flush();
}

void WriteSummaryLocked(Session const& session) {
	std::ofstream out(session.directory / "summary.txt", std::ios::out | std::ios::trunc);
	if (!out.is_open())
		return;

	auto write_double = [&out](char const* key, double value) {
		out << key << "=" << ToStringDouble(value) << "\n";
	};
	auto write_int = [&out](char const* key, uint64_t value) {
		out << key << "=" << value << "\n";
	};
	auto write_mean = [&write_double](char const* key, double total, uint64_t count) {
		write_double(key, count ? total / count : 0.0);
	};

	out.imbue(std::locale::classic());
	out << "session=" << session.session_id << "\n";
	out << "build=" << session.build_label << "\n";
	write_int("trace.entries", session.summary.trace_entries);
	write_int("trace.flushes.buffered", session.summary.buffered_flushes);
	write_int("trace.flushes.immediate", session.summary.immediate_flushes);
	write_int("frame.request.total", session.summary.frame_requests);
	write_int("frame.request.immediate", session.summary.frame_requests_immediate);
	write_int("frame.delivered.total", session.summary.frame_delivered);
	write_int("frame.delivered.immediate", session.summary.frame_delivered_immediate);
	write_int("frame.dropped.total", session.summary.frame_dropped);
	write_int("lua_dialog.success", session.summary.lua_dialog_success);
	write_int("lua_dialog.failure", session.summary.lua_dialog_failure);
	write_int("video_memory.samples", session.summary.video_memory_samples);
	write_int("log.exception", session.summary.log_counts[agi::log::Exception]);
	write_int("log.assert", session.summary.log_counts[agi::log::Assert]);
	write_int("log.warning", session.summary.log_counts[agi::log::Warning]);
	write_int("log.info", session.summary.log_counts[agi::log::Info]);
	write_int("log.debug", session.summary.log_counts[agi::log::Debug]);

	write_int("audio_playback_interval.count", session.summary.audio_playback_interval.count);
	write_double("audio_playback_interval.min_ms", session.summary.audio_playback_interval.min_ms);
	write_double("audio_playback_interval.max_ms", session.summary.audio_playback_interval.max_ms);
	write_mean("audio_playback_interval.mean_ms", session.summary.audio_playback_interval.total_ms, session.summary.audio_playback_interval.count);
	write_mean("audio_playback_interval.mean_abs_jitter_ms", session.summary.audio_playback_interval.total_abs_jitter_ms, session.summary.audio_playback_interval.count);

	write_int("video_playback_tick_interval.count", session.summary.video_playback_tick_interval.count);
	write_double("video_playback_tick_interval.min_ms", session.summary.video_playback_tick_interval.min_ms);
	write_double("video_playback_tick_interval.max_ms", session.summary.video_playback_tick_interval.max_ms);
	write_mean("video_playback_tick_interval.mean_ms", session.summary.video_playback_tick_interval.total_ms, session.summary.video_playback_tick_interval.count);
	write_mean("video_playback_tick_interval.mean_abs_jitter_ms", session.summary.video_playback_tick_interval.total_abs_jitter_ms, session.summary.video_playback_tick_interval.count);

	write_int("lua_dialog_duration.count", session.summary.lua_dialog_duration.count);
	write_double("lua_dialog_duration.min_ms", session.summary.lua_dialog_duration.min_ms);
	write_double("lua_dialog_duration.max_ms", session.summary.lua_dialog_duration.max_ms);
	write_mean("lua_dialog_duration.mean_ms", session.summary.lua_dialog_duration.total_ms, session.summary.lua_dialog_duration.count);
	write_int("process_working_set.max_bytes", session.summary.process_working_set_max_bytes);
	write_int("process_private.max_bytes", session.summary.process_private_max_bytes);
	write_int("provider_cache_bgra.max_bytes", session.summary.provider_cache_bgra_max_bytes);
	write_int("provider_cache_native.max_bytes", session.summary.provider_cache_native_max_bytes);
	write_int("async_source_pool.max_bytes", session.summary.async_source_pool_max_bytes);
	write_int("async_composited_pool.max_bytes", session.summary.async_composited_pool_max_bytes);
	write_int("async_overlay_pool.max_bytes", session.summary.async_overlay_pool_max_bytes);
	write_int("async_compatibility_overlay_pool.max_bytes", session.summary.async_compatibility_overlay_pool_max_bytes);
	write_int("display_pending_packet_ref.max_bytes", session.summary.display_pending_packet_ref_max_bytes);
	write_int("display_displayed_packet_ref.max_bytes", session.summary.display_displayed_packet_ref_max_bytes);
	write_int("renderer_primary_texture.max_bytes", session.summary.renderer_primary_texture_max_bytes);
	write_int("renderer_secondary_texture.max_bytes", session.summary.renderer_secondary_texture_max_bytes);
	write_int("audio_storage.max_bytes", session.summary.audio_storage_max_bytes);
	write_int("audio_logical.max_bytes", session.summary.audio_logical_max_bytes);
	write_int("audio_decoded.max_bytes", session.summary.audio_decoded_max_bytes);
	out << "audio_provider=" << session.summary.audio_provider_name << "\n";
	out << "audio_storage_kind=" << session.summary.audio_storage_kind << "\n";

	for (auto const& [name, count] : session.summary.op_counts)
		out << "op." << name << "=" << count << "\n";

	out.flush();
}

void TraceLogEntry(agi::log::SinkMessage const& sm) {
	if (!trace_active.load(std::memory_order_relaxed))
		return;

	auto& session = GetSession();
	std::lock_guard<std::mutex> lock(session.mutex);
	if (!session.enabled || session.closing)
		return;

	++session.summary.log_counts[sm.severity];

	JsonObjectBuilder payload;
	payload.AddString("severity", SeverityName(sm.severity));
	payload.AddString("message", sm.message);
	payload.AddString("func", sm.func ? sm.func : "");
	payload.AddInt("line", sm.line);
	AppendEntryLocked(
		session,
		"log",
		sm.section ? sm.section : "log",
		payload.Finish(),
		sm.severity <= agi::log::Warning,
		sm.time);
}

void TraceLogEmitter::log(agi::log::SinkMessage const& sm) {
	TraceLogEntry(sm);
}

} // namespace

bool IsEnabled() {
	return trace_active.load(std::memory_order_relaxed);
}

bool ShouldSampleVideoMemory(bool force) {
	if (!trace_active.load(std::memory_order_relaxed))
		return false;

	auto const timestamp_ns = NowNs();
	auto& session = GetSession();
	std::lock_guard<std::mutex> lock(session.mutex);
	if (!session.enabled || session.closing)
		return false;
	if (force)
		return true;
	return timestamp_ns - session.last_video_memory_sample_ns
		>= std::chrono::duration_cast<std::chrono::nanoseconds>(kVideoMemorySampleInterval).count();
}

agi::fs::path GetSessionDirectory() {
	auto& session = GetSession();
	std::lock_guard<std::mutex> lock(session.mutex);
	return session.directory;
}

void Initialize(std::string const& build_label) {
	auto const env_value = ReadEnvValue("AEGISUB_PERF_TRACE");
	if (!IsTruthyEnvValue(env_value) || !config::path)
		return;

	auto const root = config::path->Decode("?user/perf-sessions");
	auto const session_name = agi::util::strftime("%Y-%m-%d-%H-%M-%S") + "-" + ToString(static_cast<long long>(wxGetProcessId())) + "-%%%%%%%%";
	InitializeAt(agi::fs::UniquePath(root / session_name), build_label, env_value);
}

void InitializeAt(agi::fs::path const& session_dir, std::string const& build_label, std::string const& source_tag) {
	Shutdown();

	auto& session = GetSession();
	try {
		agi::fs::CreateDirectory(session_dir);
		std::ofstream trace_stream(session_dir / "trace.ndjson", std::ios::out | std::ios::trunc);
		if (!trace_stream.is_open())
			return;

		TraceLogEmitter* emitter_ptr = nullptr;
		if (agi::log::log) {
			auto emitter = agi::make_unique<TraceLogEmitter>();
			emitter_ptr = emitter.get();
			agi::log::log->Subscribe(std::move(emitter));
		}

		std::lock_guard<std::mutex> lock(session.mutex);
		session.enabled = true;
		session.closing = false;
		session.directory = session_dir;
		session.trace_stream = std::move(trace_stream);
		session.buffered_lines.clear();
		session.buffered_bytes = 0;
		session.last_flush = Clock::now();
		session.has_pending_lua_dialog_open = false;
		session.pending_lua_dialog_open_started_ns = 0;
		session.last_video_memory_sample_ns = 0;
		session.session_id = session_dir.filename().string();
		session.build_label = build_label;
		session.source_tag = source_tag.empty() ? "manual" : source_tag;
		session.started_local = agi::util::strftime("%Y-%m-%d %H:%M:%S");
		session.log_emitter = emitter_ptr;
		session.summary = Summary{};

		WriteManifest(session);
		trace_active.store(true, std::memory_order_relaxed);
	}
	catch (...) {
	}
}

void Shutdown() {
	auto& session = GetSession();
	if (!session.enabled && !trace_active.load(std::memory_order_relaxed))
		return;

	TraceLogEmitter* emitter = nullptr;
	{
		std::lock_guard<std::mutex> lock(session.mutex);
		emitter = session.log_emitter;
	}

	if (emitter && agi::log::log)
		agi::log::log->Unsubscribe(emitter);

	{
		std::lock_guard<std::mutex> lock(session.mutex);
		if (!session.enabled)
			return;

		session.closing = true;
		FlushLocked(session, true);
		WriteSummaryLocked(session);
		session.trace_stream.close();
		session.log_emitter = nullptr;
		session.enabled = false;
	}

	trace_active.store(false, std::memory_order_relaxed);
}

void ResetAudioPlaybackInterval() {
	if (!trace_active.load(std::memory_order_relaxed))
		return;
	auto& session = GetSession();
	std::lock_guard<std::mutex> lock(session.mutex);
	if (!session.enabled || session.closing)
		return;
	session.summary.audio_playback_interval.Reset();
}

void ResetVideoPlaybackInterval() {
	if (!trace_active.load(std::memory_order_relaxed))
		return;
	auto& session = GetSession();
	std::lock_guard<std::mutex> lock(session.mutex);
	if (!session.enabled || session.closing)
		return;
	session.summary.video_playback_tick_interval.Reset();
}

void TraceVideoOpen(agi::fs::path const& path, int width, int height, int frame_count, bool has_audio, std::string const& decoder_name, double duration_ms) {
	RecordEntry("op", "video_open", true, [&](JsonObjectBuilder& payload) {
		payload.AddString("path", agi::fs::PathToString(path));
		payload.AddInt("width", width);
		payload.AddInt("height", height);
		payload.AddInt("frame_count", frame_count);
		payload.AddBool("has_audio", has_audio);
		payload.AddString("decoder", decoder_name);
		payload.AddDouble("duration_ms", duration_ms);
	});
}

void TracePlayStart(int frame, int start_ms) {
	RecordEntry("op", "play_start", true, [&](JsonObjectBuilder& payload) {
		payload.AddInt("frame", frame);
		payload.AddInt("start_ms", start_ms);
	});
}

void TracePlayStop(int frame) {
	RecordEntry("op", "play_stop", true, [&](JsonObjectBuilder& payload) {
		payload.AddInt("frame", frame);
	});
}

void TraceSeek(int frame, bool was_playing) {
	RecordEntry("op", "seek", true, [&](JsonObjectBuilder& payload) {
		payload.AddInt("frame", frame);
		payload.AddBool("was_playing", was_playing);
	});
}

void ObserveFrameRequest(int frame, double time, bool immediate) {
	if (!trace_active.load(std::memory_order_relaxed))
		return;

	auto& session = GetSession();
	std::lock_guard<std::mutex> lock(session.mutex);
	if (!session.enabled || session.closing)
		return;

	++session.summary.frame_requests;
	if (immediate)
		++session.summary.frame_requests_immediate;

	JsonObjectBuilder payload;
	payload.AddInt("frame", frame);
	payload.AddDouble("time_ms", time);
	payload.AddBool("immediate", immediate);
	AppendEntryLocked(session, "metric", "video_frame_request", payload.Finish(), false, NowNs());
}

void ObserveFrameResult(int frame, double time, bool delivered, bool immediate) {
	if (!trace_active.load(std::memory_order_relaxed))
		return;

	auto& session = GetSession();
	std::lock_guard<std::mutex> lock(session.mutex);
	if (!session.enabled || session.closing)
		return;

	if (delivered) {
		++session.summary.frame_delivered;
		if (immediate)
			++session.summary.frame_delivered_immediate;
	}
	else {
		++session.summary.frame_dropped;
	}

	JsonObjectBuilder payload;
	payload.AddInt("frame", frame);
	payload.AddDouble("time_ms", time);
	payload.AddBool("immediate", immediate);
	AppendEntryLocked(session, "metric", delivered ? "video_frame_delivered" : "video_frame_dropped", payload.Finish(), false, NowNs());
}

void ObserveAudioPlaybackPosition(int ms) {
	if (!trace_active.load(std::memory_order_relaxed))
		return;

	auto const timestamp_ns = NowNs();
	auto& session = GetSession();
	std::lock_guard<std::mutex> lock(session.mutex);
	if (!session.enabled || session.closing)
		return;

	double interval_ms = 0.0;
	if (!session.summary.audio_playback_interval.Observe(timestamp_ns, kAudioPlaybackTargetMs, interval_ms))
		return;

	JsonObjectBuilder payload;
	payload.AddInt("position_ms", ms);
	payload.AddDouble("delta_ms", interval_ms);
	AppendEntryLocked(session, "metric", "audio_playback_interval", payload.Finish(), false, timestamp_ns);
}

void ObserveVideoPlaybackTick(int frame) {
	if (!trace_active.load(std::memory_order_relaxed))
		return;

	auto const timestamp_ns = NowNs();
	auto& session = GetSession();
	std::lock_guard<std::mutex> lock(session.mutex);
	if (!session.enabled || session.closing)
		return;

	double interval_ms = 0.0;
	if (!session.summary.video_playback_tick_interval.Observe(timestamp_ns, kVideoPlaybackTargetMs, interval_ms))
		return;

	JsonObjectBuilder payload;
	payload.AddInt("frame", frame);
	payload.AddDouble("delta_ms", interval_ms);
	AppendEntryLocked(session, "metric", "video_playback_tick_interval", payload.Finish(), false, timestamp_ns);
}

void TraceLuaDialogOpenBegin() {
	auto const timestamp_ns = NowNs();
	if (!trace_active.load(std::memory_order_relaxed))
		return;

	auto& session = GetSession();
	std::lock_guard<std::mutex> lock(session.mutex);
	if (!session.enabled || session.closing)
		return;

	session.has_pending_lua_dialog_open = true;
	session.pending_lua_dialog_open_started_ns = timestamp_ns;
	++session.summary.op_counts["lua_dialog_open_begin"];

	JsonObjectBuilder payload;
	AppendEntryLocked(session, "op", "lua_dialog_open_begin", payload.Finish(), true, timestamp_ns);
}

void TraceLuaDialogOpenEnd(int control_count, int button_count, double duration_ms, bool succeeded) {
	if (!trace_active.load(std::memory_order_relaxed))
		return;

	auto const timestamp_ns = NowNs();
	auto& session = GetSession();
	std::lock_guard<std::mutex> lock(session.mutex);
	if (!session.enabled || session.closing)
		return;

	if (!session.has_pending_lua_dialog_open && duration_ms < 0.0)
		return;

	if (duration_ms < 0.0 && session.has_pending_lua_dialog_open)
		duration_ms = static_cast<double>(timestamp_ns - session.pending_lua_dialog_open_started_ns) / 1000000.0;

	session.has_pending_lua_dialog_open = false;
	session.pending_lua_dialog_open_started_ns = 0;

	if (succeeded)
		++session.summary.lua_dialog_success;
	else
		++session.summary.lua_dialog_failure;
	session.summary.lua_dialog_duration.Observe(duration_ms);
	++session.summary.op_counts["lua_dialog_open_end"];

	JsonObjectBuilder op_payload;
	op_payload.AddInt("control_count", control_count);
	op_payload.AddInt("button_count", button_count);
	op_payload.AddDouble("duration_ms", duration_ms);
	op_payload.AddBool("succeeded", succeeded);
	AppendEntryLocked(session, "op", "lua_dialog_open_end", op_payload.Finish(), true, timestamp_ns);

	JsonObjectBuilder metric_payload;
	metric_payload.AddInt("control_count", control_count);
	metric_payload.AddInt("button_count", button_count);
	metric_payload.AddDouble("duration_ms", duration_ms);
	metric_payload.AddBool("succeeded", succeeded);
	AppendEntryLocked(session, "metric", "lua_dialog_open_duration", metric_payload.Finish(), false, timestamp_ns);
}

void ObserveVideoMemorySnapshot(char const* reason, VideoMemorySnapshot const& snapshot_in, bool force) {
	if (!trace_active.load(std::memory_order_relaxed))
		return;

	auto snapshot = snapshot_in;
	auto const timestamp_ns = NowNs();
	auto& session = GetSession();
	std::lock_guard<std::mutex> lock(session.mutex);
	if (!session.enabled || session.closing)
		return;

	auto const sample_interval_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(kVideoMemorySampleInterval).count();
	if (!force && timestamp_ns - session.last_video_memory_sample_ns < sample_interval_ns)
		return;
	session.last_video_memory_sample_ns = timestamp_ns;

#ifdef _WIN32
	PROCESS_MEMORY_COUNTERS_EX counters = { };
	if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) {
		snapshot.process_working_set_bytes = static_cast<size_t>(counters.WorkingSetSize);
		snapshot.process_private_bytes = static_cast<size_t>(counters.PrivateUsage);
	}
#endif

	++session.summary.video_memory_samples;
	session.summary.process_working_set_max_bytes = std::max(session.summary.process_working_set_max_bytes, snapshot.process_working_set_bytes);
	session.summary.process_private_max_bytes = std::max(session.summary.process_private_max_bytes, snapshot.process_private_bytes);
	session.summary.provider_cache_bgra_max_bytes = std::max(session.summary.provider_cache_bgra_max_bytes, snapshot.async.provider.cache_bgra_bytes);
	session.summary.provider_cache_native_max_bytes = std::max(session.summary.provider_cache_native_max_bytes, snapshot.async.provider.cache_native_bytes);
	session.summary.async_source_pool_max_bytes = std::max(session.summary.async_source_pool_max_bytes, snapshot.async.source_pool_bytes);
	session.summary.async_composited_pool_max_bytes = std::max(session.summary.async_composited_pool_max_bytes, snapshot.async.composited_pool_bytes);
	session.summary.async_overlay_pool_max_bytes = std::max(session.summary.async_overlay_pool_max_bytes, snapshot.async.subtitle_overlay_pool_bytes);
	session.summary.async_compatibility_overlay_pool_max_bytes = std::max(session.summary.async_compatibility_overlay_pool_max_bytes, snapshot.async.compatibility_overlay_pool_bytes);
	session.summary.display_pending_packet_ref_max_bytes = std::max(session.summary.display_pending_packet_ref_max_bytes, snapshot.display.pending_packet_ref_bytes);
	session.summary.display_displayed_packet_ref_max_bytes = std::max(session.summary.display_displayed_packet_ref_max_bytes, snapshot.display.displayed_packet_ref_bytes);
	session.summary.renderer_primary_texture_max_bytes = std::max(session.summary.renderer_primary_texture_max_bytes, snapshot.display.primary_renderer_texture_bytes);
	session.summary.renderer_secondary_texture_max_bytes = std::max(session.summary.renderer_secondary_texture_max_bytes, snapshot.display.secondary_renderer_texture_bytes);
	session.summary.audio_storage_max_bytes = std::max(session.summary.audio_storage_max_bytes, snapshot.audio.storage_bytes);
	session.summary.audio_logical_max_bytes = std::max(session.summary.audio_logical_max_bytes, snapshot.audio.logical_bytes);
	session.summary.audio_decoded_max_bytes = std::max(session.summary.audio_decoded_max_bytes, snapshot.audio.decoded_bytes);
	if (!snapshot.audio.provider_name.empty())
		session.summary.audio_provider_name = snapshot.audio.provider_name;
	if (!snapshot.audio.storage_kind.empty())
		session.summary.audio_storage_kind = snapshot.audio.storage_kind;

	JsonObjectBuilder payload;
	payload.AddString("reason", reason ? reason : "video_memory");
	payload.AddInt("process_working_set_bytes", static_cast<int64_t>(snapshot.process_working_set_bytes));
	payload.AddInt("process_private_bytes", static_cast<int64_t>(snapshot.process_private_bytes));
	payload.AddInt("provider_cache_total_bytes", static_cast<int64_t>(snapshot.async.provider.cache_total_bytes));
	payload.AddInt("provider_cache_bgra_bytes", static_cast<int64_t>(snapshot.async.provider.cache_bgra_bytes));
	payload.AddInt("provider_cache_native_bytes", static_cast<int64_t>(snapshot.async.provider.cache_native_bytes));
	payload.AddInt("provider_cache_bgra_frames", snapshot.async.provider.cache_bgra_frames);
	payload.AddInt("provider_cache_native_frames", snapshot.async.provider.cache_native_frames);
	payload.AddInt("async_source_pool_bytes", static_cast<int64_t>(snapshot.async.source_pool_bytes));
	payload.AddInt("async_source_pool_buffers", snapshot.async.source_pool_buffers);
	payload.AddInt("async_composited_pool_bytes", static_cast<int64_t>(snapshot.async.composited_pool_bytes));
	payload.AddInt("async_composited_pool_buffers", snapshot.async.composited_pool_buffers);
	payload.AddInt("async_subtitle_overlay_pool_bytes", static_cast<int64_t>(snapshot.async.subtitle_overlay_pool_bytes));
	payload.AddInt("async_subtitle_overlay_pool_buffers", snapshot.async.subtitle_overlay_pool_buffers);
	payload.AddInt("async_compatibility_overlay_pool_bytes", static_cast<int64_t>(snapshot.async.compatibility_overlay_pool_bytes));
	payload.AddInt("async_compatibility_overlay_pool_buffers", snapshot.async.compatibility_overlay_pool_buffers);
	payload.AddString("source_mode", SourceFrameOutputModeName(snapshot.async.selected_source_mode));
	payload.AddString("decoder", snapshot.async.decoder_name);
	payload.AddInt("display_pending_packet_ref_bytes", static_cast<int64_t>(snapshot.display.pending_packet_ref_bytes));
	payload.AddInt("display_displayed_packet_ref_bytes", static_cast<int64_t>(snapshot.display.displayed_packet_ref_bytes));
	payload.AddInt("renderer_primary_texture_bytes", static_cast<int64_t>(snapshot.display.primary_renderer_texture_bytes));
	payload.AddString("renderer_primary", snapshot.display.primary_renderer_name);
	payload.AddInt("renderer_secondary_texture_bytes", static_cast<int64_t>(snapshot.display.secondary_renderer_texture_bytes));
	payload.AddString("renderer_secondary", snapshot.display.secondary_renderer_name);
	payload.AddString("audio_provider", snapshot.audio.provider_name);
	payload.AddString("audio_storage_kind", snapshot.audio.storage_kind);
	payload.AddInt("audio_storage_bytes", static_cast<int64_t>(snapshot.audio.storage_bytes));
	payload.AddInt("audio_logical_bytes", static_cast<int64_t>(snapshot.audio.logical_bytes));
	payload.AddInt("audio_decoded_bytes", static_cast<int64_t>(snapshot.audio.decoded_bytes));
	payload.AddInt("audio_num_samples", snapshot.audio.num_samples);
	payload.AddInt("audio_decoded_samples", snapshot.audio.decoded_samples);
	payload.AddInt("audio_sample_rate", snapshot.audio.sample_rate);
	payload.AddInt("audio_bytes_per_sample", snapshot.audio.bytes_per_sample);
	payload.AddInt("audio_channels", snapshot.audio.channels);
	payload.AddBool("audio_float_samples", snapshot.audio.float_samples);
	AppendEntryLocked(session, "metric", "video_memory_snapshot", payload.Finish(), force, timestamp_ns);
}

} // namespace perf_trace
