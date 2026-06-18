#include <aegisub/core_c_api.h>

#include "core_api_facade.h"
#include "subtitle_grid_selection_policy.h"

#include <chrono>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

struct aegisub_core_context {
	std::unique_ptr<aegisub::core_api::Context> impl;
};

struct aegisub_core_provider_catalog {
	aegisub::core_api::ProviderCatalogSnapshot snapshot;
};

struct aegisub_core_provider_open_report {
	aegisub::core_api::ProviderOpenReportSnapshot snapshot;
};

struct aegisub_core_video_session {
	std::unique_ptr<aegisub::core_api::VideoSession> impl;
};

struct aegisub_core_audio_session {
	std::unique_ptr<aegisub::core_api::AudioSession> impl;
};

struct aegisub_core_subtitle_session {
	std::unique_ptr<aegisub::core_api::SubtitleSession> impl;
};

struct aegisub_core_grid_selection_plan {
	aegisub::subtitle_grid_selection_policy::SelectionPlan snapshot;
};

namespace {

aegisub_core_status ToCStatus(aegisub::core_api::Status status) {
	switch (status) {
	case aegisub::core_api::Status::Ok:
		return AEGISUB_CORE_STATUS_OK;
	case aegisub::core_api::Status::Cancelled:
		return AEGISUB_CORE_STATUS_CANCELLED;
	case aegisub::core_api::Status::InvalidArgument:
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;
	case aegisub::core_api::Status::FileNotFound:
		return AEGISUB_CORE_STATUS_FILE_NOT_FOUND;
	case aegisub::core_api::Status::FileSystemError:
		return AEGISUB_CORE_STATUS_FILE_SYSTEM_ERROR;
	case aegisub::core_api::Status::NotSupported:
		return AEGISUB_CORE_STATUS_NOT_SUPPORTED;
	case aegisub::core_api::Status::NoMedia:
		return AEGISUB_CORE_STATUS_NO_MEDIA;
	case aegisub::core_api::Status::RegistryFinalized:
		return AEGISUB_CORE_STATUS_REGISTRY_FINALIZED;
	case aegisub::core_api::Status::TimedOut:
		return AEGISUB_CORE_STATUS_TIMED_OUT;
	case aegisub::core_api::Status::Error:
		return AEGISUB_CORE_STATUS_ERROR;
	}
	return AEGISUB_CORE_STATUS_ERROR;
}

aegisub::core_api::ProviderKind ToProviderKind(aegisub_core_provider_kind kind) {
	switch (kind) {
	case AEGISUB_CORE_PROVIDER_AUDIO:
		return aegisub::core_api::ProviderKind::Audio;
	case AEGISUB_CORE_PROVIDER_VIDEO:
		return aegisub::core_api::ProviderKind::Video;
	case AEGISUB_CORE_PROVIDER_SUBTITLES:
		return aegisub::core_api::ProviderKind::Subtitles;
	}
	throw std::invalid_argument("unknown provider kind");
}

aegisub::core_api::SubtitleSortKey ToSubtitleSortKey(aegisub_core_subtitle_sort_key key) {
	switch (key) {
	case AEGISUB_CORE_SUBTITLE_SORT_START:
		return aegisub::core_api::SubtitleSortKey::Start;
	case AEGISUB_CORE_SUBTITLE_SORT_END:
		return aegisub::core_api::SubtitleSortKey::End;
	case AEGISUB_CORE_SUBTITLE_SORT_STYLE:
		return aegisub::core_api::SubtitleSortKey::Style;
	case AEGISUB_CORE_SUBTITLE_SORT_ACTOR:
		return aegisub::core_api::SubtitleSortKey::Actor;
	case AEGISUB_CORE_SUBTITLE_SORT_EFFECT:
		return aegisub::core_api::SubtitleSortKey::Effect;
	case AEGISUB_CORE_SUBTITLE_SORT_LAYER:
		return aegisub::core_api::SubtitleSortKey::Layer;
	}
	throw std::invalid_argument("unknown subtitle sort key");
}

aegisub_core_provider_kind ToCProviderKind(aegisub::core_api::ProviderKind kind) {
	switch (kind) {
	case aegisub::core_api::ProviderKind::Audio:
		return AEGISUB_CORE_PROVIDER_AUDIO;
	case aegisub::core_api::ProviderKind::Video:
		return AEGISUB_CORE_PROVIDER_VIDEO;
	case aegisub::core_api::ProviderKind::Subtitles:
		return AEGISUB_CORE_PROVIDER_SUBTITLES;
	}
	return AEGISUB_CORE_PROVIDER_VIDEO;
}

aegisub_core_string BorrowString(std::string const& value) {
	return {value.data(), value.size()};
}

aegisub_core_string BorrowStringView(std::string_view value) {
	return {value.data(), value.size()};
}

std::string_view ViewString(aegisub_core_string value) {
	if (!value.data && value.size != 0)
		throw std::invalid_argument("string view has null data with non-zero size");
	return {value.data ? value.data : "", value.size};
}

aegisub_core_owned_string CopyString(std::string_view value) {
	aegisub_core_owned_string result{};
	auto data = std::make_unique<char[]>(value.size() + 1);
	if (!value.empty())
		std::memcpy(data.get(), value.data(), value.size());
	data[value.size()] = '\0';
	result.data = data.release();
	result.size = value.size();
	return result;
}

void SetLastError(aegisub_core_context *context, std::string error) {
	if (context && context->impl)
		context->impl->SetLastError(std::move(error));
}

// Validates the struct_size field that follows abi_version on every
// options/input struct. See "Struct evolution" in core_c_api.h.
//
//   caller_struct_size == 0: legacy zero-initialized caller; accept and let the
//     caller read core's full known layout.
//   caller_struct_size < known_min_size: caller compiled against an older
//     header missing fields core needs; reject.
//   caller_struct_size >= known_min_size: accept (core reads only its known
//     fields and ignores any trailing bytes from a newer caller).
void ValidateStructSize(size_t caller_struct_size, size_t known_min_size, char const* struct_name) {
	if (caller_struct_size == 0)
		return;
	if (caller_struct_size < known_min_size) {
		throw std::invalid_argument(std::string(struct_name)
			+ " struct_size (" + std::to_string(caller_struct_size)
			+ ") is smaller than the minimum supported size ("
			+ std::to_string(known_min_size) + ")");
	}
}

void AEGISUB_CORE_CALL RunHostTask(void *task_userdata) {
	std::unique_ptr<aegisub::core::CoreHostTask> callback(static_cast<aegisub::core::CoreHostTask *>(task_userdata));
	(*callback)();
}

aegisub::core_api::ContextOptions ConvertOptions(aegisub_core_context_options const *options) {
	aegisub::core_api::ContextOptions converted;
	if (!options)
		return converted;

	ValidateStructSize(options->struct_size, sizeof(aegisub_core_context_options), "aegisub_core_context_options");
	converted.abi_version = options->abi_version;
	auto const& hooks = options->thread_hooks;
	auto const has_thread_hooks = hooks.post_to_main
		|| hooks.is_main_thread
		|| hooks.flush_main_jobs
		|| hooks.synchronous_invoke_timeout_ms != 0;
	if (!has_thread_hooks)
		return converted;
	if (!hooks.post_to_main)
		throw std::invalid_argument("host thread hooks require post_to_main");

	converted.use_thread_hooks = true;
	converted.thread_hooks.post_to_main = [hooks](aegisub::core::CoreHostTask task) {
		auto owned_task = std::make_unique<aegisub::core::CoreHostTask>(std::move(task));
		auto raw_task = owned_task.release();
		auto accepted = hooks.post_to_main(
			hooks.host_userdata,
			RunHostTask,
			raw_task);
		if (!accepted) {
			delete raw_task;
			throw std::runtime_error("host main-thread queue rejected task");
		}
	};
	if (hooks.is_main_thread) {
		converted.thread_hooks.is_main_thread = [hooks] {
			return hooks.is_main_thread(hooks.host_userdata) != 0;
		};
	}
	if (hooks.flush_main_jobs) {
		converted.thread_hooks.flush_main_jobs = [hooks] {
			return hooks.flush_main_jobs(hooks.host_userdata);
		};
	}
	if (hooks.synchronous_invoke_timeout_ms != 0)
		converted.thread_hooks.synchronous_invoke_timeout = std::chrono::milliseconds(hooks.synchronous_invoke_timeout_ms);
	return converted;
}

aegisub::core_api::VideoOpenOptions ConvertVideoOpenOptions(aegisub_core_video_open_options const& options) {
	if (options.abi_version != AEGISUB_CORE_ABI_VERSION)
		throw std::invalid_argument("unsupported video open options ABI version");
	ValidateStructSize(options.struct_size, sizeof(aegisub_core_video_open_options), "aegisub_core_video_open_options");
	if (options.max_cache_size_bytes > std::numeric_limits<size_t>::max())
		throw std::invalid_argument("video cache size exceeds host size_t");
	aegisub::core_api::VideoOpenOptions converted;
	converted.path = std::string(ViewString(options.path));
	converted.colormatrix = std::string(ViewString(options.colormatrix));
	converted.preferred_provider = std::string(ViewString(options.preferred_provider));
	if (options.max_cache_size_bytes != 0)
		converted.max_cache_size_bytes = static_cast<size_t>(options.max_cache_size_bytes);
	return converted;
}

aegisub::core_api::AudioOpenOptions ConvertAudioOpenOptions(aegisub_core_audio_open_options const& options) {
	if (options.abi_version != AEGISUB_CORE_ABI_VERSION)
		throw std::invalid_argument("unsupported audio open options ABI version");
	ValidateStructSize(options.struct_size, sizeof(aegisub_core_audio_open_options), "aegisub_core_audio_open_options");
	aegisub::core_api::AudioOpenOptions converted;
	converted.path = std::string(ViewString(options.path));
	converted.preferred_provider = std::string(ViewString(options.preferred_provider));
	return converted;
}

aegisub::core_api::SubtitleOpenOptions ConvertSubtitleOpenOptions(aegisub_core_subtitle_open_options const& options) {
	if (options.abi_version != AEGISUB_CORE_ABI_VERSION)
		throw std::invalid_argument("unsupported subtitle open options ABI version");
	ValidateStructSize(options.struct_size, sizeof(aegisub_core_subtitle_open_options), "aegisub_core_subtitle_open_options");
	aegisub::core_api::SubtitleOpenOptions converted;
	converted.path = std::string(ViewString(options.path));
	converted.encoding = std::string(ViewString(options.encoding));
	return converted;
}

aegisub::core_api::SubtitleSaveOptions ConvertSubtitleSaveOptions(aegisub_core_subtitle_save_options const& options) {
	if (options.abi_version != AEGISUB_CORE_ABI_VERSION)
		throw std::invalid_argument("unsupported subtitle save options ABI version");
	ValidateStructSize(options.struct_size, sizeof(aegisub_core_subtitle_save_options), "aegisub_core_subtitle_save_options");
	aegisub::core_api::SubtitleSaveOptions converted;
	converted.path = std::string(ViewString(options.path));
	converted.encoding = std::string(ViewString(options.encoding));
	return converted;
}

std::vector<aegisub::core_api::SubtitleRowTextEdit> ConvertSubtitleRowTextEdits(
	aegisub_core_subtitle_row_text_edit const *edits,
	size_t edit_count) {
	if (!edits && edit_count != 0)
		throw std::invalid_argument("subtitle row text edit array is null with non-zero size");

	std::vector<aegisub::core_api::SubtitleRowTextEdit> converted;
	converted.reserve(edit_count);
	for (size_t i = 0; i < edit_count; ++i) {
		converted.push_back({
			edits[i].row,
			std::string(ViewString(edits[i].text)),
		});
	}
	return converted;
}

std::vector<aegisub::core_api::SubtitleRowPatch> ConvertSubtitleRowPatches(
	aegisub_core_subtitle_row_patch const *patches,
	size_t patch_count) {
	if (!patches && patch_count != 0)
		throw std::invalid_argument("subtitle row patch array is null with non-zero size");

	constexpr auto known_fields =
		AEGISUB_CORE_SUBTITLE_ROW_PATCH_COMMENT
		| AEGISUB_CORE_SUBTITLE_ROW_PATCH_LAYER
		| AEGISUB_CORE_SUBTITLE_ROW_PATCH_START
		| AEGISUB_CORE_SUBTITLE_ROW_PATCH_END
		| AEGISUB_CORE_SUBTITLE_ROW_PATCH_MARGINS
		| AEGISUB_CORE_SUBTITLE_ROW_PATCH_STYLE
		| AEGISUB_CORE_SUBTITLE_ROW_PATCH_ACTOR
		| AEGISUB_CORE_SUBTITLE_ROW_PATCH_EFFECT
		| AEGISUB_CORE_SUBTITLE_ROW_PATCH_TEXT;

	std::vector<aegisub::core_api::SubtitleRowPatch> converted;
	converted.reserve(patch_count);
	for (size_t i = 0; i < patch_count; ++i) {
		auto const& source = patches[i];
		if ((source.fields & ~known_fields) != 0)
			throw std::invalid_argument("subtitle row patch contains unknown field bits");

		aegisub::core_api::SubtitleRowPatch patch;
		patch.row = source.row;
		patch.fields = source.fields;
		patch.comment = source.comment != 0;
		patch.layer = source.layer;
		patch.start_ms = source.start_ms;
		patch.end_ms = source.end_ms;
		patch.margin_left = source.margin_left;
		patch.margin_right = source.margin_right;
		patch.margin_vertical = source.margin_vertical;
		if ((source.fields & AEGISUB_CORE_SUBTITLE_ROW_PATCH_STYLE) != 0)
			patch.style = std::string(ViewString(source.style));
		if ((source.fields & AEGISUB_CORE_SUBTITLE_ROW_PATCH_ACTOR) != 0)
			patch.actor = std::string(ViewString(source.actor));
		if ((source.fields & AEGISUB_CORE_SUBTITLE_ROW_PATCH_EFFECT) != 0)
			patch.effect = std::string(ViewString(source.effect));
		if ((source.fields & AEGISUB_CORE_SUBTITLE_ROW_PATCH_TEXT) != 0)
			patch.text = std::string(ViewString(source.text));
		converted.push_back(std::move(patch));
	}
	return converted;
}

std::vector<aegisub::core_api::SubtitlePasteOverSource> ConvertSubtitlePasteOverSources(
	aegisub_core_subtitle_paste_over_source const *sources,
	size_t source_count,
	aegisub_core_subtitle_paste_over_fields fields) {
	if (!sources && source_count != 0)
		throw std::invalid_argument("subtitle paste-over source array is null with non-zero size");

	std::vector<aegisub::core_api::SubtitlePasteOverSource> converted;
	converted.reserve(source_count);
	for (size_t i = 0; i < source_count; ++i) {
		auto const& source = sources[i];
		aegisub::core_api::SubtitlePasteOverSource row;
		row.comment = source.comment != 0;
		row.layer = source.layer;
		row.start_ms = source.start_ms;
		row.end_ms = source.end_ms;
		row.margin_left = source.margin_left;
		row.margin_right = source.margin_right;
		row.margin_vertical = source.margin_vertical;
		if ((fields & AEGISUB_CORE_SUBTITLE_PASTE_OVER_STYLE) != 0)
			row.style = std::string(ViewString(source.style));
		if ((fields & AEGISUB_CORE_SUBTITLE_PASTE_OVER_ACTOR) != 0)
			row.actor = std::string(ViewString(source.actor));
		if ((fields & AEGISUB_CORE_SUBTITLE_PASTE_OVER_EFFECT) != 0)
			row.effect = std::string(ViewString(source.effect));
		if ((fields & AEGISUB_CORE_SUBTITLE_PASTE_OVER_TEXT) != 0)
			row.text = std::string(ViewString(source.text));
		converted.push_back(std::move(row));
	}
	return converted;
}

std::vector<std::size_t> ConvertSubtitleRowSelection(
	int32_t const *selected_rows,
	size_t selected_row_count) {
	if (!selected_rows && selected_row_count != 0)
		throw std::invalid_argument("subtitle row move selection is null with non-zero size");

	std::vector<std::size_t> converted;
	converted.reserve(selected_row_count);
	for (size_t i = 0; i < selected_row_count; ++i) {
		if (selected_rows[i] < 0)
			throw std::invalid_argument("subtitle row move selection has a negative row");
		converted.push_back(static_cast<std::size_t>(selected_rows[i]));
	}
	return converted;
}

aegisub::subtitle_grid_selection_policy::ModifierState ConvertGridModifiers(
	aegisub_core_grid_modifier_state modifiers) {
	return {
		modifiers.shift != 0,
		modifiers.ctrl != 0,
		modifiers.alt != 0,
	};
}

std::vector<int> ConvertGridSelectedRows(int32_t const *rows, size_t count) {
	if (!rows && count != 0)
		throw std::invalid_argument("selected row array is null with non-zero size");

	std::vector<int> converted;
	converted.reserve(count);
	for (size_t i = 0; i < count; ++i)
		converted.push_back(static_cast<int>(rows[i]));
	return converted;
}

aegisub::subtitle_grid_selection_policy::MouseSelectionInput ConvertGridMouseSelectionInput(
	aegisub_core_grid_mouse_selection_input const& input) {
	if (input.abi_version != AEGISUB_CORE_ABI_VERSION)
		throw std::invalid_argument("unsupported grid mouse selection input ABI version");
	ValidateStructSize(input.struct_size, sizeof(aegisub_core_grid_mouse_selection_input), "aegisub_core_grid_mouse_selection_input");
	return {
		static_cast<int>(input.row_count),
		static_cast<int>(input.target_row),
		static_cast<int>(input.anchor_row),
		ConvertGridSelectedRows(input.selected_rows, input.selected_row_count),
		input.click != 0,
		input.double_click != 0,
		input.dragging != 0,
		ConvertGridModifiers(input.modifiers),
	};
}

aegisub::subtitle_grid_selection_policy::KeyboardSelectionInput ConvertGridKeyboardSelectionInput(
	aegisub_core_grid_keyboard_selection_input const& input) {
	if (input.abi_version != AEGISUB_CORE_ABI_VERSION)
		throw std::invalid_argument("unsupported grid keyboard selection input ABI version");
	ValidateStructSize(input.struct_size, sizeof(aegisub_core_grid_keyboard_selection_input), "aegisub_core_grid_keyboard_selection_input");
	return {
		static_cast<int>(input.row_count),
		static_cast<int>(input.active_row),
		static_cast<int>(input.anchor_row),
		ConvertGridSelectedRows(input.selected_rows, input.selected_row_count),
		static_cast<int>(input.direction),
		static_cast<int>(input.step),
		ConvertGridModifiers(input.modifiers),
	};
}

aegisub::subtitle_grid_selection_policy::RowInsertSelectionInput ConvertGridRowInsertSelectionInput(
	aegisub_core_grid_row_insert_selection_input const& input) {
	if (input.abi_version != AEGISUB_CORE_ABI_VERSION)
		throw std::invalid_argument("unsupported grid row insert selection input ABI version");
	ValidateStructSize(input.struct_size, sizeof(aegisub_core_grid_row_insert_selection_input), "aegisub_core_grid_row_insert_selection_input");
	return {
		static_cast<int>(input.row_count),
		static_cast<int>(input.inserted_row),
	};
}

aegisub::subtitle_grid_selection_policy::RowsDeleteSelectionInput ConvertGridRowsDeleteSelectionInput(
	aegisub_core_grid_rows_delete_selection_input const& input) {
	if (input.abi_version != AEGISUB_CORE_ABI_VERSION)
		throw std::invalid_argument("unsupported grid rows delete selection input ABI version");
	ValidateStructSize(input.struct_size, sizeof(aegisub_core_grid_rows_delete_selection_input), "aegisub_core_grid_rows_delete_selection_input");
	return {
		static_cast<int>(input.row_count_before),
		static_cast<int>(input.first_deleted_row),
		static_cast<int>(input.deleted_row_count),
	};
}

aegisub_core_video_info ToCVideoInfo(aegisub::core_api::VideoInfoSnapshot const& info) {
	return {
		static_cast<int32_t>(info.frame_count),
		static_cast<int32_t>(info.width),
		static_cast<int32_t>(info.height),
		info.dar,
		info.fps,
		static_cast<uint8_t>(info.is_vfr ? 1 : 0),
		static_cast<uint8_t>(info.has_audio ? 1 : 0),
		static_cast<uint8_t>(info.should_set_video_properties ? 1 : 0),
		static_cast<uint8_t>(info.wants_caching ? 1 : 0),
		BorrowString(info.selected_provider),
		BorrowString(info.decoder_name),
		BorrowString(info.color_space),
		BorrowString(info.real_color_space),
		BorrowString(info.warning),
		BorrowString(info.native_format_description),
		info.keyframes.size(),
	};
}

aegisub_core_video_frame_info ToCVideoFrameInfo(aegisub::core_api::VideoFrameSnapshot const& frame) {
	if (frame.width > static_cast<size_t>(std::numeric_limits<int32_t>::max())
		|| frame.height > static_cast<size_t>(std::numeric_limits<int32_t>::max()))
		throw std::invalid_argument("video frame dimensions exceed C ABI limits");

	return {
		static_cast<int32_t>(frame.width),
		static_cast<int32_t>(frame.height),
		frame.pitch,
		frame.data.size(),
		static_cast<uint8_t>(frame.flipped ? 1 : 0),
	};
}

aegisub_core_audio_info ToCAudioInfo(aegisub::core_api::AudioInfoSnapshot const& info) {
	return {
		info.num_samples,
		info.decoded_samples,
		static_cast<int32_t>(info.sample_rate),
		static_cast<int32_t>(info.bytes_per_sample),
		static_cast<int32_t>(info.channels),
		static_cast<uint8_t>(info.float_samples ? 1 : 0),
		static_cast<uint8_t>(info.source_needs_cache ? 1 : 0),
		static_cast<uint64_t>(info.logical_bytes),
		static_cast<uint64_t>(info.decoded_bytes),
		BorrowString(info.selected_provider),
		BorrowString(info.storage_kind),
	};
}

aegisub_core_subtitle_info ToCSubtitleInfo(aegisub::core_api::SubtitleInfoSnapshot const& info) {
	return {
		static_cast<int32_t>(info.row_count),
		static_cast<int32_t>(info.style_count),
		static_cast<int32_t>(info.attachment_count),
		static_cast<int32_t>(info.script_info_count),
		static_cast<int32_t>(info.width),
		static_cast<int32_t>(info.height),
		static_cast<int32_t>(info.resolution_type),
		BorrowString(info.format_name),
	};
}

aegisub_core_subtitle_state ToCSubtitleState(aegisub::core_api::SubtitleStateSnapshot const& state) {
	return {
		state.revision,
		state.row_change_revision,
		state.row_change_first_row,
		state.row_change_row_count,
		state.row_change_fields,
		static_cast<uint8_t>(state.dirty ? 1 : 0),
	};
}

aegisub_core_subtitle_row ToCSubtitleRow(aegisub::core_api::SubtitleRowSnapshot const& row) {
	return {
		static_cast<int32_t>(row.line_id),
		static_cast<int32_t>(row.row_index),
		static_cast<uint8_t>(row.comment ? 1 : 0),
		static_cast<int32_t>(row.layer),
		static_cast<int32_t>(row.start_ms),
		static_cast<int32_t>(row.end_ms),
		static_cast<int32_t>(row.margin_left),
		static_cast<int32_t>(row.margin_right),
		static_cast<int32_t>(row.margin_vertical),
		BorrowStringView(row.style),
		BorrowStringView(row.actor),
		BorrowStringView(row.effect),
		BorrowStringView(row.text),
	};
}

aegisub_core_grid_selection_plan_info ToCGridSelectionPlanInfo(
	aegisub::subtitle_grid_selection_policy::SelectionPlan const& plan) {
	return {
		static_cast<uint8_t>(plan.handled ? 1 : 0),
		static_cast<uint8_t>(plan.set_active ? 1 : 0),
		static_cast<uint8_t>(plan.set_selection ? 1 : 0),
		static_cast<uint8_t>(plan.activate_media ? 1 : 0),
		static_cast<uint8_t>(plan.make_active_visible ? 1 : 0),
		static_cast<int32_t>(plan.active_row),
		static_cast<int32_t>(plan.anchor_row),
		plan.selected_rows.size(),
	};
}

aegisub_core_provider_open_report_info ToCProviderOpenReportInfo(
	aegisub::core_api::ProviderOpenReportSnapshot const& report) {
	return {
		ToCProviderKind(report.kind),
		BorrowString(report.preferred_provider),
		BorrowString(report.selected_provider),
		report.attempts.size(),
	};
}

aegisub_core_provider_open_attempt ToCProviderOpenAttempt(
	aegisub::core_api::ProviderOpenAttemptSnapshot const& attempt) {
	return {
		BorrowString(attempt.provider_name),
		BorrowString(attempt.outcome),
		BorrowString(attempt.detail),
	};
}

} // namespace

