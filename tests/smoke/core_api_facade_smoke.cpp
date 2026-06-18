#include "core_api_facade.h"
#include "options.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

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

class ScopedNullOptions {
	agi::Options *previous = nullptr;

public:
	ScopedNullOptions()
	: previous(config::opt) {
		config::opt = nullptr;
	}

	~ScopedNullOptions() {
		config::opt = previous;
	}
};

std::filesystem::path MakeTempAssPath() {
	auto const stamp = std::chrono::steady_clock::now().time_since_epoch().count();
	return std::filesystem::temp_directory_path() / ("aegisub_core_api_facade_" + std::to_string(stamp) + ".ass");
}

std::filesystem::path MakeTempTxtPath() {
	auto const stamp = std::chrono::steady_clock::now().time_since_epoch().count();
	return std::filesystem::temp_directory_path() / ("aegisub_core_api_facade_" + std::to_string(stamp) + ".txt");
}

void WriteSmokeAss(std::filesystem::path const& path) {
	std::ofstream file(path, std::ios::binary);
	if (!file)
		throw std::runtime_error("failed to create facade smoke ASS file");

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

void WriteSmokeTxt(std::filesystem::path const& path) {
	std::ofstream file(path, std::ios::binary);
	if (!file)
		throw std::runtime_error("failed to create facade smoke TXT file");

	file <<
		"Alice: Hello from TXT\n"
		"# Internal note\n";
}

bool HasProvider(aegisub::core_api::ProviderCatalogSnapshot const& catalog,
                 std::string const& name,
                 bool hidden,
                 bool available) {
	return std::find_if(catalog.providers.begin(), catalog.providers.end(), [&](auto const& provider) {
		return provider.name == name
			&& provider.hidden == hidden
			&& provider.available == available;
	}) != catalog.providers.end();
}

std::uint32_t ChangeField(aegisub::core_api::SubtitleChangeFields field) {
	return static_cast<std::uint32_t>(field);
}

void RequireStatus(aegisub::core_api::Status actual, aegisub::core_api::Status expected, char const* context) {
	if (actual != expected)
		throw std::runtime_error(std::string(context) + " returned unexpected status");
}

void CheckStatusValuesMatchDraftCAbi() {
	using aegisub::core_api::Status;
	if (static_cast<std::uint32_t>(Status::Ok) != 0
		|| static_cast<std::uint32_t>(Status::Cancelled) != 1
		|| static_cast<std::uint32_t>(Status::InvalidArgument) != 2
		|| static_cast<std::uint32_t>(Status::FileNotFound) != 3
		|| static_cast<std::uint32_t>(Status::FileSystemError) != 4
		|| static_cast<std::uint32_t>(Status::NotSupported) != 5
		|| static_cast<std::uint32_t>(Status::NoMedia) != 6
		|| static_cast<std::uint32_t>(Status::RegistryFinalized) != 7
		|| static_cast<std::uint32_t>(Status::TimedOut) != 8
		|| static_cast<std::uint32_t>(Status::Error) != 100)
		throw std::runtime_error("core API facade status values drifted from draft C ABI");
}

void CheckContextCreationOptions() {
	std::unique_ptr<aegisub::core_api::Context> context;
	std::string error;
	aegisub::core_api::ContextOptions invalid_options;
	invalid_options.abi_version = aegisub::core_api::AbiVersion + 1;
	RequireStatus(aegisub::core_api::TryCreateContext(invalid_options, context, &error),
		aegisub::core_api::Status::InvalidArgument,
		"invalid ABI version context creation");
	if (context)
		throw std::runtime_error("invalid ABI version unexpectedly created a context");
	if (error.empty())
		throw std::runtime_error("invalid ABI version did not report an error");

	int posted = 0;
	bool ran_inline = false;
	aegisub::core_api::ContextOptions hook_options;
	hook_options.use_thread_hooks = true;
	hook_options.thread_hooks = {
		[&](aegisub::core::CoreHostTask task) {
			++posted;
			task();
		},
		[] {
			return true;
		},
		[] {
			return std::size_t{0};
		},
	};
	RequireStatus(aegisub::core_api::TryCreateContext(hook_options, context, &error),
		aegisub::core_api::Status::Ok,
		"custom host-thread context creation");
	if (!context)
		throw std::runtime_error("custom host-thread context was not created");
	context->ThreadContext().InvokeOnMain([&] {
		ran_inline = true;
	});
	if (!ran_inline)
		throw std::runtime_error("custom host-thread context did not run inline on host main thread");
	if (posted != 0)
		throw std::runtime_error("custom host-thread context posted while already on host main thread");
}

void RequireCatalog(aegisub::core_api::Context& context,
                    aegisub::core_api::ProviderKind kind,
                    char const* preferred,
                    char const* required_name,
                    bool hidden,
                    char const* context_name) {
	aegisub::core_api::ProviderCatalogSnapshot catalog;
	RequireStatus(aegisub::core_api::GetProviderCatalog(context, kind, preferred, catalog),
		aegisub::core_api::Status::Ok,
		context_name);
	if (catalog.preferred_provider != preferred)
		throw std::runtime_error(std::string(context_name) + " did not preserve preferred provider");
	if (!HasProvider(catalog, required_name, hidden, true))
		throw std::runtime_error(std::string(context_name) + " did not expose required provider snapshot");
	if (!context.LastError().empty())
		throw std::runtime_error(std::string(context_name) + " left stale last_error after success");
}

void CheckVideoOpen(aegisub::core_api::Context& context) {
	aegisub::core_api::VideoOpenOptions options;
	options.path = "?dummy:24:2:16:8:10:20:30:";
	options.preferred_provider = "Dummy";

	std::unique_ptr<aegisub::core_api::VideoSession> session;
	RequireStatus(aegisub::core_api::OpenVideo(context, options, session),
		aegisub::core_api::Status::Ok,
		"dummy video open");
	if (!session)
		throw std::runtime_error("dummy video open returned null session");

	auto const& info = session->Info();
	if (info.decoder_name != "Dummy Video Provider"
		|| info.selected_provider != "Dummy"
		|| info.frame_count != 2
		|| info.width != 16
		|| info.height != 8
		|| info.fps != 24.0
		|| info.should_set_video_properties)
		throw std::runtime_error("dummy video info snapshot was not stable");
	if (!context.LastError().empty())
		throw std::runtime_error("dummy video open left stale last_error after success");

	auto const frame = session->FrameAt(0);
	if (frame.width != 16
		|| frame.height != 8
		|| frame.pitch != 64
		|| frame.data.size() != 512
		|| frame.flipped
		|| frame.data[0] != 30
		|| frame.data[1] != 20
		|| frame.data[2] != 10
		|| frame.data[3] != 255)
		throw std::runtime_error("dummy video frame snapshot did not return expected BGRA bytes");
	try {
		(void)session->FrameAt(info.frame_count);
		throw std::runtime_error("out-of-range video frame unexpectedly succeeded");
	}
	catch (std::invalid_argument const&) {
	}

	aegisub::core_api::ProviderOpenReportSnapshot report;
	RequireStatus(aegisub::core_api::GetLastProviderOpenReport(context, aegisub::core_api::ProviderKind::Video, report),
		aegisub::core_api::Status::Ok,
		"dummy video provider open report");
	if (report.preferred_provider != "Dummy"
		|| report.selected_provider != "Dummy"
		|| report.attempts.empty()
		|| report.attempts.back().provider_name != "Dummy"
		|| report.attempts.back().outcome != "opened")
		throw std::runtime_error("dummy video provider open report was not stable");
}

void CheckAudioOpen(aegisub::core_api::Context& context) {
	aegisub::core_api::AudioOpenOptions options;
	options.path = "dummy-audio:";
	options.preferred_provider = "Dummy";

	std::unique_ptr<aegisub::core_api::AudioSession> session;
	RequireStatus(aegisub::core_api::OpenAudio(context, options, session),
		aegisub::core_api::Status::Ok,
		"dummy audio open");
	if (!session)
		throw std::runtime_error("dummy audio open returned null session");

	auto const& info = session->Info();
	if (info.selected_provider != "Dummy"
		|| info.num_samples != 396900000
		|| info.decoded_samples != 396900000
		|| info.sample_rate != 44100
		|| info.bytes_per_sample != 2
		|| info.channels != 1
		|| info.float_samples
		|| info.source_needs_cache)
		throw std::runtime_error("dummy audio info snapshot was not stable");
	if (!context.LastError().empty())
		throw std::runtime_error("dummy audio open left stale last_error after success");

	aegisub::core_api::ProviderOpenReportSnapshot report;
	RequireStatus(aegisub::core_api::GetLastProviderOpenReport(context, aegisub::core_api::ProviderKind::Audio, report),
		aegisub::core_api::Status::Ok,
		"dummy audio provider open report");
	if (report.preferred_provider != "Dummy"
		|| report.selected_provider != "Dummy"
		|| report.attempts.empty()
		|| report.attempts.back().provider_name != "Dummy"
		|| report.attempts.back().outcome != "opened")
		throw std::runtime_error("dummy audio provider open report was not stable");
}

void CheckSubtitleOpen(aegisub::core_api::Context& context) {
	ScopedFile ass_path(MakeTempAssPath());
	WriteSmokeAss(ass_path.get());

	aegisub::core_api::SubtitleOpenOptions options;
	options.path = ass_path.get().string();
	options.encoding = "utf-8";

	std::unique_ptr<aegisub::core_api::SubtitleSession> session;
	RequireStatus(aegisub::core_api::OpenSubtitles(context, options, session),
		aegisub::core_api::Status::Ok,
		"subtitle open");
	if (!session)
		throw std::runtime_error("subtitle open returned null session");

	auto const& info = session->Info();
	if (info.format_name != "Advanced SubStation Alpha"
		|| info.row_count != 2
		|| info.style_count != 1
		|| info.width != 1280
		|| info.height != 720
		|| session->RowCount() != 2)
		throw std::runtime_error("subtitle info snapshot was not stable");
	auto state = session->State();
	if (state.revision != 0
		|| state.row_change_revision != 0
		|| state.row_change_first_row != 0
		|| state.row_change_row_count != 0
		|| state.row_change_fields != ChangeField(aegisub::core_api::SubtitleChangeFields::None)
		|| state.dirty)
		throw std::runtime_error("new subtitle session state was not clean");

	auto const& first = session->RowAt(0);
	auto const& second = session->RowAt(1);
	if (first.row_index != 0
		|| first.comment
		|| first.layer != 0
		|| first.start_ms != 1000
		|| first.end_ms != 2500
		|| first.margin_left != 10
		|| first.margin_right != 20
		|| first.margin_vertical != 30
		|| first.actor != "Actor"
		|| first.text != "Hello core"
		|| !second.comment
		|| second.layer != 1
		|| second.effect != "fx"
		|| second.text != "Hidden note")
		throw std::runtime_error("subtitle row snapshots were not stable");
	if (!context.LastError().empty())
		throw std::runtime_error("subtitle open left stale last_error after success");

	session->SetRowText(0, "Edited from facade");
	state = session->State();
	if (state.revision != 1
		|| state.row_change_revision != 1
		|| state.row_change_first_row != 0
		|| state.row_change_row_count != 1
		|| state.row_change_fields != ChangeField(aegisub::core_api::SubtitleChangeFields::Text)
		|| !state.dirty)
		throw std::runtime_error("subtitle state did not track single row text edit");
	auto const& edited = session->RowAt(0);
	if (edited.text != "Edited from facade"
		|| edited.row_index != 0
		|| session->RowCount() != 2
		|| session->Info().row_count != 2)
		throw std::runtime_error("subtitle row text edit was not reflected in snapshots");
	session->SetRowText(0, "Edited from facade");
	state = session->State();
	if (state.revision != 1
		|| state.row_change_revision != 1
		|| state.row_change_first_row != 0
		|| state.row_change_row_count != 1
		|| state.row_change_fields != ChangeField(aegisub::core_api::SubtitleChangeFields::Text)
		|| !state.dirty)
		throw std::runtime_error("subtitle same text edit changed state");

	auto applied_texts = session->SetRowTexts({
		{0, "Batch edit zero"},
		{1, "Batch edit one"},
	});
	if (applied_texts != 2)
		throw std::runtime_error("subtitle batch row text edit reported wrong applied count");
	state = session->State();
	if (state.revision != 2
		|| state.row_change_revision != 2
		|| state.row_change_first_row != 0
		|| state.row_change_row_count != 2
		|| state.row_change_fields != ChangeField(aegisub::core_api::SubtitleChangeFields::Text)
		|| !state.dirty)
		throw std::runtime_error("subtitle state did not track batch row text edit");
	if (session->RowAt(0).text != "Batch edit zero"
		|| session->RowAt(1).text != "Batch edit one")
		throw std::runtime_error("subtitle batch row text edit did not update both rows");
	applied_texts = session->SetRowTexts({
		{0, "Batch edit zero"},
		{1, "Batch edit one"},
	});
	if (applied_texts != 0)
		throw std::runtime_error("subtitle same batch row text edit reported applied rows");
	state = session->State();
	if (state.revision != 2
		|| state.row_change_revision != 2
		|| state.row_change_first_row != 0
		|| state.row_change_row_count != 2
		|| state.row_change_fields != ChangeField(aegisub::core_api::SubtitleChangeFields::Text)
		|| !state.dirty)
		throw std::runtime_error("subtitle same batch row text edit changed state");
	try {
		session->SetRowTexts({
			{0, "Should not apply"},
			{session->RowCount(), "Invalid row"},
		});
		throw std::runtime_error("subtitle invalid batch row text edit did not throw");
	}
	catch (std::invalid_argument const&) {
	}
	if (session->RowAt(0).text != "Batch edit zero"
		|| session->RowAt(1).text != "Batch edit one")
		throw std::runtime_error("subtitle invalid batch row text edit was not transactional");
	state = session->State();
	if (state.revision != 2
		|| state.row_change_revision != 2
		|| state.row_change_first_row != 0
		|| state.row_change_row_count != 2
		|| state.row_change_fields != ChangeField(aegisub::core_api::SubtitleChangeFields::Text)
		|| !state.dirty)
		throw std::runtime_error("subtitle invalid batch row text edit changed state");
	try {
		session->SetRowTexts({
			{0, "Duplicate row first"},
			{0, "Duplicate row second"},
		});
		throw std::runtime_error("subtitle duplicate batch row text edit did not throw");
	}
	catch (std::invalid_argument const&) {
	}
	if (session->RowAt(0).text != "Batch edit zero"
		|| session->RowAt(1).text != "Batch edit one")
		throw std::runtime_error("subtitle duplicate batch row text edit was not transactional");
	state = session->State();
	if (state.revision != 2
		|| state.row_change_revision != 2
		|| state.row_change_first_row != 0
		|| state.row_change_row_count != 2
		|| state.row_change_fields != ChangeField(aegisub::core_api::SubtitleChangeFields::Text)
		|| !state.dirty)
		throw std::runtime_error("subtitle duplicate batch row text edit changed state");

	auto applied_patches = session->ApplyRowPatches({
		{0},
	});
	if (applied_patches != 0)
		throw std::runtime_error("subtitle zero-field row patch reported applied patches");
	state = session->State();
	if (state.revision != 2
		|| state.row_change_revision != 2
		|| state.row_change_first_row != 0
		|| state.row_change_row_count != 2
		|| state.row_change_fields != ChangeField(aegisub::core_api::SubtitleChangeFields::Text)
		|| !state.dirty)
		throw std::runtime_error("subtitle zero-field row patch changed state");
	try {
		session->ApplyRowPatches({
			{
				0,
				static_cast<std::uint32_t>(aegisub::core_api::SubtitleRowPatchFields::Text),
				false,
				0,
				0,
				0,
				0,
				0,
				0,
				{},
				{},
				{},
				"Duplicate patch first",
			},
			{
				0,
				static_cast<std::uint32_t>(aegisub::core_api::SubtitleRowPatchFields::Text),
				false,
				0,
				0,
				0,
				0,
				0,
				0,
				{},
				{},
				{},
				"Duplicate patch second",
			},
		});
		throw std::runtime_error("subtitle duplicate row patch did not throw");
	}
	catch (std::invalid_argument const&) {
	}
	if (session->RowAt(0).text != "Batch edit zero")
		throw std::runtime_error("subtitle duplicate row patch was not transactional");
	state = session->State();
	if (state.revision != 2
		|| state.row_change_revision != 2
		|| state.row_change_first_row != 0
		|| state.row_change_row_count != 2
		|| state.row_change_fields != ChangeField(aegisub::core_api::SubtitleChangeFields::Text)
		|| !state.dirty)
		throw std::runtime_error("subtitle duplicate row patch changed state");

	applied_patches = session->ApplyRowPatches({
		{
			0,
			static_cast<std::uint32_t>(aegisub::core_api::SubtitleRowPatchFields::Start)
				| static_cast<std::uint32_t>(aegisub::core_api::SubtitleRowPatchFields::End)
				| static_cast<std::uint32_t>(aegisub::core_api::SubtitleRowPatchFields::Text),
			false,
			0,
			1200,
			2800,
			0,
			0,
			0,
			{},
			{},
			{},
			"Patch edit zero",
		},
		{
			1,
			static_cast<std::uint32_t>(aegisub::core_api::SubtitleRowPatchFields::Comment)
				| static_cast<std::uint32_t>(aegisub::core_api::SubtitleRowPatchFields::Layer)
				| static_cast<std::uint32_t>(aegisub::core_api::SubtitleRowPatchFields::Margins)
				| static_cast<std::uint32_t>(aegisub::core_api::SubtitleRowPatchFields::Effect)
				| static_cast<std::uint32_t>(aegisub::core_api::SubtitleRowPatchFields::Text),
			false,
			3,
			0,
			0,
			11,
			22,
			33,
			{},
			{},
			"patched-effect",
			"Patch edit one",
		},
	});
	if (applied_patches != 2)
		throw std::runtime_error("subtitle row patch reported the wrong applied count");
	state = session->State();
	if (state.revision != 3
		|| state.row_change_revision != 3
		|| state.row_change_first_row != 0
		|| state.row_change_row_count != 2
		|| state.row_change_fields != (ChangeField(aegisub::core_api::SubtitleChangeFields::Text)
			| ChangeField(aegisub::core_api::SubtitleChangeFields::Time)
			| ChangeField(aegisub::core_api::SubtitleChangeFields::Metadata))
		|| !state.dirty)
		throw std::runtime_error("subtitle state did not track row patch edit");
	if (session->RowAt(0).start_ms != 1200
		|| session->RowAt(0).end_ms != 2800
		|| session->RowAt(0).text != "Patch edit zero"
		|| session->RowAt(1).comment
		|| session->RowAt(1).layer != 3
		|| session->RowAt(1).margin_left != 11
		|| session->RowAt(1).margin_right != 22
		|| session->RowAt(1).margin_vertical != 33
		|| session->RowAt(1).effect != "patched-effect"
		|| session->RowAt(1).text != "Patch edit one")
		throw std::runtime_error("subtitle row patch did not update requested fields");
	applied_patches = session->ApplyRowPatches({
		{
			0,
			static_cast<std::uint32_t>(aegisub::core_api::SubtitleRowPatchFields::Start)
				| static_cast<std::uint32_t>(aegisub::core_api::SubtitleRowPatchFields::End)
				| static_cast<std::uint32_t>(aegisub::core_api::SubtitleRowPatchFields::Text),
			false,
			0,
			1200,
			2800,
			0,
			0,
			0,
			{},
			{},
			{},
			"Patch edit zero",
		},
	});
	if (applied_patches != 0)
		throw std::runtime_error("subtitle same row patch reported applied patches");
	state = session->State();
	if (state.revision != 3
		|| state.row_change_revision != 3
		|| state.row_change_first_row != 0
		|| state.row_change_row_count != 2
		|| state.row_change_fields != (ChangeField(aegisub::core_api::SubtitleChangeFields::Text)
			| ChangeField(aegisub::core_api::SubtitleChangeFields::Time)
			| ChangeField(aegisub::core_api::SubtitleChangeFields::Metadata))
		|| !state.dirty)
		throw std::runtime_error("subtitle same row patch changed state");
	try {
		session->ApplyRowPatches({
			{
				0,
				static_cast<std::uint32_t>(aegisub::core_api::SubtitleRowPatchFields::Start),
				false,
				0,
				5000,
			},
		});
		throw std::runtime_error("subtitle invalid row patch did not throw");
	}
	catch (std::invalid_argument const&) {
	}
	if (session->RowAt(0).start_ms != 1200
		|| session->RowAt(0).text != "Patch edit zero")
		throw std::runtime_error("subtitle invalid row patch was not transactional");
	state = session->State();
	if (state.revision != 3
		|| state.row_change_revision != 3
		|| state.row_change_first_row != 0
		|| state.row_change_row_count != 2
		|| state.row_change_fields != (ChangeField(aegisub::core_api::SubtitleChangeFields::Text)
			| ChangeField(aegisub::core_api::SubtitleChangeFields::Time)
			| ChangeField(aegisub::core_api::SubtitleChangeFields::Metadata))
		|| !state.dirty)
		throw std::runtime_error("subtitle invalid row patch changed state");

	auto inserted_row = session->InsertRow(1);
	if (inserted_row != 1
		|| session->RowCount() != 3
		|| session->Info().row_count != 3
		|| session->RowAt(0).row_index != 0
		|| session->RowAt(1).row_index != 1
		|| session->RowAt(2).row_index != 2
		|| session->RowAt(1).text != "")
		throw std::runtime_error("subtitle row insert did not update row snapshots");
	state = session->State();
	if (state.revision != 4
		|| state.row_change_revision != 4
		|| state.row_change_first_row != 1
		|| state.row_change_row_count != 2
		|| state.row_change_fields != ChangeField(aegisub::core_api::SubtitleChangeFields::Structure)
		|| !state.dirty)
		throw std::runtime_error("subtitle state did not track row insert");
	try {
		session->DeleteRows(3, 1);
		throw std::runtime_error("subtitle invalid row delete did not throw");
	}
	catch (std::invalid_argument const&) {
	}
	state = session->State();
	if (state.revision != 4
		|| state.row_change_revision != 4
		|| state.row_change_first_row != 1
		|| state.row_change_row_count != 2
		|| state.row_change_fields != ChangeField(aegisub::core_api::SubtitleChangeFields::Structure)
		|| !state.dirty)
		throw std::runtime_error("subtitle invalid row delete changed state");
	session->DeleteRows(session->RowCount(), 0);
	state = session->State();
	if (state.revision != 4
		|| state.row_change_revision != 4
		|| state.row_change_first_row != 1
		|| state.row_change_row_count != 2
		|| state.row_change_fields != ChangeField(aegisub::core_api::SubtitleChangeFields::Structure)
		|| !state.dirty)
		throw std::runtime_error("subtitle tail zero row delete changed state");
	try {
		session->DeleteRows(session->RowCount() + 1, 0);
		throw std::runtime_error("subtitle out-of-range zero row delete did not throw");
	}
	catch (std::invalid_argument const&) {
	}
	state = session->State();
	if (state.revision != 4
		|| state.row_change_revision != 4
		|| state.row_change_first_row != 1
		|| state.row_change_row_count != 2
		|| state.row_change_fields != ChangeField(aegisub::core_api::SubtitleChangeFields::Structure)
		|| !state.dirty)
		throw std::runtime_error("subtitle out-of-range zero row delete changed state");
	session->DeleteRows(1, 1);
	if (session->RowCount() != 2
		|| session->Info().row_count != 2
		|| session->RowAt(0).text != "Patch edit zero"
		|| session->RowAt(1).text != "Patch edit one"
		|| session->RowAt(1).row_index != 1)
		throw std::runtime_error("subtitle row delete did not update row snapshots");
	state = session->State();
	if (state.revision != 5
		|| state.row_change_revision != 5
		|| state.row_change_first_row != 1
		|| state.row_change_row_count != 1
		|| state.row_change_fields != ChangeField(aegisub::core_api::SubtitleChangeFields::Structure)
		|| !state.dirty)
		throw std::runtime_error("subtitle state did not track row delete");

	ScopedFile saved_path(MakeTempAssPath());
	aegisub::core_api::SubtitleSaveOptions save_options;
	save_options.path = saved_path.get().string();
	{
		ScopedNullOptions null_options;
		RequireStatus(aegisub::core_api::SaveSubtitles(context, *session, save_options),
			aegisub::core_api::Status::Ok,
			"subtitle save without app options");
	}
	if (!context.LastError().empty())
		throw std::runtime_error("subtitle save left stale last_error after success");
	state = session->State();
	if (state.revision != 5
		|| state.row_change_revision != 5
		|| state.row_change_first_row != 1
		|| state.row_change_row_count != 1
		|| state.row_change_fields != ChangeField(aegisub::core_api::SubtitleChangeFields::Structure)
		|| state.dirty)
		throw std::runtime_error("subtitle save did not clear dirty state while preserving revision");

	aegisub::core_api::SubtitleOpenOptions reopen_options;
	reopen_options.path = saved_path.get().string();
	reopen_options.encoding = "utf-8";
	std::unique_ptr<aegisub::core_api::SubtitleSession> reopened;
	RequireStatus(aegisub::core_api::OpenSubtitles(context, reopen_options, reopened),
		aegisub::core_api::Status::Ok,
		"reopen saved subtitles");
	if (!reopened
		|| reopened->RowCount() != 2
		|| reopened->RowAt(0).start_ms != 1200
		|| reopened->RowAt(0).end_ms != 2800
		|| reopened->RowAt(0).text != "Patch edit zero"
		|| reopened->RowAt(1).layer != 3
		|| reopened->RowAt(1).effect != "patched-effect"
		|| reopened->RowAt(1).text != "Patch edit one")
		throw std::runtime_error("saved subtitle edits did not round trip through core facade");
	state = reopened->State();
	if (state.revision != 0
		|| state.row_change_revision != 0
		|| state.row_change_first_row != 0
		|| state.row_change_row_count != 0
		|| state.row_change_fields != ChangeField(aegisub::core_api::SubtitleChangeFields::None)
		|| state.dirty)
		throw std::runtime_error("reopened subtitle session state was not clean");

	try {
		(void)session->RowAt(session->RowCount());
		throw std::runtime_error("subtitle out-of-range row did not throw");
	}
	catch (std::out_of_range const&) {
	}

	// Regression: Subtitles must not return the last video/audio provider-open
	// report. Subtitle opens never go through a provider factory, so the report
	// must be an empty snapshot regardless of prior video/audio opens. Earlier
	// in this suite CheckVideoOpen/CheckAudioOpen populated those reports with
	// a non-empty selected_provider ("Dummy").
	aegisub::core_api::ProviderOpenReportSnapshot subtitle_report;
	RequireStatus(aegisub::core_api::GetLastProviderOpenReport(
			context, aegisub::core_api::ProviderKind::Subtitles, subtitle_report),
		aegisub::core_api::Status::Ok,
		"subtitle provider open report");
	if (subtitle_report.kind != aegisub::core_api::ProviderKind::Subtitles
		|| !subtitle_report.selected_provider.empty()
		|| !subtitle_report.preferred_provider.empty()
		|| !subtitle_report.attempts.empty())
		throw std::runtime_error("subtitle provider open report leaked video/audio provider data");
	// The Context-level accessor used by internal callers must agree with the
	// facade entry point and not fall through to the video report.
	auto const& direct_subtitle_report = context.LastProviderOpenReport(aegisub::core_api::ProviderKind::Subtitles);
	if (direct_subtitle_report.kind != aegisub::core_api::ProviderKind::Subtitles
		|| !direct_subtitle_report.selected_provider.empty()
		|| !direct_subtitle_report.attempts.empty())
		throw std::runtime_error("Context::LastProviderOpenReport(Subtitles) leaked video/audio provider data");
}

void CheckTextSubtitleOpenWithoutAppOptions(aegisub::core_api::Context& context) {
	ScopedFile txt_path(MakeTempTxtPath());
	WriteSmokeTxt(txt_path.get());

	aegisub::core_api::SubtitleOpenOptions options;
	options.path = txt_path.get().string();
	options.encoding = "utf-8";

	std::unique_ptr<aegisub::core_api::SubtitleSession> session;
	RequireStatus(aegisub::core_api::OpenSubtitles(context, options, session),
		aegisub::core_api::Status::Ok,
		"text subtitle open without app options");
	if (!session || session->RowCount() != 2)
		throw std::runtime_error("text subtitle open without app options returned wrong row count");
	if (session->RowAt(0).actor != "Alice" || session->RowAt(0).text != "Hello from TXT")
		throw std::runtime_error("text subtitle open without app options did not use default actor separator");
	if (!session->RowAt(1).comment || session->RowAt(1).text != "Internal note")
		throw std::runtime_error("text subtitle open without app options did not use default comment starter");
	if (!context.LastError().empty())
		throw std::runtime_error("text subtitle open without app options left stale last_error after success");
}

} // namespace

