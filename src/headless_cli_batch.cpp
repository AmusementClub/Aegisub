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

#include "headless_cli_execute.h"
#include "headless_cli_internal.h"

#include <libaegisub/dispatch.h>
#include <libaegisub/exception.h>
#include <libaegisub/fs.h>

#include <fstream>
#include <memory>
#include <utility>

namespace headless_cli {
namespace {

void WriteBatchResultsCsv(agi::fs::path const& output_dir, std::vector<detail::BatchCaseResult> const& results) {
	std::ofstream out(output_dir / "results.csv", std::ios::out | std::ios::trunc);
	out << "index,passed,exit_code,video_path,audio_path,performed_seeks,seek_samples,audio_timer_samples,seek_max_abs_delta_ms,seek_mean_abs_delta_ms,actual_video_decoder,actual_audio_provider,trace_dir,message\n";
	for (auto const& result : results) {
		out
			<< result.index + 1 << ','
			<< (result.probe_result.passed ? "true" : "false") << ','
			<< result.probe_result.exit_code << ','
			<< detail::CsvEscape(detail::ToGenericString(result.spec.video_path)) << ','
			<< detail::CsvEscape(detail::ToGenericString(result.spec.audio_path.value_or(result.spec.video_path))) << ','
			<< result.probe_result.performed_seeks << ','
			<< result.probe_result.seek_samples << ','
			<< result.probe_result.audio_timer_samples << ','
			<< result.probe_result.max_abs_delta_ms << ','
			<< result.probe_result.mean_abs_delta_ms << ','
			<< detail::CsvEscape(result.probe_result.actual_video_decoder) << ','
			<< detail::CsvEscape(result.probe_result.actual_audio_provider) << ','
			<< detail::CsvEscape(detail::ToGenericString(result.probe_result.trace_dir)) << ','
			<< detail::CsvEscape(result.probe_result.message)
			<< "\n";
	}
}

void WriteBatchSummary(agi::fs::path const& output_dir, agi::fs::path const& list_file, BatchPlaybackProbeResult const& result) {
	std::ofstream out(output_dir / "summary.txt", std::ios::out | std::ios::trunc);
	out << "command=batch playback-probe\n";
	out << "list_file=" << detail::ToGenericString(list_file) << "\n";
	out << "output_dir=" << detail::ToGenericString(result.output_dir) << "\n";
	out << "total_cases=" << result.total_cases << "\n";
	out << "passed_cases=" << result.passed_cases << "\n";
	out << "failed_cases=" << result.failed_cases << "\n";
	out << "result=" << (result.exit_code == 0 ? "PASS" : "FAIL") << "\n";
	out << "message=" << result.message << "\n";
}

void WriteBatchManifestJson(agi::fs::path const& output_dir, agi::fs::path const& list_file, std::vector<detail::BatchCaseResult> const& results, BatchPlaybackProbeResult const& result) {
	std::ofstream out(output_dir / "manifest.json", std::ios::out | std::ios::trunc);
	out << "{\n";
	out << "  \"command\": \"batch playback-probe\",\n";
	out << "  \"list_file\": \"" << detail::JsonEscape(detail::ToGenericString(list_file)) << "\",\n";
	out << "  \"output_dir\": \"" << detail::JsonEscape(detail::ToGenericString(result.output_dir)) << "\",\n";
	out << "  \"total_cases\": " << result.total_cases << ",\n";
	out << "  \"passed_cases\": " << result.passed_cases << ",\n";
	out << "  \"failed_cases\": " << result.failed_cases << ",\n";
	out << "  \"result\": \"" << (result.exit_code == 0 ? "PASS" : "FAIL") << "\",\n";
	out << "  \"message\": \"" << detail::JsonEscape(result.message) << "\",\n";
	out << "  \"cases\": [\n";
	out << std::boolalpha;
	for (size_t i = 0; i < results.size(); ++i) {
		auto const& item = results[i];
		if (i)
			out << ",\n";
		out << "    {\n";
		out << "      \"index\": " << item.index + 1 << ",\n";
		out << "      \"video_path\": \"" << detail::JsonEscape(detail::ToGenericString(item.spec.video_path)) << "\",\n";
		out << "      \"audio_path\": \"" << detail::JsonEscape(detail::ToGenericString(item.spec.audio_path.value_or(item.spec.video_path))) << "\",\n";
		out << "      \"passed\": " << item.probe_result.passed << ",\n";
		out << "      \"exit_code\": " << item.probe_result.exit_code << ",\n";
		out << "      \"performed_seeks\": " << item.probe_result.performed_seeks << ",\n";
		out << "      \"seek_max_abs_delta_ms\": " << item.probe_result.max_abs_delta_ms << ",\n";
		out << "      \"seek_mean_abs_delta_ms\": " << item.probe_result.mean_abs_delta_ms << ",\n";
		out << "      \"actual_video_decoder\": \"" << detail::JsonEscape(item.probe_result.actual_video_decoder) << "\",\n";
		out << "      \"actual_audio_provider\": \"" << detail::JsonEscape(item.probe_result.actual_audio_provider) << "\",\n";
		out << "      \"trace_dir\": \"" << detail::JsonEscape(detail::ToGenericString(item.probe_result.trace_dir)) << "\",\n";
		out << "      \"message\": \"" << detail::JsonEscape(item.probe_result.message) << "\"\n";
		out << "    }";
	}
	out << "\n  ]\n";
	out << "}\n";
}

class BatchPlaybackProbeRunner final : public std::enable_shared_from_this<BatchPlaybackProbeRunner> {
	BatchPlaybackProbeRequest request;
	std::function<void(BatchPlaybackProbeResult)> on_done;
	std::vector<detail::BatchCaseSpec> cases;
	std::vector<detail::BatchCaseResult> results;
	size_t next_index = 0;
	bool finished = false;