uint32_t AEGISUB_CORE_CALL aegisub_core_abi_version(void) {
	return aegisub::core_api::AbiVersion;
}

void AEGISUB_CORE_CALL aegisub_core_free_string(aegisub_core_owned_string value) {
	delete[] value.data;
}

aegisub_core_status AEGISUB_CORE_CALL aegisub_core_context_create(aegisub_core_context **context) {
	return aegisub_core_context_create_with_options(nullptr, context);
}

aegisub_core_status AEGISUB_CORE_CALL aegisub_core_context_create_with_options(
	aegisub_core_context_options const *options,
	aegisub_core_context **context) {
	if (!context)
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;
	*context = nullptr;

	try {
		std::string error;
		std::unique_ptr<aegisub::core_api::Context> impl;
		auto status = aegisub::core_api::TryCreateContext(ConvertOptions(options), impl, &error);
		if (status != aegisub::core_api::Status::Ok)
			return ToCStatus(status);

		auto result = std::make_unique<aegisub_core_context>();
		result->impl = std::move(impl);
		*context = result.release();
		return AEGISUB_CORE_STATUS_OK;
	}
	catch (std::invalid_argument const&) {
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;
	}
	catch (...) {
		return AEGISUB_CORE_STATUS_ERROR;
	}
}

void AEGISUB_CORE_CALL aegisub_core_context_destroy(aegisub_core_context *context) {
	delete context;
}

