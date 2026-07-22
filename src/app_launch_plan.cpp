#include "app_launch_plan.h"

#include <CLI/CLI.hpp>

#include <libaegisub/fs.h>

#include <charconv>
#include <cmath>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

CLI::Validator NonEmptyValue() {
	return CLI::Validator(
		[](std::string& value) {
			if (value.empty())
				return std::string("value must not be empty");
			if (value.size() > 1 && value.front() == '-')
				return std::string("value must not be another option");
			return std::string();
		},
		"NONEMPTY");
}

CLI::Validator NameValuePair() {
	return CLI::Validator(
		[](std::string& value) {
			if (value.size() > 1 && value.front() == '-')
				return std::string("value must not be another option");
			auto const separator = value.find('=');
			return separator == std::string::npos || separator == 0
				? std::string("value must use name=value")
				: std::string();
		},
		"NAME=VALUE");
}

CLI::Validator FinitePositiveNumber() {
	return CLI::Validator(
		[](std::string& value) {
			try {
				size_t consumed = 0;
				auto const parsed = std::stod(value, &consumed);
				if (consumed == value.size() && std::isfinite(parsed) && parsed > 0.0)
					return std::string();
			}
			catch (...) {
			}
			return std::string("value must be a finite positive number");
		},
		"FINITE_POSITIVE");
}

CLI::Validator DecimalInteger(int minimum, int maximum, std::string description) {
	return CLI::Validator(
		[minimum, maximum](std::string& value) {
			std::string_view input = value;
			if (input.starts_with('+'))
				input.remove_prefix(1);
			if (input.empty())
				return std::string("value must be a decimal integer");

			int parsed = 0;
			auto const [end, error] = std::from_chars(
				input.data(), input.data() + input.size(), parsed, 10);
			if (error != std::errc() || end != input.data() + input.size())
				return std::string("value must be a decimal integer");
			if (parsed < minimum || parsed > maximum)
				return std::string("decimal integer is outside the permitted range");

			// CLI11's default int conversion accepts base-0 and bool spellings.
			// Canonicalizing here keeps validated decimal input decimal.
			value = std::to_string(parsed);
			return std::string();
		},
		std::move(description));
}

CLI::Validator NonNegativeInteger() {
	return DecimalInteger(0, std::numeric_limits<int>::max(), "NONNEGATIVE_DECIMAL");
}

CLI::Validator PositiveInteger() {
	return DecimalInteger(1, std::numeric_limits<int>::max(), "POSITIVE_DECIMAL");
}

struct RunOptions {
	std::string scenario;
	std::vector<std::string> inputs;
	std::string profile_directory;
	std::string artifacts_directory;
	bool keep_profile = false;
	bool internal_worker = false;
};

struct GuiHostOptions {
	std::string profile_directory;
	std::string artifacts_directory;
	std::vector<std::string> open_files;
	bool keep_profile = false;
};

struct ProbeOptions {
	std::string video;
	std::string audio;
	std::string video_provider;
	std::string audio_provider;
	std::string trace_directory;
	bool skip_audio = false;
	int line_start_ms = 0;
	int repeat_count = 1;
	int repeat_gap_ms = 0;
	int seek_after_ms = 0;
	int seek_target_offset_ms = 0;
	int video_track_index = 0;
	int audio_track_index = 0;
	int duration_ms = 2000;
	double audio_rate_scale = 0.9;
	int audio_quantum_ms = 0;
	int max_abs_delta_ms = 100;
	CLI::Option* video_provider_option = nullptr;
	CLI::Option* audio_provider_option = nullptr;
	CLI::Option* trace_directory_option = nullptr;
	CLI::Option* skip_audio_option = nullptr;
	CLI::Option* seek_after_option = nullptr;
	CLI::Option* seek_target_offset_option = nullptr;
	CLI::Option* video_track_option = nullptr;
	CLI::Option* audio_track_option = nullptr;
};