	void ScheduleRunNext() {
		auto self = shared_from_this();
		agi::dispatch::Main().Async([self] {
			if (!self->finished)
				self->RunNext();
		});
	}

	void RecordCaseFailure(size_t index, detail::BatchCaseSpec spec, agi::fs::path trace_dir, std::string message) {
		headless_playback_probe::PlaybackProbeResult probe_result;
		probe_result.exit_code = 70;
		probe_result.trace_dir = std::move(trace_dir);
		probe_result.message = std::move(message);
		results.push_back(detail::BatchCaseResult{
			index,
			std::move(spec),
			std::move(probe_result),
		});
		ScheduleRunNext();
	}

	void Finish(BatchPlaybackProbeResult result) {
		if (finished)
			return;
		finished = true;
		result.output_dir = request.output_dir;
		if (result.message.empty()) {
			result.message = result.exit_code == 0
				? "batch playback probe completed"
				: "batch playback probe completed with failures";
		}
		bool const directory_ready = agi::fs::DirectoryExists(request.output_dir) || agi::fs::CreateDirectory(request.output_dir);
		if (directory_ready) {
			WriteBatchResultsCsv(request.output_dir, results);
			WriteBatchSummary(request.output_dir, request.list_file, result);
			WriteBatchManifestJson(request.output_dir, request.list_file, results, result);
		}
		if (on_done)
			on_done(std::move(result));
	}