aegisub_core_owned_string AEGISUB_CORE_CALL aegisub_core_context_last_error(aegisub_core_context *context) {
	try {
		if (!context || !context->impl)
			return CopyString({});
		return CopyString(context->impl->LastError());
	}
	catch (...) {
		return {};
	}
}

aegisub_core_status AEGISUB_CORE_CALL aegisub_core_register_builtin_provider_factories(aegisub_core_context *context) {
	if (!context || !context->impl)
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;
	return ToCStatus(aegisub::core_api::RegisterBuiltinProviderFactories(*context->impl));
}

aegisub_core_status AEGISUB_CORE_CALL aegisub_core_finalize_provider_registry(aegisub_core_context *context) {
	if (!context || !context->impl)
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;
	return ToCStatus(aegisub::core_api::FinalizeProviderRegistry(*context->impl));
}

uint8_t AEGISUB_CORE_CALL aegisub_core_provider_registry_is_finalized(aegisub_core_context *context) {
	if (!context || !context->impl)
		return 0;
	return aegisub::core_api::IsProviderRegistryFinalized(*context->impl) ? 1 : 0;
}

aegisub_core_status AEGISUB_CORE_CALL aegisub_core_provider_catalog_get(
	aegisub_core_context *context,
	aegisub_core_provider_kind kind,
	aegisub_core_string preferred_provider,
	aegisub_core_provider_catalog **catalog) {
	if (!catalog)
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;
	*catalog = nullptr;
	if (!context || !context->impl)
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;

	try {
		auto result = std::make_unique<aegisub_core_provider_catalog>();
		auto status = aegisub::core_api::GetProviderCatalog(
			*context->impl,
			ToProviderKind(kind),
			ViewString(preferred_provider),
			result->snapshot);
		if (status != aegisub::core_api::Status::Ok)
			return ToCStatus(status);

		*catalog = result.release();
		return AEGISUB_CORE_STATUS_OK;
	}
	catch (std::invalid_argument const& err) {
		SetLastError(context, err.what());
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;
	}
	catch (std::exception const& err) {
		SetLastError(context, err.what());
		return AEGISUB_CORE_STATUS_ERROR;
	}
	catch (...) {
		SetLastError(context, "unknown provider catalog error");
		return AEGISUB_CORE_STATUS_ERROR;
	}
}