int main() {
	try {
		if (aegisub::core_api::AbiVersion != 16)
			throw std::runtime_error("unexpected ABI facade version");
		CheckStatusValuesMatchDraftCAbi();
		CheckContextCreationOptions();

		auto context = aegisub::core_api::CreateContext();
		if (!context)
			throw std::runtime_error("core API facade context creation failed");
		if (aegisub::core_api::IsProviderRegistryFinalized(*context))
			throw std::runtime_error("provider registry was finalized before host startup completed");
		RequireStatus(aegisub::core_api::RegisterBuiltinProviderFactories(*context),
			aegisub::core_api::Status::Ok,
			"builtin provider factory registration");

		RequireCatalog(*context, aegisub::core_api::ProviderKind::Audio, "Dummy", "Dummy", true, "audio provider catalog");
		RequireCatalog(*context, aegisub::core_api::ProviderKind::Video, "Dummy", "Dummy", true, "video provider catalog");
		RequireCatalog(*context, aegisub::core_api::ProviderKind::Subtitles, "libass", "libass", false, "subtitle provider catalog");
		CheckVideoOpen(*context);
		CheckAudioOpen(*context);
		CheckSubtitleOpen(*context);
		CheckTextSubtitleOpenWithoutAppOptions(*context);

		aegisub::core_api::ProviderCatalogSnapshot invalid_catalog;
		RequireStatus(aegisub::core_api::GetProviderCatalog(
			*context,
			static_cast<aegisub::core_api::ProviderKind>(255),
			{},
			invalid_catalog),
			aegisub::core_api::Status::InvalidArgument,
			"invalid provider kind");
		if (context->LastError().empty())
			throw std::runtime_error("invalid provider kind did not set last_error");

		RequireStatus(aegisub::core_api::FinalizeProviderRegistry(*context),
			aegisub::core_api::Status::Ok,
			"provider registry finalization");
		if (!aegisub::core_api::IsProviderRegistryFinalized(*context))
			throw std::runtime_error("provider registry did not report finalized after finalization");
		RequireStatus(aegisub::core_api::RegisterBuiltinProviderFactories(*context),
			aegisub::core_api::Status::RegistryFinalized,
			"post-finalize builtin provider factory registration");

		RequireCatalog(*context, aegisub::core_api::ProviderKind::Audio, "Dummy", "Dummy", true, "post-finalize audio provider catalog");
	}
	catch (std::exception const& err) {
		std::cerr << "core_api_facade_smoke failed: " << err.what() << "\n";
		return 1;
	}

	std::cout << "core_api_facade_smoke: context, last-error, provider catalogs, open diagnostics, audio/video/subtitle metadata sessions, and provider finalization are stable\n";
	return 0;
}