	void RunNext() {
		try {
			if (next_index >= cases.size()) {
				BatchPlaybackProbeResult result;
				result.total_cases = results.size();
				for (auto const& item : results) {
					if (item.probe_result.passed)
						++result.passed_cases;
					else
						++result.failed_cases;
				}
				result.exit_code = result.failed_cases == 0 ? 0 : 1;
				Finish(std::move(result));
				return;
			}

			auto const index = next_index++;
			auto const spec = cases[index];
			auto const trace_dir = request.output_dir / detail::CaseDirectoryName(index);

			auto probe_request = request.probe_template;
			probe_request.video_path = spec.video_path;
			if (probe_request.skip_audio) {
				probe_request.audio_path.clear();
			}
			else if (spec.audio_path) {
				probe_request.audio_path = *spec.audio_path;
			}
			else {
				probe_request.audio_path = spec.video_path;
			}
			probe_request.trace_dir = trace_dir;

			auto self = shared_from_this();
			aegisub::playback_probe_service::RunAsync(std::move(probe_request), [self, index, spec](headless_playback_probe::PlaybackProbeResult probe_result) mutable {
				// Let the previous probe fully unwind and destroy its runtime/session
				// before the batch runner starts constructing the next case.
				agi::dispatch::Main().Async([self, index, spec = std::move(spec), probe_result = std::move(probe_result)]() mutable {
					self->results.push_back(detail::BatchCaseResult{
						index,
						std::move(spec),
						std::move(probe_result),
					});
					if (!self->finished)
						self->RunNext();
				});
			});
		}
		catch (std::exception const& error) {
			auto const failed_index = next_index - 1;
			auto failed_spec = cases[failed_index];
			auto const trace_dir = request.output_dir / detail::CaseDirectoryName(failed_index);
			RecordCaseFailure(failed_index, std::move(failed_spec), trace_dir, error.what());
		}
		catch (agi::Exception const& error) {
			auto const failed_index = next_index - 1;
			auto failed_spec = cases[failed_index];
			auto const trace_dir = request.output_dir / detail::CaseDirectoryName(failed_index);
			RecordCaseFailure(failed_index, std::move(failed_spec), trace_dir, error.GetMessage());
		}
		catch (...) {
			auto const failed_index = next_index - 1;
			auto failed_spec = cases[failed_index];
			auto const trace_dir = request.output_dir / detail::CaseDirectoryName(failed_index);
			RecordCaseFailure(failed_index, std::move(failed_spec), trace_dir, "unhandled exception");
		}
	}

public:
	BatchPlaybackProbeRunner(BatchPlaybackProbeRequest request, std::function<void(BatchPlaybackProbeResult)> on_done)
	: request(std::move(request))
	, on_done(std::move(on_done)) {
	}