size_t AEGISUB_CORE_CALL aegisub_core_provider_catalog_count(aegisub_core_provider_catalog const *catalog) {
	return catalog ? catalog->snapshot.providers.size() : 0;
}

aegisub_core_status AEGISUB_CORE_CALL aegisub_core_provider_catalog_get_at(
	aegisub_core_provider_catalog const *catalog,
	size_t index,
	aegisub_core_provider_descriptor *descriptor) {
	if (!catalog || !descriptor)
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;
	if (index >= catalog->snapshot.providers.size())
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;

	auto const& provider = catalog->snapshot.providers[index];
	*descriptor = {
		BorrowString(provider.name),
		BorrowString(provider.display_name),
		BorrowString(provider.unavailable_reason),
		static_cast<uint8_t>(provider.hidden ? 1 : 0),
		static_cast<uint8_t>(provider.available ? 1 : 0),
	};
	return AEGISUB_CORE_STATUS_OK;
}

aegisub_core_status AEGISUB_CORE_CALL aegisub_core_provider_catalog_descriptors_get(
	aegisub_core_provider_catalog const *catalog,
	size_t first_provider,
	size_t provider_count,
	aegisub_core_provider_descriptor *descriptors,
	size_t *written) {
	if (!written)
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;
	*written = 0;
	if (!catalog || (!descriptors && provider_count != 0))
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;

	auto const& providers = catalog->snapshot.providers;
	if (first_provider > providers.size())
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;

	auto const available = providers.size() - first_provider;
	auto const to_copy = provider_count < available ? provider_count : available;
	for (size_t i = 0; i < to_copy; ++i) {
		auto const& provider = providers[first_provider + i];
		descriptors[i] = {
			BorrowString(provider.name),
			BorrowString(provider.display_name),
			BorrowString(provider.unavailable_reason),
			static_cast<uint8_t>(provider.hidden ? 1 : 0),
			static_cast<uint8_t>(provider.available ? 1 : 0),
		};
	}
	*written = to_copy;
	return AEGISUB_CORE_STATUS_OK;
}