struct MediaInspectOptions {
	std::string video;
	std::string audio;
	std::string video_provider;
	std::string audio_provider;
	std::string trace_directory;
	bool skip_audio = false;
	int video_track_index = 0;
	int audio_track_index = 0;
	int subtitle_track_index = 0;
	double audio_rate_scale = 1.0;
	int audio_quantum_ms = 0;
	CLI::Option* video_provider_option = nullptr;
	CLI::Option* audio_provider_option = nullptr;
	CLI::Option* trace_directory_option = nullptr;
	CLI::Option* skip_audio_option = nullptr;
	CLI::Option* video_track_option = nullptr;
	CLI::Option* audio_track_option = nullptr;
	CLI::Option* subtitle_track_option = nullptr;
};

struct SingleInputOptions {
	std::string positional;
	std::string named;
};

void ConfigureRoot(CLI::App& app) {
	app.require_subcommand(1, 1);
	app.failure_message(CLI::FailureMessage::help);
}

void AddRunOptions(CLI::App& command, RunOptions& options, bool allow_internal_worker) {
	command.add_option("--scenario", options.scenario, "Versioned automation scenario JSON")
		->required()
		->check(NonEmptyValue());
	command.add_option("--input", options.inputs, "Scenario resource override as name=value")
		->expected(1)
		->allow_extra_args(false)
		->multi_option_policy(CLI::MultiOptionPolicy::TakeAll)
		->check(NameValuePair());
	command.add_option("--profile-dir", options.profile_directory, "Automation profile directory")
		->check(NonEmptyValue());
	command.add_option("--artifacts", options.artifacts_directory, "Automation artifacts directory")
		->check(NonEmptyValue());
	command.add_flag("--keep-profile", options.keep_profile, "Keep the automation profile after success");
	if (allow_internal_worker) {
		command.add_flag("--automation-worker", options.internal_worker)
			->group("");
	}
}

void AddGuiHostOptions(CLI::App& command, GuiHostOptions& options) {
	command.add_option("--profile-dir", options.profile_directory, "Automation profile directory")
		->check(NonEmptyValue());
	command.add_option("--artifacts", options.artifacts_directory, "Automation artifacts directory")
		->check(NonEmptyValue());
	command.add_option("--open", options.open_files, "File to open after the GUI host starts")
		->expected(1)
		->allow_extra_args(false)
		->multi_option_policy(CLI::MultiOptionPolicy::TakeAll)
		->check(NonEmptyValue());
	command.add_flag("--keep-profile", options.keep_profile, "Keep the automation profile after success");
}