	void Start() {
		try {
			agi::fs::CreateDirectory(request.output_dir);
			cases = detail::ReadBatchCaseList(request.list_file);
			if (cases.empty()) {
				BatchPlaybackProbeResult result;
				result.exit_code = 2;
				result.message = "batch playback probe list is empty";
				Finish(std::move(result));
				return;
			}
			ScheduleRunNext();
		}
		catch (std::exception const& error) {
			BatchPlaybackProbeResult result;
			result.exit_code = 2;
			result.message = error.what();
			Finish(std::move(result));
		}
	}
};

void WriteBatchTraceSummariesCsv(agi::fs::path const& output_dir, std::vector<aegisub::trace_summary_service::TraceSummaryRow> const& rows) {
	std::ofstream out(output_dir / "results.csv", std::ios::out | std::ios::trunc);
	out << "index,command,result,input_path,session_dir,build,video_path,audio_path,selected_video_provider,selected_audio_provider,actual_video_provider,actual_video_decoder,actual_audio_provider_factory,actual_audio_provider,video_provider_fallback,audio_provider_fallback,probe_performed_seeks,probe_seek_max_abs_delta_ms,probe_seek_mean_abs_delta_ms,session_open_count,session_reopen_count,session_close_count,session_query_count,session_play_count,session_playline_count,session_stop_count,session_jump_time_count,session_jump_frame_count,session_final_playback_uses_audio_authority\n";
	for (size_t i = 0; i < rows.size(); ++i) {
		auto const& row = rows[i];
		out
			<< (i + 1) << ','
			<< detail::CsvEscape(row.command) << ','
			<< detail::CsvEscape(row.result) << ','
			<< detail::CsvEscape(detail::ToGenericString(row.input_path)) << ','
			<< detail::CsvEscape(detail::ToGenericString(row.session_dir)) << ','
			<< detail::CsvEscape(row.build) << ','
			<< detail::CsvEscape(row.video_path) << ','
			<< detail::CsvEscape(row.audio_path) << ','
			<< detail::CsvEscape(row.selected_video_provider) << ','
			<< detail::CsvEscape(row.selected_audio_provider) << ','
			<< detail::CsvEscape(row.actual_video_provider) << ','
			<< detail::CsvEscape(row.actual_video_decoder) << ','
			<< detail::CsvEscape(row.actual_audio_provider_factory) << ','
			<< detail::CsvEscape(row.actual_audio_provider) << ','
			<< (row.video_provider_fallback ? "true" : "false") << ','
			<< (row.audio_provider_fallback ? "true" : "false") << ','
			<< row.probe_performed_seeks << ','
			<< row.probe_seek_max_abs_delta_ms << ','
			<< row.probe_seek_mean_abs_delta_ms << ','
			<< row.session_open_count << ','
			<< row.session_reopen_count << ','
			<< row.session_close_count << ','
			<< row.session_query_count << ','
			<< row.session_play_count << ','
			<< row.session_playline_count << ','
			<< row.session_stop_count << ','
			<< row.session_jump_time_count << ','
			<< row.session_jump_frame_count << ','
			<< (row.session_final_playback_uses_audio_authority ? "true" : "false")
			<< "\n";
	}
}

void WriteBatchTraceSummariesText(agi::fs::path const& output_dir, std::vector<aegisub::trace_summary_service::TraceSummaryRow> const& rows, BatchTraceSummarizeResult const& result) {
	std::ofstream out(output_dir / "summary.txt", std::ios::out | std::ios::trunc);
	out << "command=batch trace-summarize\n";
	out << "output_dir=" << detail::ToGenericString(result.output_dir) << "\n";
	out << "total_sessions=" << result.total_sessions << "\n";
	out << "passed_sessions=" << result.passed_sessions << "\n";
	out << "failed_sessions=" << result.failed_sessions << "\n";
	out << "result=" << (result.exit_code == 0 ? "PASS" : "FAIL") << "\n";
	out << "message=" << result.message << "\n";
	for (size_t i = 0; i < rows.size(); ++i)
		out << "input_" << (i + 1) << "=" << detail::ToGenericString(rows[i].input_path) << "\n";
}

void WriteBatchTraceSummariesManifest(agi::fs::path const& output_dir, std::vector<aegisub::trace_summary_service::TraceSummaryRow> const& rows, BatchTraceSummarizeResult const& result) {
	std::ofstream out(output_dir / "manifest.json", std::ios::out | std::ios::trunc);
	out << "{\n";
	out << "  \"command\": \"batch trace-summarize\",\n";
	out << "  \"output_dir\": \"" << detail::JsonEscape(detail::ToGenericString(result.output_dir)) << "\",\n";
	out << "  \"total_sessions\": " << result.total_sessions << ",\n";
	out << "  \"passed_sessions\": " << result.passed_sessions << ",\n";
	out << "  \"failed_sessions\": " << result.failed_sessions << ",\n";
	out << "  \"result\": \"" << (result.exit_code == 0 ? "PASS" : "FAIL") << "\",\n";
	out << "  \"message\": \"" << detail::JsonEscape(result.message) << "\",\n";
	out << "  \"sessions\": [\n";
	out << std::boolalpha;
	for (size_t i = 0; i < rows.size(); ++i) {
		auto const& row = rows[i];
		if (i)
			out << ",\n";
		out << "    {\n";
		out << "      \"index\": " << (i + 1) << ",\n";
		out << "      \"command\": \"" << detail::JsonEscape(row.command) << "\",\n";
		out << "      \"result\": \"" << detail::JsonEscape(row.result) << "\",\n";
		out << "      \"input_path\": \"" << detail::JsonEscape(detail::ToGenericString(row.input_path)) << "\",\n";
		out << "      \"session_dir\": \"" << detail::JsonEscape(detail::ToGenericString(row.session_dir)) << "\",\n";
		out << "      \"build\": \"" << detail::JsonEscape(row.build) << "\",\n";
		out << "      \"actual_video_decoder\": \"" << detail::JsonEscape(row.actual_video_decoder) << "\",\n";
		out << "      \"actual_audio_provider\": \"" << detail::JsonEscape(row.actual_audio_provider) << "\"\n";
		out << "    }";
	}
	out << "\n  ]\n";
	out << "}\n";
}

void WriteBatchAssInfoCsv(agi::fs::path const& output_dir, std::vector<std::pair<agi::fs::path, AssInfoInspectResult>> const& rows) {
	std::ofstream out(output_dir / "results.csv", std::ios::out | std::ios::trunc);
	out << "index,passed,subtitle_path,format_name,title,script_type,wrap_style,scaled_border_and_shadow,play_res_x,play_res_y,layout_res_x,layout_res_y,info_count,style_count,event_count,dialogue_count,comment_count,attachment_count,extradata_count,project_audio_file,project_video_file,project_timecodes_file,project_keyframes_file,error\n";
	for (size_t i = 0; i < rows.size(); ++i) {
		auto const& [path, inspect] = rows[i];
		auto passed = inspect.snapshot.has_value();
		out << (i + 1) << ','
			<< (passed ? "true" : "false") << ','
			<< detail::CsvEscape(detail::ToGenericString(path)) << ',';
		if (!passed) {
			out << ",,,,,,,,,,,,,,,,,,,,"
				<< detail::CsvEscape(inspect.error) << "\n";
			continue;
		}
		auto const& snapshot = *inspect.snapshot;
		out
			<< detail::CsvEscape(snapshot.format_name) << ','
			<< detail::CsvEscape(snapshot.title) << ','
			<< detail::CsvEscape(snapshot.script_type) << ','
			<< detail::CsvEscape(snapshot.wrap_style) << ','
			<< detail::CsvEscape(snapshot.scaled_border_and_shadow) << ','
			<< snapshot.play_res_x << ','
			<< snapshot.play_res_y << ','
			<< snapshot.layout_res_x << ','
			<< snapshot.layout_res_y << ','
			<< snapshot.info_count << ','
			<< snapshot.style_count << ','
			<< snapshot.event_count << ','
			<< snapshot.dialogue_count << ','
			<< snapshot.comment_count << ','
			<< snapshot.attachment_count << ','
			<< snapshot.extradata_count << ','
			<< detail::CsvEscape(snapshot.project_audio_file) << ','
			<< detail::CsvEscape(snapshot.project_video_file) << ','
			<< detail::CsvEscape(snapshot.project_timecodes_file) << ','
			<< detail::CsvEscape(snapshot.project_keyframes_file) << ','
			<< detail::CsvEscape(inspect.error)
			<< "\n";
	}
}

void WriteBatchAssInfoText(agi::fs::path const& output_dir, std::vector<std::pair<agi::fs::path, AssInfoInspectResult>> const& rows, BatchAssInfoResult const& result) {
	std::ofstream out(output_dir / "summary.txt", std::ios::out | std::ios::trunc);
	out << "command=batch ass-info\n";
	out << "output_dir=" << detail::ToGenericString(result.output_dir) << "\n";
	out << "total_files=" << result.total_files << "\n";
	out << "passed_files=" << result.passed_files << "\n";
	out << "failed_files=" << result.failed_files << "\n";
	out << "result=" << (result.exit_code == 0 ? "PASS" : "FAIL") << "\n";
	out << "message=" << result.message << "\n";
	for (size_t i = 0; i < rows.size(); ++i)
		out << "input_" << (i + 1) << "=" << detail::ToGenericString(rows[i].first) << "\n";
}

void WriteBatchAssInfoManifest(agi::fs::path const& output_dir, std::vector<std::pair<agi::fs::path, AssInfoInspectResult>> const& rows, BatchAssInfoResult const& result) {
	std::ofstream out(output_dir / "manifest.json", std::ios::out | std::ios::trunc);
	out << "{\n";
	out << "  \"command\": \"batch ass-info\",\n";
	out << "  \"output_dir\": \"" << detail::JsonEscape(detail::ToGenericString(result.output_dir)) << "\",\n";
	out << "  \"total_files\": " << result.total_files << ",\n";
	out << "  \"passed_files\": " << result.passed_files << ",\n";
	out << "  \"failed_files\": " << result.failed_files << ",\n";
	out << "  \"result\": \"" << (result.exit_code == 0 ? "PASS" : "FAIL") << "\",\n";
	out << "  \"message\": \"" << detail::JsonEscape(result.message) << "\",\n";
	out << "  \"files\": [\n";
	for (size_t i = 0; i < rows.size(); ++i) {
		auto const& [path, inspect] = rows[i];
		if (i)
			out << ",\n";
		out << "    {\n";
		out << "      \"index\": " << (i + 1) << ",\n";
		out << "      \"subtitle_path\": \"" << detail::JsonEscape(detail::ToGenericString(path)) << "\",\n";
		out << "      \"passed\": " << (inspect.snapshot ? "true" : "false") << ",\n";
		out << "      \"error\": \"" << detail::JsonEscape(inspect.error) << "\"";
		if (inspect.snapshot) {
			out << ",\n";
			out << "      \"format_name\": \"" << detail::JsonEscape(inspect.snapshot->format_name) << "\",\n";
			out << "      \"title\": \"" << detail::JsonEscape(inspect.snapshot->title) << "\",\n";
			out << "      \"dialogue_count\": " << inspect.snapshot->dialogue_count << "\n";
		}
		else {
			out << "\n";
		}
		out << "    }";
	}
	out << "\n  ]\n";
	out << "}\n";
}

}

void RunBatchPlaybackProbeAsync(BatchPlaybackProbeRequest request, std::function<void(BatchPlaybackProbeResult)> on_done) {
	auto runner = std::make_shared<BatchPlaybackProbeRunner>(std::move(request), std::move(on_done));
	runner->Start();
}

BatchTraceSummarizeResult RunBatchTraceSummarize(BatchTraceSummarizeRequest const& request) {
	BatchTraceSummarizeResult result;
	result.output_dir = request.output_dir;
	try {
		agi::fs::CreateDirectory(request.output_dir);
		auto summary = aegisub::trace_summary_service::Summarize(request.inputs);
		if (!summary.error.empty()) {
			result.exit_code = 2;
			result.message = summary.error;
			return result;
		}

		result.total_sessions = summary.rows.size();
		for (auto const& row : summary.rows) {
			if (row.result == "PASS")
				++result.passed_sessions;
			else
				++result.failed_sessions;
		}
		result.exit_code = result.failed_sessions == 0 ? 0 : 1;
		result.message = result.exit_code == 0
			? "batch trace summarize completed"
			: "batch trace summarize completed with failures";

		WriteBatchTraceSummariesCsv(request.output_dir, summary.rows);
		WriteBatchTraceSummariesText(request.output_dir, summary.rows, result);
		WriteBatchTraceSummariesManifest(request.output_dir, summary.rows, result);
	}
	catch (std::exception const& error) {
		result.exit_code = 2;
		result.message = error.what();
	}
	return result;
}

BatchAssInfoResult RunBatchAssInfo(BatchAssInfoRequest const& request) {
	BatchAssInfoResult result;
	result.output_dir = request.output_dir;
	try {
		agi::fs::CreateDirectory(request.output_dir);
		std::vector<std::pair<agi::fs::path, AssInfoInspectResult>> rows;
		rows.reserve(request.inputs.size());
		for (auto const& input : request.inputs)
			rows.emplace_back(input, aegisub::ass_info_service::Inspect({input, request.encoding}));

		result.total_files = rows.size();
		for (auto const& row : rows) {
			if (row.second.snapshot)
				++result.passed_files;
			else
				++result.failed_files;
		}
		result.exit_code = result.failed_files == 0 ? 0 : 1;
		result.message = result.exit_code == 0
			? "batch ass-info completed"
			: "batch ass-info completed with failures";

		WriteBatchAssInfoCsv(request.output_dir, rows);
		WriteBatchAssInfoText(request.output_dir, rows, result);
		WriteBatchAssInfoManifest(request.output_dir, rows, result);
	}
	catch (std::exception const& error) {
		result.exit_code = 2;
		result.message = error.what();
	}
	return result;
}

}