void AEGISUB_CORE_CALL aegisub_core_provider_catalog_destroy(aegisub_core_provider_catalog *catalog) {
	delete catalog;
}

aegisub_core_status AEGISUB_CORE_CALL aegisub_core_provider_open_report_get(
	aegisub_core_context *context,
	aegisub_core_provider_kind kind,
	aegisub_core_provider_open_report **report) {
	if (!report)
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;
	*report = nullptr;
	if (!context || !context->impl)
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;

	try {
		auto result = std::make_unique<aegisub_core_provider_open_report>();
		auto status = aegisub::core_api::GetLastProviderOpenReport(
			*context->impl,
			ToProviderKind(kind),
			result->snapshot);
		if (status != aegisub::core_api::Status::Ok)
			return ToCStatus(status);

		*report = result.release();
		return AEGISUB_CORE_STATUS_OK;
	}
	catch (std::invalid_argument const& err) {
		SetLastError(context, err.what());
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;
	}
	catch (std::exception const& err) {
		SetLastError(context, err.what());
		return AEGISUB_CORE_STATUS_ERROR;
	}
	catch (...) {
		SetLastError(context, "unknown provider open report C API error");
		return AEGISUB_CORE_STATUS_ERROR;
	}
}

aegisub_core_status AEGISUB_CORE_CALL aegisub_core_provider_open_report_info_get(
	aegisub_core_provider_open_report const *report,
	aegisub_core_provider_open_report_info *info) {
	if (!report || !info)
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;
	*info = ToCProviderOpenReportInfo(report->snapshot);
	return AEGISUB_CORE_STATUS_OK;
}

size_t AEGISUB_CORE_CALL aegisub_core_provider_open_report_attempt_count(
	aegisub_core_provider_open_report const *report) {
	return report ? report->snapshot.attempts.size() : 0;
}

aegisub_core_status AEGISUB_CORE_CALL aegisub_core_provider_open_report_attempt_get_at(
	aegisub_core_provider_open_report const *report,
	size_t index,
	aegisub_core_provider_open_attempt *attempt) {
	if (!report || !attempt)
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;
	if (index >= report->snapshot.attempts.size())
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;
	*attempt = ToCProviderOpenAttempt(report->snapshot.attempts[index]);
	return AEGISUB_CORE_STATUS_OK;
}

aegisub_core_status AEGISUB_CORE_CALL aegisub_core_provider_open_report_attempts_get(
	aegisub_core_provider_open_report const *report,
	size_t first_attempt,
	size_t attempt_count,
	aegisub_core_provider_open_attempt *attempts,
	size_t *written) {
	if (!written)
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;
	*written = 0;
	if (!report || (!attempts && attempt_count != 0))
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;

	auto const& source = report->snapshot.attempts;
	if (first_attempt > source.size())
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;

	auto const available = source.size() - first_attempt;
	auto const to_copy = attempt_count < available ? attempt_count : available;
	for (size_t i = 0; i < to_copy; ++i)
		attempts[i] = ToCProviderOpenAttempt(source[first_attempt + i]);
	*written = to_copy;
	return AEGISUB_CORE_STATUS_OK;
}

void AEGISUB_CORE_CALL aegisub_core_provider_open_report_destroy(
	aegisub_core_provider_open_report *report) {
	delete report;
}

aegisub_core_status AEGISUB_CORE_CALL aegisub_core_video_open(
	aegisub_core_context *context,
	aegisub_core_video_open_options const *options,
	aegisub_core_video_session **session) {
	if (!session)
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;
	*session = nullptr;
	if (!context || !context->impl || !options)
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;

	try {
		std::unique_ptr<aegisub::core_api::VideoSession> impl;
		auto status = aegisub::core_api::OpenVideo(*context->impl, ConvertVideoOpenOptions(*options), impl);
		if (status != aegisub::core_api::Status::Ok)
			return ToCStatus(status);

		auto result = std::make_unique<aegisub_core_video_session>();
		result->impl = std::move(impl);
		*session = result.release();
		return AEGISUB_CORE_STATUS_OK;
	}
	catch (std::invalid_argument const& err) {
		SetLastError(context, err.what());
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;
	}
	catch (std::exception const& err) {
		SetLastError(context, err.what());
		return AEGISUB_CORE_STATUS_ERROR;
	}
	catch (...) {
		SetLastError(context, "unknown video open C API error");
		return AEGISUB_CORE_STATUS_ERROR;
	}
}

void AEGISUB_CORE_CALL aegisub_core_video_destroy(aegisub_core_video_session *session) {
	delete session;
}

aegisub_core_status AEGISUB_CORE_CALL aegisub_core_video_info_get(
	aegisub_core_video_session const *session,
	aegisub_core_video_info *info) {
	if (!session || !session->impl || !info)
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;
	*info = ToCVideoInfo(session->impl->Info());
	return AEGISUB_CORE_STATUS_OK;
}

aegisub_core_status AEGISUB_CORE_CALL aegisub_core_video_frame_bgra_info_get(
	aegisub_core_video_session const *session,
	int32_t frame_number,
	aegisub_core_video_frame_info *info) {
	if (!session || !session->impl || !info)
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;

	try {
		auto frame = session->impl->FrameAt(frame_number);
		*info = ToCVideoFrameInfo(frame);
		return AEGISUB_CORE_STATUS_OK;
	}
	catch (std::invalid_argument const&) {
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;
	}
	catch (...) {
		return AEGISUB_CORE_STATUS_ERROR;
	}
}