void AddProbeOptions(CLI::App& command, ProbeOptions& options) {
	command.add_option("--probe-video", options.video, "Video path")
		->required()
		->check(NonEmptyValue());
	auto* audio_option = command.add_option("--probe-audio", options.audio, "Audio path")
		->check(NonEmptyValue());
	options.skip_audio_option = command.add_flag(
		"--probe-skip-audio", options.skip_audio, "Run without audio");
	command.add_option("--probe-line-start-ms", options.line_start_ms, "Playback line start in milliseconds")
		->transform(NonNegativeInteger());
	command.add_option("--probe-repeat-count", options.repeat_count, "Playback repetition count")
		->transform(PositiveInteger());
	command.add_option("--probe-repeat-gap-ms", options.repeat_gap_ms, "Gap between repetitions in milliseconds")
		->transform(NonNegativeInteger());
	options.seek_after_option = command.add_option(
		"--probe-seek-after-ms", options.seek_after_ms, "Delay before the probe seek in milliseconds")
		->transform(NonNegativeInteger());
	options.seek_target_offset_option = command.add_option(
		"--probe-seek-target-offset-ms", options.seek_target_offset_ms, "Seek target offset in milliseconds")
		->transform(NonNegativeInteger());
	options.seek_after_option->needs(options.seek_target_offset_option);
	options.seek_target_offset_option->needs(options.seek_after_option);
	options.video_provider_option = command.add_option(
		"--probe-video-provider", options.video_provider, "Video provider name")
		->check(NonEmptyValue());
	options.audio_provider_option = command.add_option(
		"--probe-audio-provider", options.audio_provider, "Audio provider name")
		->check(NonEmptyValue());
	options.video_track_option = command.add_option(
		"--probe-video-track-index", options.video_track_index, "Zero-based video track index")
		->transform(NonNegativeInteger());
	options.audio_track_option = command.add_option(
		"--probe-audio-track-index", options.audio_track_index, "Zero-based audio track index")
		->transform(NonNegativeInteger());
	command.add_option("--probe-duration-ms", options.duration_ms, "Playback duration in milliseconds")
		->transform(PositiveInteger());
	auto* audio_rate_option = command.add_option(
		"--probe-audio-rate-scale", options.audio_rate_scale, "Audio playback rate scale")
		->check(FinitePositiveNumber());
	auto* audio_quantum_option = command.add_option(
		"--probe-audio-quantum-ms", options.audio_quantum_ms, "Audio timer quantum in milliseconds")
		->transform(NonNegativeInteger());
	auto* max_abs_delta_option = command.add_option(
		"--probe-max-abs-delta-ms", options.max_abs_delta_ms, "Maximum permitted A/V delta")
		->transform(PositiveInteger());
	options.trace_directory_option = command.add_option(
		"--probe-trace-dir", options.trace_directory, "Trace output directory")
		->check(NonEmptyValue());
	options.skip_audio_option->excludes(audio_option);
	options.skip_audio_option->excludes(options.audio_provider_option);
	options.skip_audio_option->excludes(options.audio_track_option);
	options.skip_audio_option->excludes(audio_rate_option);
	options.skip_audio_option->excludes(audio_quantum_option);
	options.skip_audio_option->excludes(max_abs_delta_option);
}

void AddMediaInspectOptions(CLI::App& command, MediaInspectOptions& options) {
	auto* input_group = command.add_option_group("Media input");
	input_group->add_option("--video", options.video, "Video path")
		->check(NonEmptyValue());
	auto* audio_option = input_group->add_option("--audio", options.audio, "Audio path")
		->check(NonEmptyValue());
	input_group->require_option(1, 2);

	options.video_provider_option = command.add_option(
		"--video-provider", options.video_provider, "Video provider name")
		->check(NonEmptyValue());
	options.audio_provider_option = command.add_option(
		"--audio-provider", options.audio_provider, "Audio provider name")
		->check(NonEmptyValue());
	options.video_track_option = command.add_option(
		"--video-track-index", options.video_track_index, "Zero-based video track index")
		->transform(NonNegativeInteger());
	options.audio_track_option = command.add_option(
		"--audio-track-index", options.audio_track_index, "Zero-based audio track index")
		->transform(NonNegativeInteger());
	options.subtitle_track_option = command.add_option(
		"--subtitle-track-index", options.subtitle_track_index, "Zero-based subtitle track index")
		->transform(NonNegativeInteger());
	options.skip_audio_option = command.add_flag("--skip-audio", options.skip_audio, "Do not open audio");
	auto* audio_rate_option = command.add_option(
		"--audio-rate-scale", options.audio_rate_scale, "Audio playback rate scale")
		->check(FinitePositiveNumber());
	auto* audio_quantum_option = command.add_option(
		"--audio-quantum-ms", options.audio_quantum_ms, "Audio timer quantum in milliseconds")
		->transform(NonNegativeInteger());
	options.trace_directory_option = command.add_option(
		"--trace-dir", options.trace_directory, "Trace output directory")
		->check(NonEmptyValue());
	options.skip_audio_option->excludes(audio_option);
	options.skip_audio_option->excludes(options.audio_provider_option);
	options.skip_audio_option->excludes(options.audio_track_option);
	options.skip_audio_option->excludes(audio_rate_option);
	options.skip_audio_option->excludes(audio_quantum_option);
}

void AddSingleInputOptions(
	CLI::App& command,
	SingleInputOptions& options,
	std::string const& positional_name,
	std::string const& description) {
	auto* input_group = command.add_option_group("Input");
	input_group->add_option(positional_name, options.positional, description)
		->check(NonEmptyValue());
	input_group->add_option("--input", options.named, description)
		->check(NonEmptyValue());
	input_group->require_option(1, 1);
}

void AddFontCollectorCommonOptions(
	CLI::App& command,
	aegisub::fontcollector_subcommand::Options& options) {
	command.add_option("inputs", options.inputs, "ASS/SSA subtitle files or directories")
		->required()
		->expected(1, -1)
		->check(NonEmptyValue());
	command.add_option("--encoding", options.encoding,
		"Input subtitle encoding; omitted enables automatic detection")
		->check(NonEmptyValue());
	command.add_option("--matcher", options.matcher, "Font matcher: platform or libass")
		->check(CLI::IsMember({"platform", "libass"}));
	command.add_option("--additional-fonts", options.additional_fonts,
		"Additional font files or directories (non-recursive; libass matcher only)")
		->expected(1)
		->allow_extra_args(false)
		->multi_option_policy(CLI::MultiOptionPolicy::TakeAll)
		->check(NonEmptyValue());
	command.add_option("--additional-fonts-recursive", options.additional_fonts_recursive,
		"Additional font directories searched recursively (libass matcher only)")
		->expected(1)
		->allow_extra_args(false)
		->multi_option_policy(CLI::MultiOptionPolicy::TakeAll)
		->check(NonEmptyValue());
	command.add_flag("--exclude-system-fonts", options.exclude_system_fonts,
		"Match only additional fonts (libass matcher only)");
	command.add_flag("--details", options.details,
		"Print ASS font usage and matched font details");
	command.add_flag("--json", options.json, "Print structured JSON output for automation");
	command.add_flag("-r,--recursive", options.recursive,
		"Recursively scan input directories for .ass/.ssa files");
}

void AddFontCollectorNormalizationOptions(
	CLI::App& command,
	aegisub::fontcollector_subcommand::Options& options) {
	command.add_option("inputs", options.inputs, "ASS/SSA subtitle files or directories")
		->required()
		->expected(1, -1)
		->check(NonEmptyValue());
	command.add_option("--encoding", options.encoding,
		"Input subtitle encoding; omitted enables automatic detection")
		->check(NonEmptyValue());
	command.add_option("--target", options.normalization_target,
		"Preferred family name: localized or english")
		->check(CLI::IsMember({"localized", "english"}));
	command.add_flag("--details", options.details, "Include match evidence in text output");
	command.add_flag("--json", options.json, "Print structured JSON normalization plans");
	command.add_flag("-r,--recursive", options.recursive,
		"Recursively scan input directories for .ass/.ssa files");
}

bool ParseWithCli11(
	CLI::App& app,
	std::vector<std::string> const& args,
	size_t first_argument,
	AppLaunchPlan& plan) {
	std::vector<std::string> storage;
	storage.reserve(args.size() - first_argument + 1);
	storage.emplace_back(app.get_name());
	storage.insert(storage.end(), args.begin() + first_argument, args.end());

	std::vector<char*> argv;
	argv.reserve(storage.size());
	for (auto& argument : storage)
		argv.push_back(argument.data());

	try {
		app.parse(static_cast<int>(argv.size()), argv.data());
		return true;
	}
	catch (CLI::ParseError const& error) {
		std::ostringstream standard_output;
		std::ostringstream standard_error;
		auto const cli_exit_code = app.exit(error, standard_output, standard_error);
		auto message = cli_exit_code == 0 ? standard_output.str() : standard_error.str();
		if (message.empty())
			message = error.what();
		plan.immediate_exit_code = cli_exit_code == 0 ? 0 : 64;
		plan.immediate_output = std::move(message);
		if (cli_exit_code != 0)
			plan.error = plan.immediate_output;
		return false;
	}
}