aegisub_core_status AEGISUB_CORE_CALL aegisub_core_video_frame_bgra_get(
	aegisub_core_video_session const *session,
	int32_t frame_number,
	uint8_t *buffer,
	size_t buffer_size,
	aegisub_core_video_frame_info *info) {
	if (!session || !session->impl || !buffer || !info)
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;

	try {
		auto frame = session->impl->FrameAt(frame_number);
		auto frame_info = ToCVideoFrameInfo(frame);
		if (buffer_size < frame.data.size())
			return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;
		if (!frame.data.empty())
			std::memcpy(buffer, frame.data.data(), frame.data.size());
		*info = frame_info;
		return AEGISUB_CORE_STATUS_OK;
	}
	catch (std::invalid_argument const&) {
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;
	}
	catch (...) {
		return AEGISUB_CORE_STATUS_ERROR;
	}
}

size_t AEGISUB_CORE_CALL aegisub_core_video_keyframe_count(aegisub_core_video_session const *session) {
	if (!session || !session->impl)
		return 0;
	return session->impl->Info().keyframes.size();
}

aegisub_core_status AEGISUB_CORE_CALL aegisub_core_video_keyframe_get_at(
	aegisub_core_video_session const *session,
	size_t index,
	int32_t *keyframe) {
	if (!session || !session->impl || !keyframe)
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;
	auto const& keyframes = session->impl->Info().keyframes;
	if (index >= keyframes.size())
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;
	*keyframe = static_cast<int32_t>(keyframes[index]);
	return AEGISUB_CORE_STATUS_OK;
}

aegisub_core_status AEGISUB_CORE_CALL aegisub_core_video_keyframes_get(
	aegisub_core_video_session const *session,
	size_t first_keyframe,
	size_t keyframe_count,
	int32_t *keyframes,
	size_t *written) {
	if (!written)
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;
	*written = 0;
	if (!session || !session->impl || (!keyframes && keyframe_count != 0))
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;

	auto const& source = session->impl->Info().keyframes;
	if (first_keyframe > source.size())
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;

	auto const available = source.size() - first_keyframe;
	auto const to_copy = keyframe_count < available ? keyframe_count : available;
	for (size_t i = 0; i < to_copy; ++i)
		keyframes[i] = static_cast<int32_t>(source[first_keyframe + i]);
	*written = to_copy;
	return AEGISUB_CORE_STATUS_OK;
}

aegisub_core_status AEGISUB_CORE_CALL aegisub_core_audio_open(
	aegisub_core_context *context,
	aegisub_core_audio_open_options const *options,
	aegisub_core_audio_session **session) {
	if (!session)
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;
	*session = nullptr;
	if (!context || !context->impl || !options)
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;

	try {
		std::unique_ptr<aegisub::core_api::AudioSession> impl;
		auto status = aegisub::core_api::OpenAudio(*context->impl, ConvertAudioOpenOptions(*options), impl);
		if (status != aegisub::core_api::Status::Ok)
			return ToCStatus(status);

		auto result = std::make_unique<aegisub_core_audio_session>();
		result->impl = std::move(impl);
		*session = result.release();
		return AEGISUB_CORE_STATUS_OK;
	}
	catch (std::invalid_argument const& err) {
		SetLastError(context, err.what());
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;
	}
	catch (std::exception const& err) {
		SetLastError(context, err.what());
		return AEGISUB_CORE_STATUS_ERROR;
	}
	catch (...) {
		SetLastError(context, "unknown audio open C API error");
		return AEGISUB_CORE_STATUS_ERROR;
	}
}

void AEGISUB_CORE_CALL aegisub_core_audio_destroy(aegisub_core_audio_session *session) {
	delete session;
}

aegisub_core_status AEGISUB_CORE_CALL aegisub_core_audio_info_get(
	aegisub_core_audio_session const *session,
	aegisub_core_audio_info *info) {
	if (!session || !session->impl || !info)
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;
	*info = ToCAudioInfo(session->impl->Info());
	return AEGISUB_CORE_STATUS_OK;
}

aegisub_core_status AEGISUB_CORE_CALL aegisub_core_subtitle_open(
	aegisub_core_context *context,
	aegisub_core_subtitle_open_options const *options,
	aegisub_core_subtitle_session **session) {
	if (!session)
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;
	*session = nullptr;
	if (!context || !context->impl || !options)
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;

	try {
		std::unique_ptr<aegisub::core_api::SubtitleSession> impl;
		auto status = aegisub::core_api::OpenSubtitles(*context->impl, ConvertSubtitleOpenOptions(*options), impl);
		if (status != aegisub::core_api::Status::Ok)
			return ToCStatus(status);

		auto result = std::make_unique<aegisub_core_subtitle_session>();
		result->impl = std::move(impl);
		*session = result.release();
		return AEGISUB_CORE_STATUS_OK;
	}
	catch (std::invalid_argument const& err) {
		SetLastError(context, err.what());
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;
	}
	catch (std::exception const& err) {
		SetLastError(context, err.what());
		return AEGISUB_CORE_STATUS_ERROR;
	}
	catch (...) {
		SetLastError(context, "unknown subtitle open C API error");
		return AEGISUB_CORE_STATUS_ERROR;
	}
}

aegisub_core_status AEGISUB_CORE_CALL aegisub_core_subtitle_save(
	aegisub_core_context *context,
	aegisub_core_subtitle_session const *session,
	aegisub_core_subtitle_save_options const *options) {
	if (!context || !context->impl || !session || !session->impl || !options)
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;

	try {
		return ToCStatus(aegisub::core_api::SaveSubtitles(
			*context->impl,
			*session->impl,
			ConvertSubtitleSaveOptions(*options)));
	}
	catch (std::invalid_argument const& err) {
		SetLastError(context, err.what());
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;
	}
	catch (std::exception const& err) {
		SetLastError(context, err.what());
		return AEGISUB_CORE_STATUS_ERROR;
	}
	catch (...) {
		SetLastError(context, "unknown subtitle save C API error");
		return AEGISUB_CORE_STATUS_ERROR;
	}
}

void AEGISUB_CORE_CALL aegisub_core_subtitle_destroy(aegisub_core_subtitle_session *session) {
	delete session;
}

aegisub_core_status AEGISUB_CORE_CALL aegisub_core_subtitle_info_get(
	aegisub_core_subtitle_session const *session,
	aegisub_core_subtitle_info *info) {
	if (!session || !session->impl || !info)
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;
	*info = ToCSubtitleInfo(session->impl->Info());
	return AEGISUB_CORE_STATUS_OK;
}

aegisub_core_status AEGISUB_CORE_CALL aegisub_core_subtitle_state_get(
	aegisub_core_subtitle_session const *session,
	aegisub_core_subtitle_state *state) {
	if (!session || !session->impl || !state)
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;
	*state = ToCSubtitleState(session->impl->State());
	return AEGISUB_CORE_STATUS_OK;
}

size_t AEGISUB_CORE_CALL aegisub_core_subtitle_row_count(
	aegisub_core_subtitle_session const *session) {
	if (!session || !session->impl)
		return 0;
	return session->impl->RowCount();
}