aegisub::headless_automation_cli::RunRequest BuildRunRequest(RunOptions const& options) {
	aegisub::headless_automation_cli::RunRequest request;
	request.scenario_path = agi::fs::PathFromString(options.scenario);
	for (auto const& input : options.inputs) {
		auto const separator = input.find('=');
		request.inputs.emplace_back(input.substr(0, separator), input.substr(separator + 1));
	}
	if (!options.profile_directory.empty())
		request.profile_directory = agi::fs::PathFromString(options.profile_directory);
	if (!options.artifacts_directory.empty())
		request.artifacts_directory = agi::fs::PathFromString(options.artifacts_directory);
	request.keep_profile = options.keep_profile;
	request.internal_worker = options.internal_worker;
	return request;
}

aegisub::headless_service_cli::ProbePlaybackCommand BuildProbeCommand(ProbeOptions const& options) {
	aegisub::playback_probe_service::PlaybackProbeRequest request;
	request.video_path = agi::fs::PathFromString(options.video);
	if (!options.audio.empty())
		request.audio_path = agi::fs::PathFromString(options.audio);
	else if (!options.skip_audio)
		request.audio_path = request.video_path;
	request.skip_audio = options.skip_audio;
	request.line_start_ms = options.line_start_ms;
	request.repeat_count = options.repeat_count;
	request.repeat_gap_ms = options.repeat_gap_ms;
	request.duration_ms = options.duration_ms;
	request.audio_rate_scale = options.audio_rate_scale;
	request.audio_quantum_ms = options.audio_quantum_ms;
	request.max_allowed_abs_delta_ms = options.max_abs_delta_ms;
	if (options.seek_after_option->count())
		request.seek_after_ms = options.seek_after_ms;
	if (options.seek_target_offset_option->count())
		request.seek_target_offset_ms = options.seek_target_offset_ms;
	if (options.video_provider_option->count())
		request.video_provider = options.video_provider;
	if (options.audio_provider_option->count())
		request.audio_provider = options.audio_provider;
	if (options.video_track_option->count())
		request.video_track_index = options.video_track_index;
	if (options.audio_track_option->count())
		request.audio_track_index = options.audio_track_index;
	if (options.trace_directory_option->count())
		request.trace_dir = agi::fs::PathFromString(options.trace_directory);
	return {std::move(request)};
}

aegisub::headless_service_cli::InspectMediaCommand BuildMediaInspectCommand(
	MediaInspectOptions const& options) {
	aegisub::media_inspect_service::MediaInspectRequest request;
	if (!options.video.empty())
		request.video_path = agi::fs::PathFromString(options.video);
	if (!options.audio.empty())
		request.audio_path = agi::fs::PathFromString(options.audio);
	else if (!options.skip_audio)
		request.audio_path = request.video_path;
	request.skip_audio = options.skip_audio;
	request.audio_rate_scale = options.audio_rate_scale;
	request.audio_quantum_ms = options.audio_quantum_ms;
	if (options.video_provider_option->count())
		request.video_provider = options.video_provider;
	if (options.audio_provider_option->count())
		request.audio_provider = options.audio_provider;
	if (options.video_track_option->count())
		request.video_track_index = options.video_track_index;
	if (options.audio_track_option->count())
		request.audio_track_index = options.audio_track_index;
	if (options.subtitle_track_option->count())
		request.subtitle_track_index = options.subtitle_track_index;
	if (options.trace_directory_option->count())
		request.trace_dir = agi::fs::PathFromString(options.trace_directory);
	return {std::move(request)};
}

std::string const& SelectedInput(SingleInputOptions const& options) {
	return options.positional.empty() ? options.named : options.positional;
}

void ParseHeadless(std::vector<std::string> const& args, AppLaunchPlan& plan) {
	CLI::App app{"Run Aegisub automation and inspection without the GUI", "Aegisub.exe --headless"};
	ConfigureRoot(app);

	RunOptions run_options;
	auto* run = app.add_subcommand("run", "Run a versioned JSON automation scenario");
	AddRunOptions(*run, run_options, true);

	ProbeOptions probe_options;
	auto* probe = app.add_subcommand("probe", "Run a focused runtime probe");
	probe->require_subcommand(1, 1);
	auto* playback = probe->add_subcommand("playback", "Validate playback timing and provider selection");
	AddProbeOptions(*playback, probe_options);

	MediaInspectOptions media_options;
	SingleInputOptions ass_info_options;
	SingleInputOptions trace_options;
	std::string ass_encoding;
	auto* inspect = app.add_subcommand("inspect", "Inspect runtime-readable project artifacts");
	inspect->require_subcommand(1, 1);
	auto* media = inspect->add_subcommand("media", "Inspect media providers, tracks, and playback state");
	AddMediaInspectOptions(*media, media_options);
	auto* ass_info = inspect->add_subcommand("ass-info", "Inspect subtitle script metadata");
	AddSingleInputOptions(*ass_info, ass_info_options, "subtitle", "Subtitle script path");
	ass_info->add_option("--encoding", ass_encoding, "Subtitle character encoding")
		->check(NonEmptyValue());
	auto* trace = inspect->add_subcommand("trace", "Inspect a trace session");
	AddSingleInputOptions(*trace, trace_options, "trace", "Trace session directory or trace file");

	if (!ParseWithCli11(app, args, 2, plan))
		return;

	if (*run) {
		plan.headless_run = BuildRunRequest(run_options);
		return;
	}
	if (*playback) {
		plan.headless_service.emplace(BuildProbeCommand(probe_options));
		return;
	}
	if (*media) {
		plan.headless_service.emplace(BuildMediaInspectCommand(media_options));
		return;
	}
	if (*ass_info) {
		aegisub::ass_info_service::AssInfoInspectRequest request;
		request.subtitle_path = agi::fs::PathFromString(SelectedInput(ass_info_options));
		request.encoding = std::move(ass_encoding);
		plan.headless_service.emplace(
			aegisub::headless_service_cli::InspectAssInfoCommand{std::move(request)});
		return;
	}
	if (*trace) {
		aegisub::trace_inspect_service::TraceInspectRequest request;
		request.input_path = agi::fs::PathFromString(SelectedInput(trace_options));
		plan.headless_service.emplace(
			aegisub::headless_service_cli::InspectTraceCommand{std::move(request)});
	}
}