aegisub_core_status AEGISUB_CORE_CALL aegisub_core_subtitle_row_get_at(
	aegisub_core_subtitle_session const *session,
	size_t index,
	aegisub_core_subtitle_row *row) {
	if (!session || !session->impl || !row)
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;
	if (index >= session->impl->RowCount())
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;
	*row = ToCSubtitleRow(session->impl->RowAt(index));
	return AEGISUB_CORE_STATUS_OK;
}

aegisub_core_status AEGISUB_CORE_CALL aegisub_core_subtitle_rows_get(
	aegisub_core_subtitle_session const *session,
	size_t first_row,
	size_t row_count,
	aegisub_core_subtitle_row *rows,
	size_t *written) {
	if (!written)
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;
	*written = 0;
	if (!session || !session->impl || (!rows && row_count != 0))
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;

	auto const total_rows = session->impl->RowCount();
	if (first_row > total_rows)
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;

	auto const available = total_rows - first_row;
	auto const to_copy = row_count < available ? row_count : available;
	for (size_t i = 0; i < to_copy; ++i)
		rows[i] = ToCSubtitleRow(session->impl->RowAt(first_row + i));
	*written = to_copy;
	return AEGISUB_CORE_STATUS_OK;
}

aegisub_core_status AEGISUB_CORE_CALL aegisub_core_subtitle_row_text_set(
	aegisub_core_subtitle_session *session,
	size_t row,
	aegisub_core_string text) {
	if (!session || !session->impl)
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;
	if (row >= session->impl->RowCount())
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;

	try {
		session->impl->SetRowText(row, ViewString(text));
		return AEGISUB_CORE_STATUS_OK;
	}
	catch (std::invalid_argument const&) {
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;
	}
	catch (...) {
		return AEGISUB_CORE_STATUS_ERROR;
	}
}

aegisub_core_status AEGISUB_CORE_CALL aegisub_core_subtitle_row_texts_set(
	aegisub_core_subtitle_session *session,
	aegisub_core_subtitle_row_text_edit const *edits,
	size_t edit_count,
	size_t *applied) {
	if (!applied)
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;
	*applied = 0;
	if (!session || !session->impl)
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;

	try {
		auto converted = ConvertSubtitleRowTextEdits(edits, edit_count);
		*applied = session->impl->SetRowTexts(converted);
		return AEGISUB_CORE_STATUS_OK;
	}
	catch (std::invalid_argument const&) {
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;
	}
	catch (...) {
		return AEGISUB_CORE_STATUS_ERROR;
	}
}

aegisub_core_status AEGISUB_CORE_CALL aegisub_core_subtitle_row_insert(
	aegisub_core_subtitle_session *session,
	size_t position,
	size_t *inserted_row) {
	if (!inserted_row)
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;
	*inserted_row = 0;
	if (!session || !session->impl)
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;

	try {
		*inserted_row = session->impl->InsertRow(position);
		return AEGISUB_CORE_STATUS_OK;
	}
	catch (std::invalid_argument const&) {
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;
	}
	catch (...) {
		return AEGISUB_CORE_STATUS_ERROR;
	}
}

aegisub_core_status AEGISUB_CORE_CALL aegisub_core_subtitle_rows_delete(
	aegisub_core_subtitle_session *session,
	size_t first_row,
	size_t row_count,
	size_t *deleted) {
	if (!deleted)
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;
	*deleted = 0;
	if (!session || !session->impl)
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;

	try {
		session->impl->DeleteRows(first_row, row_count);
		*deleted = row_count;
		return AEGISUB_CORE_STATUS_OK;
	}
	catch (std::invalid_argument const&) {
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;
	}
	catch (...) {
		return AEGISUB_CORE_STATUS_ERROR;
	}
}

aegisub_core_status AEGISUB_CORE_CALL aegisub_core_subtitle_selected_rows_move(
	aegisub_core_subtitle_session *session,
	int32_t const *selected_rows,
	size_t selected_row_count,
	int32_t direction,
	int32_t *moved_selected_rows,
	size_t *moved_selected_row_count) {
	if (!moved_selected_row_count)
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;
	*moved_selected_row_count = 0;
	if (!session || !session->impl || (!moved_selected_rows && selected_row_count != 0))
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;

	try {
		auto selection = ConvertSubtitleRowSelection(selected_rows, selected_row_count);
		std::vector<std::size_t> moved_rows;
		if (!session->impl->MoveSelectedRows(selection, direction, moved_rows))
			return AEGISUB_CORE_STATUS_OK;

		for (size_t i = 0; i < moved_rows.size(); ++i)
			moved_selected_rows[i] = static_cast<int32_t>(moved_rows[i]);
		*moved_selected_row_count = moved_rows.size();
		return AEGISUB_CORE_STATUS_OK;
	}
	catch (std::invalid_argument const&) {
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;
	}
	catch (...) {
		return AEGISUB_CORE_STATUS_ERROR;
	}
}

aegisub_core_status AEGISUB_CORE_CALL aegisub_core_subtitle_selected_rows_duplicate(
	aegisub_core_subtitle_session *session,
	int32_t const *selected_rows,
	size_t selected_row_count,
	int32_t *duplicated_rows,
	size_t *duplicated_row_count) {
	if (!duplicated_row_count)
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;
	*duplicated_row_count = 0;
	if (!session || !session->impl || (!duplicated_rows && selected_row_count != 0))
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;

	try {
		auto selection = ConvertSubtitleRowSelection(selected_rows, selected_row_count);
		std::vector<std::size_t> duplicated;
		if (!session->impl->DuplicateSelectedRows(selection, duplicated))
			return AEGISUB_CORE_STATUS_OK;

		for (size_t i = 0; i < duplicated.size(); ++i)
			duplicated_rows[i] = static_cast<int32_t>(duplicated[i]);
		*duplicated_row_count = duplicated.size();
		return AEGISUB_CORE_STATUS_OK;
	}
	catch (std::invalid_argument const&) {
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;
	}
	catch (...) {
		return AEGISUB_CORE_STATUS_ERROR;
	}
}

aegisub_core_status AEGISUB_CORE_CALL aegisub_core_subtitle_rows_sort(
	aegisub_core_subtitle_session *session,
	aegisub_core_subtitle_sort_key key,
	uint8_t selected_only,
	int32_t const *selected_rows,
	size_t selected_row_count,
	int32_t *sorted_selected_rows,
	size_t *sorted_selected_row_count) {
	if (!sorted_selected_row_count)
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;
	*sorted_selected_row_count = 0;
	auto const use_selection = selected_only != 0;
	if (!session || !session->impl)
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;
	if (use_selection && selected_row_count > 1 && !sorted_selected_rows)
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;

	try {
		auto selection = use_selection
			? ConvertSubtitleRowSelection(selected_rows, selected_row_count)
			: std::vector<std::size_t>{};
		std::vector<std::size_t> sorted_rows;
		if (!session->impl->SortRows(ToSubtitleSortKey(key), use_selection, selection, sorted_rows))
			return AEGISUB_CORE_STATUS_OK;

		for (size_t i = 0; i < sorted_rows.size(); ++i)
			sorted_selected_rows[i] = static_cast<int32_t>(sorted_rows[i]);
		*sorted_selected_row_count = sorted_rows.size();
		return AEGISUB_CORE_STATUS_OK;
	}
	catch (std::invalid_argument const&) {
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;
	}
	catch (...) {
		return AEGISUB_CORE_STATUS_ERROR;
	}
}

aegisub_core_status AEGISUB_CORE_CALL aegisub_core_subtitle_paste_over_apply(
	aegisub_core_subtitle_session *session,
	int32_t const *target_rows,
	aegisub_core_subtitle_paste_over_source const *sources,
	size_t row_count,
	aegisub_core_subtitle_paste_over_fields fields,
	size_t *applied) {
	if (!applied)
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;
	*applied = 0;
	if (!session || !session->impl)
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;

	try {
		auto targets = ConvertSubtitleRowSelection(target_rows, row_count);
		auto converted_sources = ConvertSubtitlePasteOverSources(sources, row_count, fields);
		*applied = session->impl->PasteOverRows(targets, converted_sources, fields);
		return AEGISUB_CORE_STATUS_OK;
	}
	catch (std::invalid_argument const&) {
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;
	}
	catch (...) {
		return AEGISUB_CORE_STATUS_ERROR;
	}
}

aegisub_core_status AEGISUB_CORE_CALL aegisub_core_subtitle_row_patches_apply(
	aegisub_core_subtitle_session *session,
	aegisub_core_subtitle_row_patch const *patches,
	size_t patch_count,
	size_t *applied) {
	if (!applied)
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;
	*applied = 0;
	if (!session || !session->impl)
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;

	try {
		auto converted = ConvertSubtitleRowPatches(patches, patch_count);
		*applied = session->impl->ApplyRowPatches(converted);
		return AEGISUB_CORE_STATUS_OK;
	}
	catch (std::invalid_argument const&) {
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;
	}
	catch (...) {
		return AEGISUB_CORE_STATUS_ERROR;
	}
}

aegisub_core_status AEGISUB_CORE_CALL aegisub_core_grid_selection_plan_mouse(
	aegisub_core_grid_mouse_selection_input const *input,
	aegisub_core_grid_selection_plan **plan) {
	if (!plan)
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;
	*plan = nullptr;
	if (!input)
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;

	try {
		auto result = std::make_unique<aegisub_core_grid_selection_plan>();
		result->snapshot = aegisub::subtitle_grid_selection_policy::PlanMouseSelection(
			ConvertGridMouseSelectionInput(*input));
		*plan = result.release();
		return AEGISUB_CORE_STATUS_OK;
	}
	catch (std::invalid_argument const&) {
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;
	}
	catch (...) {
		return AEGISUB_CORE_STATUS_ERROR;
	}
}

aegisub_core_status AEGISUB_CORE_CALL aegisub_core_grid_selection_plan_keyboard(
	aegisub_core_grid_keyboard_selection_input const *input,
	aegisub_core_grid_selection_plan **plan) {
	if (!plan)
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;
	*plan = nullptr;
	if (!input)
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;

	try {
		auto result = std::make_unique<aegisub_core_grid_selection_plan>();
		result->snapshot = aegisub::subtitle_grid_selection_policy::PlanKeyboardSelection(
			ConvertGridKeyboardSelectionInput(*input));
		*plan = result.release();
		return AEGISUB_CORE_STATUS_OK;
	}
	catch (std::invalid_argument const&) {
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;
	}
	catch (...) {
		return AEGISUB_CORE_STATUS_ERROR;
	}
}

aegisub_core_status AEGISUB_CORE_CALL aegisub_core_grid_selection_plan_row_insert(
	aegisub_core_grid_row_insert_selection_input const *input,
	aegisub_core_grid_selection_plan **plan) {
	if (!plan)
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;
	*plan = nullptr;
	if (!input)
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;

	try {
		auto snapshot = aegisub::subtitle_grid_selection_policy::PlanRowInsertSelection(
			ConvertGridRowInsertSelectionInput(*input));
		if (!snapshot.handled)
			return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;

		auto result = std::make_unique<aegisub_core_grid_selection_plan>();
		result->snapshot = std::move(snapshot);
		*plan = result.release();
		return AEGISUB_CORE_STATUS_OK;
	}
	catch (std::invalid_argument const&) {
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;
	}
	catch (...) {
		return AEGISUB_CORE_STATUS_ERROR;
	}
}

aegisub_core_status AEGISUB_CORE_CALL aegisub_core_grid_selection_plan_rows_delete(
	aegisub_core_grid_rows_delete_selection_input const *input,
	aegisub_core_grid_selection_plan **plan) {
	if (!plan)
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;
	*plan = nullptr;
	if (!input)
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;

	try {
		auto snapshot = aegisub::subtitle_grid_selection_policy::PlanRowsDeleteSelection(
			ConvertGridRowsDeleteSelectionInput(*input));
		if (!snapshot.handled)
			return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;

		auto result = std::make_unique<aegisub_core_grid_selection_plan>();
		result->snapshot = std::move(snapshot);
		*plan = result.release();
		return AEGISUB_CORE_STATUS_OK;
	}
	catch (std::invalid_argument const&) {
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;
	}
	catch (...) {
		return AEGISUB_CORE_STATUS_ERROR;
	}
}

aegisub_core_status AEGISUB_CORE_CALL aegisub_core_grid_selection_plan_info_get(
	aegisub_core_grid_selection_plan const *plan,
	aegisub_core_grid_selection_plan_info *info) {
	if (!plan || !info)
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;
	*info = ToCGridSelectionPlanInfo(plan->snapshot);
	return AEGISUB_CORE_STATUS_OK;
}

size_t AEGISUB_CORE_CALL aegisub_core_grid_selection_plan_selected_row_count(
	aegisub_core_grid_selection_plan const *plan) {
	return plan ? plan->snapshot.selected_rows.size() : 0;
}

aegisub_core_status AEGISUB_CORE_CALL aegisub_core_grid_selection_plan_selected_row_get_at(
	aegisub_core_grid_selection_plan const *plan,
	size_t index,
	int32_t *row) {
	if (!plan || !row)
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;
	if (index >= plan->snapshot.selected_rows.size())
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;
	*row = static_cast<int32_t>(plan->snapshot.selected_rows[index]);
	return AEGISUB_CORE_STATUS_OK;
}

aegisub_core_status AEGISUB_CORE_CALL aegisub_core_grid_selection_plan_selected_rows_get(
	aegisub_core_grid_selection_plan const *plan,
	size_t first_row,
	size_t row_count,
	int32_t *rows,
	size_t *written) {
	if (!written)
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;
	*written = 0;
	if (!plan || (!rows && row_count != 0))
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;

	auto const& source = plan->snapshot.selected_rows;
	if (first_row > source.size())
		return AEGISUB_CORE_STATUS_INVALID_ARGUMENT;

	auto const available = source.size() - first_row;
	auto const to_copy = row_count < available ? row_count : available;
	for (size_t i = 0; i < to_copy; ++i)
		rows[i] = static_cast<int32_t>(source[first_row + i]);
	*written = to_copy;
	return AEGISUB_CORE_STATUS_OK;
}

void AEGISUB_CORE_CALL aegisub_core_grid_selection_plan_destroy(
	aegisub_core_grid_selection_plan *plan) {
	delete plan;
}