void ParseFontCollector(std::vector<std::string> const& args, AppLaunchPlan& plan) {
	using aegisub::fontcollector_subcommand::Options;
	using aegisub::fontcollector_subcommand::Operation;

	CLI::App app{
		"Collect, validate, list, or normalize fonts used by ASS/SSA subtitle scripts",
		"Aegisub.exe fontcollector"};
	ConfigureRoot(app);

	Options check_options;
	check_options.operation = Operation::Check;
	auto* check = app.add_subcommand("check", "Check fonts used by subtitle scripts");
	AddFontCollectorCommonOptions(*check, check_options);
	check->add_flag("--strict", check_options.strict,
		"Exit non-zero when fonts, glyphs, or styles are missing");

	Options collect_options;
	collect_options.operation = Operation::Collect;
	auto* collect = app.add_subcommand("collect", "Copy fonts used by subtitle scripts");
	AddFontCollectorCommonOptions(*collect, collect_options);
	auto* destination = collect->add_option_group("Destination");
	destination->add_option("--to", collect_options.destination, "Copy fonts to directory")
		->option_text("DIR")
		->check(NonEmptyValue());
	destination->add_flag("--to-script-dir", collect_options.copy_to_script_directory,
		"Copy fonts next to each subtitle file");
	destination->require_option(1, 1);
	collect->add_flag("--strict", collect_options.strict,
		"Exit non-zero when fonts, glyphs, styles, or copies are missing");

	Options validate_options;
	validate_options.operation = Operation::Validate;
	validate_options.strict = true;
	auto* validate = app.add_subcommand("validate", "Validate fonts with strict exit status");
	AddFontCollectorCommonOptions(*validate, validate_options);

	Options list_options;
	list_options.operation = Operation::List;
	auto* list = app.add_subcommand("list", "List fonts used by subtitle scripts");
	AddFontCollectorCommonOptions(*list, list_options);
	list->add_flag("-q,--quiet", list_options.quiet, "Suppress normal font list output");

	Options normalize_options;
	normalize_options.operation = Operation::Normalize;
	auto* normalize = app.add_subcommand(
		"normalize", "Build a read-only ASS font-name normalization plan");
	AddFontCollectorNormalizationOptions(*normalize, normalize_options);

	if (!ParseWithCli11(app, args, 2, plan))
		return;

	if (*check)
		plan.fontcollector_options.emplace(std::move(check_options));
	else if (*collect)
		plan.fontcollector_options.emplace(std::move(collect_options));
	else if (*validate)
		plan.fontcollector_options.emplace(std::move(validate_options));
	else if (*list)
		plan.fontcollector_options.emplace(std::move(list_options));
	else if (*normalize)
		plan.fontcollector_options.emplace(std::move(normalize_options));
}

void ParseGuiTest(std::vector<std::string> const& args, AppLaunchPlan& plan) {
	CLI::App app{"Run or host deterministic Aegisub GUI automation", "Aegisub.exe --gui-test"};
	ConfigureRoot(app);

	GuiHostOptions host_options;
	auto* host = app.add_subcommand("host", "Start an isolated GUI host for an external driver");
	AddGuiHostOptions(*host, host_options);

	RunOptions run_options;
	auto* run = app.add_subcommand("run", "Run a versioned JSON scenario inside the GUI");
	AddRunOptions(*run, run_options, false);

	if (!ParseWithCli11(app, args, 2, plan))
		return;

	if (*host) {
		plan.gui_test_host = true;
		aegisub::headless_automation_cli::RunRequest request;
		if (!host_options.profile_directory.empty())
			request.profile_directory = agi::fs::PathFromString(host_options.profile_directory);
		if (!host_options.artifacts_directory.empty())
			request.artifacts_directory = agi::fs::PathFromString(host_options.artifacts_directory);
		request.keep_profile = host_options.keep_profile;
		plan.gui_test_open_files = std::move(host_options.open_files);
		plan.gui_test_run.emplace(std::move(request));
		return;
	}

	if (*run)
		plan.gui_test_run = BuildRunRequest(run_options);
}

}

AppLaunchPlan ParseAppLaunchPlan(std::vector<std::string> const& args) {
	AppLaunchPlan plan;
	plan.original_args = args;
	if (args.size() < 2)
		return plan;

	if (args[1] == "--headless") {
		plan.mode = AppLaunchMode::Headless;
		ParseHeadless(args, plan);
		return plan;
	}

	if (args[1] == "--gui-test") {
		plan.mode = AppLaunchMode::GuiTest;
		ParseGuiTest(args, plan);
		return plan;
	}

	if (args[1] == "fontcollector") {
		plan.mode = AppLaunchMode::FontCollector;
		ParseFontCollector(args, plan);
		return plan;
	}

	if (args[1] == "--cli") {
		plan.mode = AppLaunchMode::Headless;
		plan.error = "--cli is no longer supported; use --headless run, probe, or inspect";
	}
	else if (args[1] == "--headless-playback-probe") {
		plan.mode = AppLaunchMode::Headless;
		plan.error = "--headless-playback-probe is no longer supported; use --headless probe playback";
	}
	return plan;
}
