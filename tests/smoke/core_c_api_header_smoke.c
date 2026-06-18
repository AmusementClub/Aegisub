#include <aegisub/core_c_api.h>

static void AEGISUB_CORE_CALL smoke_host_task(void *task_userdata) {
	(void)task_userdata;
}

static uint8_t AEGISUB_CORE_CALL smoke_post_to_main(
	void *host_userdata,
	aegisub_core_host_task task,
	void *task_userdata) {
	(void)host_userdata;
	task(task_userdata);
	return 1;
}

static uint8_t AEGISUB_CORE_CALL smoke_is_main_thread(void *host_userdata) {
	(void)host_userdata;
	return 1;
}

static size_t AEGISUB_CORE_CALL smoke_flush_main_jobs(void *host_userdata) {
	(void)host_userdata;
	return 0;
}

enum {
	aegisub_core_c_header_status_ok_value = 1 / (AEGISUB_CORE_STATUS_OK == 0),
	aegisub_core_c_header_status_error_value = 1 / (AEGISUB_CORE_STATUS_ERROR == 100),
	aegisub_core_c_header_provider_video_value = 1 / (AEGISUB_CORE_PROVIDER_VIDEO == 1),
	aegisub_core_c_header_sort_style_value = 1 / (AEGISUB_CORE_SUBTITLE_SORT_STYLE == 2),
	aegisub_core_c_header_paste_over_text_value = 1 / (AEGISUB_CORE_SUBTITLE_PASTE_OVER_TEXT == (1u << 10)),
	aegisub_core_c_header_patch_text_value = 1 / (AEGISUB_CORE_SUBTITLE_ROW_PATCH_TEXT == (1u << 8)),
	aegisub_core_c_header_change_structure_value = 1 / (AEGISUB_CORE_SUBTITLE_CHANGE_STRUCTURE == (1u << 3)),
	aegisub_core_c_header_status_is_i32 = 1 / (sizeof(aegisub_core_status) == 4),
	aegisub_core_c_header_provider_kind_is_i32 = 1 / (sizeof(aegisub_core_provider_kind) == 4),
	aegisub_core_c_header_sort_key_is_i32 = 1 / (sizeof(aegisub_core_subtitle_sort_key) == 4),
	aegisub_core_c_header_paste_over_fields_is_u32 = 1 / (sizeof(aegisub_core_subtitle_paste_over_fields) == 4),
	aegisub_core_c_header_patch_fields_is_u32 = 1 / (sizeof(aegisub_core_subtitle_row_patch_fields) == 4),
	aegisub_core_c_header_change_fields_is_u32 = 1 / (sizeof(aegisub_core_subtitle_change_fields) == 4),
	aegisub_core_c_header_string_size_offset = 1 / (offsetof(aegisub_core_string, size) == sizeof(char *)),
	aegisub_core_c_header_owned_string_size_offset = 1 / (offsetof(aegisub_core_owned_string, size) == sizeof(char *)),
	aegisub_core_c_header_video_frame_pitch_offset =
		1 / (offsetof(aegisub_core_video_frame_info, pitch) == sizeof(int32_t) * 2),
	aegisub_core_c_header_video_frame_data_size_offset =
		1 / (offsetof(aegisub_core_video_frame_info, data_size) == sizeof(int32_t) * 2 + sizeof(size_t)),
	aegisub_core_c_header_video_frame_flipped_offset =
		1 / (offsetof(aegisub_core_video_frame_info, flipped) == sizeof(int32_t) * 2 + sizeof(size_t) * 2),
	aegisub_core_c_header_subtitle_state_revision_offset =
		1 / (offsetof(aegisub_core_subtitle_state, revision) == 0),
	aegisub_core_c_header_subtitle_state_row_revision_offset =
		1 / (offsetof(aegisub_core_subtitle_state, row_change_revision) == sizeof(uint64_t)),
	aegisub_core_c_header_subtitle_state_first_row_offset =
		1 / (offsetof(aegisub_core_subtitle_state, row_change_first_row) == sizeof(uint64_t) * 2),
	aegisub_core_c_header_subtitle_state_row_count_offset =
		1 / (offsetof(aegisub_core_subtitle_state, row_change_row_count) == sizeof(uint64_t) * 2 + sizeof(size_t)),
	aegisub_core_c_header_subtitle_state_fields_offset =
		1 / (offsetof(aegisub_core_subtitle_state, row_change_fields) == sizeof(uint64_t) * 2 + sizeof(size_t) * 2),
	aegisub_core_c_header_subtitle_state_dirty_offset =
		1 / (offsetof(aegisub_core_subtitle_state, dirty) == sizeof(uint64_t) * 2 + sizeof(size_t) * 2 + sizeof(aegisub_core_subtitle_change_fields))
};

int aegisub_core_c_header_smoke(void) {
	aegisub_core_string string_view;
	aegisub_core_owned_string owned_string;
	aegisub_core_provider_descriptor descriptor;
	aegisub_core_provider_open_report_info report_info;
	aegisub_core_provider_open_attempt report_attempt;
	aegisub_core_context_options options;
	aegisub_core_video_open_options video_options;
	aegisub_core_video_info video_info;
	aegisub_core_video_frame_info video_frame_info;
	aegisub_core_audio_open_options audio_options;
	aegisub_core_audio_info audio_info;
	aegisub_core_subtitle_open_options subtitle_options;
	aegisub_core_subtitle_save_options subtitle_save_options;
	aegisub_core_subtitle_info subtitle_info;
	aegisub_core_subtitle_state subtitle_state;
	aegisub_core_subtitle_row subtitle_row;
	aegisub_core_subtitle_row_text_edit subtitle_text_edit;
	aegisub_core_subtitle_row_patch subtitle_row_patch;
	aegisub_core_subtitle_paste_over_source subtitle_paste_over_source;
	aegisub_core_grid_modifier_state grid_modifiers;
	aegisub_core_grid_mouse_selection_input grid_mouse_input;
	aegisub_core_grid_keyboard_selection_input grid_keyboard_input;
	aegisub_core_grid_row_insert_selection_input grid_row_insert_input;
	aegisub_core_grid_rows_delete_selection_input grid_rows_delete_input;
	aegisub_core_grid_selection_plan_info grid_plan_info;
	uint32_t (AEGISUB_CORE_CALL *abi_version_fn)(void);
	aegisub_core_status (AEGISUB_CORE_CALL *create_context_fn)(aegisub_core_context **);
	aegisub_core_status (AEGISUB_CORE_CALL *register_builtin_provider_factories_fn)(aegisub_core_context *);
	aegisub_core_status (AEGISUB_CORE_CALL *provider_catalog_descriptors_get_fn)(
		aegisub_core_provider_catalog const *,
		size_t,
		size_t,
		aegisub_core_provider_descriptor *,
		size_t *);
	aegisub_core_status (AEGISUB_CORE_CALL *provider_report_attempts_get_fn)(
		aegisub_core_provider_open_report const *,
		size_t,
		size_t,
		aegisub_core_provider_open_attempt *,
		size_t *);
	aegisub_core_status (AEGISUB_CORE_CALL *video_open_fn)(
		aegisub_core_context *,
		aegisub_core_video_open_options const *,
		aegisub_core_video_session **);
	aegisub_core_status (AEGISUB_CORE_CALL *video_frame_bgra_info_get_fn)(
		aegisub_core_video_session const *,
		int32_t,
		aegisub_core_video_frame_info *);
	aegisub_core_status (AEGISUB_CORE_CALL *video_frame_bgra_get_fn)(
		aegisub_core_video_session const *,
		int32_t,
		uint8_t *,
		size_t,
		aegisub_core_video_frame_info *);
	aegisub_core_status (AEGISUB_CORE_CALL *video_keyframes_get_fn)(
		aegisub_core_video_session const *,
		size_t,
		size_t,
		int32_t *,
		size_t *);
	aegisub_core_status (AEGISUB_CORE_CALL *audio_open_fn)(
		aegisub_core_context *,
		aegisub_core_audio_open_options const *,
		aegisub_core_audio_session **);
	aegisub_core_status (AEGISUB_CORE_CALL *subtitle_open_fn)(
		aegisub_core_context *,
		aegisub_core_subtitle_open_options const *,
		aegisub_core_subtitle_session **);
	aegisub_core_status (AEGISUB_CORE_CALL *subtitle_save_fn)(
		aegisub_core_context *,
		aegisub_core_subtitle_session const *,
		aegisub_core_subtitle_save_options const *);
	aegisub_core_status (AEGISUB_CORE_CALL *subtitle_rows_get_fn)(
		aegisub_core_subtitle_session const *,
		size_t,
		size_t,
		aegisub_core_subtitle_row *,
		size_t *);
	aegisub_core_status (AEGISUB_CORE_CALL *subtitle_state_get_fn)(
		aegisub_core_subtitle_session const *,
		aegisub_core_subtitle_state *);
	aegisub_core_status (AEGISUB_CORE_CALL *subtitle_row_text_set_fn)(
		aegisub_core_subtitle_session *,
		size_t,
		aegisub_core_string);
	aegisub_core_status (AEGISUB_CORE_CALL *subtitle_row_texts_set_fn)(
		aegisub_core_subtitle_session *,
		aegisub_core_subtitle_row_text_edit const *,
		size_t,
		size_t *);
	aegisub_core_status (AEGISUB_CORE_CALL *subtitle_row_insert_fn)(
		aegisub_core_subtitle_session *,
		size_t,
		size_t *);
	aegisub_core_status (AEGISUB_CORE_CALL *subtitle_rows_delete_fn)(
		aegisub_core_subtitle_session *,
		size_t,
		size_t,
		size_t *);
	aegisub_core_status (AEGISUB_CORE_CALL *subtitle_selected_rows_move_fn)(
		aegisub_core_subtitle_session *,
		int32_t const *,
		size_t,
		int32_t,
		int32_t *,
		size_t *);
	aegisub_core_status (AEGISUB_CORE_CALL *subtitle_selected_rows_duplicate_fn)(
		aegisub_core_subtitle_session *,
		int32_t const *,
		size_t,
		int32_t *,
		size_t *);
	aegisub_core_status (AEGISUB_CORE_CALL *subtitle_rows_sort_fn)(
		aegisub_core_subtitle_session *,
		aegisub_core_subtitle_sort_key,
		uint8_t,
		int32_t const *,
		size_t,
		int32_t *,
		size_t *);
	aegisub_core_status (AEGISUB_CORE_CALL *subtitle_paste_over_apply_fn)(
		aegisub_core_subtitle_session *,
		int32_t const *,
		aegisub_core_subtitle_paste_over_source const *,
		size_t,
		aegisub_core_subtitle_paste_over_fields,
		size_t *);
	aegisub_core_status (AEGISUB_CORE_CALL *subtitle_row_patches_apply_fn)(
		aegisub_core_subtitle_session *,
		aegisub_core_subtitle_row_patch const *,
		size_t,
		size_t *);
	aegisub_core_status (AEGISUB_CORE_CALL *grid_mouse_plan_fn)(
		aegisub_core_grid_mouse_selection_input const *,
		aegisub_core_grid_selection_plan **);
	aegisub_core_status (AEGISUB_CORE_CALL *grid_keyboard_plan_fn)(
		aegisub_core_grid_keyboard_selection_input const *,
		aegisub_core_grid_selection_plan **);
	aegisub_core_status (AEGISUB_CORE_CALL *grid_row_insert_plan_fn)(
		aegisub_core_grid_row_insert_selection_input const *,
		aegisub_core_grid_selection_plan **);
	aegisub_core_status (AEGISUB_CORE_CALL *grid_rows_delete_plan_fn)(
		aegisub_core_grid_rows_delete_selection_input const *,
		aegisub_core_grid_selection_plan **);
	aegisub_core_status (AEGISUB_CORE_CALL *grid_selected_rows_get_fn)(
		aegisub_core_grid_selection_plan const *,
		size_t,
		size_t,
		int32_t *,
		size_t *);

	string_view.data = "x";
	string_view.size = 1;
	owned_string.data = 0;
	owned_string.size = 0;
	descriptor.name = string_view;
	descriptor.display_name = string_view;
	descriptor.unavailable_reason = string_view;
	descriptor.hidden = 0;
	descriptor.available = 1;
	report_info.kind = AEGISUB_CORE_PROVIDER_VIDEO;
	report_info.preferred_provider = string_view;
	report_info.selected_provider = string_view;
	report_info.attempt_count = 0;
	report_attempt.provider_name = string_view;
	report_attempt.outcome = string_view;
	report_attempt.detail = string_view;
	options.abi_version = AEGISUB_CORE_ABI_VERSION;
	options.thread_hooks.host_userdata = 0;
	options.thread_hooks.post_to_main = smoke_post_to_main;
	options.thread_hooks.is_main_thread = smoke_is_main_thread;
	options.thread_hooks.flush_main_jobs = smoke_flush_main_jobs;
	options.thread_hooks.synchronous_invoke_timeout_ms = 0;
	video_options.abi_version = AEGISUB_CORE_ABI_VERSION;
	video_options.path = string_view;
	video_options.colormatrix = string_view;
	video_options.preferred_provider = string_view;
	video_options.max_cache_size_bytes = 0;
	video_info.frame_count = 0;
	video_info.width = 0;
	video_info.height = 0;
	video_info.dar = 0;
	video_info.fps = 0;
	video_info.is_vfr = 0;
	video_info.has_audio = 0;
	video_info.should_set_video_properties = 1;
	video_info.wants_caching = 0;
	video_info.selected_provider = string_view;
	video_info.decoder_name = string_view;
	video_info.color_space = string_view;
	video_info.real_color_space = string_view;
	video_info.warning = string_view;
	video_info.native_format_description = string_view;
	video_info.keyframe_count = 0;
	video_frame_info.width = 0;
	video_frame_info.height = 0;
	video_frame_info.pitch = 0;
	video_frame_info.data_size = 0;
	video_frame_info.flipped = 0;
	audio_options.abi_version = AEGISUB_CORE_ABI_VERSION;
	audio_options.path = string_view;
	audio_options.preferred_provider = string_view;
	audio_info.num_samples = 0;
	audio_info.decoded_samples = 0;
	audio_info.sample_rate = 0;
	audio_info.bytes_per_sample = 0;
	audio_info.channels = 0;
	audio_info.float_samples = 0;
	audio_info.source_needs_cache = 0;
	audio_info.logical_bytes = 0;
	audio_info.decoded_bytes = 0;
	audio_info.selected_provider = string_view;
	audio_info.storage_kind = string_view;
	subtitle_options.abi_version = AEGISUB_CORE_ABI_VERSION;
	subtitle_options.path = string_view;
	subtitle_options.encoding = string_view;
	subtitle_save_options.abi_version = AEGISUB_CORE_ABI_VERSION;
	subtitle_save_options.path = string_view;
	subtitle_save_options.encoding = string_view;
	subtitle_info.row_count = 0;
	subtitle_info.style_count = 0;
	subtitle_info.attachment_count = 0;
	subtitle_info.script_info_count = 0;
	subtitle_info.width = 0;
	subtitle_info.height = 0;
	subtitle_info.resolution_type = 0;
	subtitle_info.format_name = string_view;
	subtitle_state.revision = 0;
	subtitle_state.row_change_revision = 0;
	subtitle_state.row_change_first_row = 0;
	subtitle_state.row_change_row_count = 0;
	subtitle_state.row_change_fields = AEGISUB_CORE_SUBTITLE_CHANGE_TEXT
		| AEGISUB_CORE_SUBTITLE_CHANGE_TIME
		| AEGISUB_CORE_SUBTITLE_CHANGE_METADATA
		| AEGISUB_CORE_SUBTITLE_CHANGE_STRUCTURE;
	subtitle_state.dirty = 0;
	subtitle_row.line_id = 0;
	subtitle_row.row_index = 0;
	subtitle_row.comment = 0;
	subtitle_row.layer = 0;
	subtitle_row.start_ms = 0;
	subtitle_row.end_ms = 0;
	subtitle_row.margin_left = 0;
	subtitle_row.margin_right = 0;
	subtitle_row.margin_vertical = 0;
	subtitle_row.style = string_view;
	subtitle_row.actor = string_view;
	subtitle_row.effect = string_view;
	subtitle_row.text = string_view;
	subtitle_text_edit.row = 0;
	subtitle_text_edit.text = string_view;
	subtitle_row_patch.row = 0;
	subtitle_row_patch.fields = AEGISUB_CORE_SUBTITLE_ROW_PATCH_START | AEGISUB_CORE_SUBTITLE_ROW_PATCH_TEXT;
	subtitle_row_patch.comment = 0;
	subtitle_row_patch.layer = 0;
	subtitle_row_patch.start_ms = 0;
	subtitle_row_patch.end_ms = 0;
	subtitle_row_patch.margin_left = 0;
	subtitle_row_patch.margin_right = 0;
	subtitle_row_patch.margin_vertical = 0;
	subtitle_row_patch.style = string_view;
	subtitle_row_patch.actor = string_view;
	subtitle_row_patch.effect = string_view;
	subtitle_row_patch.text = string_view;
	subtitle_paste_over_source.comment = 0;
	subtitle_paste_over_source.layer = 0;
	subtitle_paste_over_source.start_ms = 0;
	subtitle_paste_over_source.end_ms = 0;
	subtitle_paste_over_source.margin_left = 0;
	subtitle_paste_over_source.margin_right = 0;
	subtitle_paste_over_source.margin_vertical = 0;
	subtitle_paste_over_source.style = string_view;
	subtitle_paste_over_source.actor = string_view;
	subtitle_paste_over_source.effect = string_view;
	subtitle_paste_over_source.text = string_view;
	grid_modifiers.shift = 0;
	grid_modifiers.ctrl = 0;
	grid_modifiers.alt = 0;
	grid_mouse_input.abi_version = AEGISUB_CORE_ABI_VERSION;
	grid_mouse_input.row_count = 0;
	grid_mouse_input.target_row = 0;
	grid_mouse_input.anchor_row = 0;
	grid_mouse_input.selected_rows = 0;
	grid_mouse_input.selected_row_count = 0;
	grid_mouse_input.click = 1;
	grid_mouse_input.double_click = 0;
	grid_mouse_input.dragging = 0;
	grid_mouse_input.modifiers = grid_modifiers;
	grid_keyboard_input.abi_version = AEGISUB_CORE_ABI_VERSION;
	grid_keyboard_input.row_count = 0;
	grid_keyboard_input.active_row = 0;
	grid_keyboard_input.anchor_row = 0;
	grid_keyboard_input.selected_rows = 0;
	grid_keyboard_input.selected_row_count = 0;
	grid_keyboard_input.direction = 1;
	grid_keyboard_input.step = 1;
	grid_keyboard_input.modifiers = grid_modifiers;
	grid_row_insert_input.abi_version = AEGISUB_CORE_ABI_VERSION;
	grid_row_insert_input.row_count = 3;
	grid_row_insert_input.inserted_row = 1;
	grid_rows_delete_input.abi_version = AEGISUB_CORE_ABI_VERSION;
	grid_rows_delete_input.row_count_before = 3;
	grid_rows_delete_input.first_deleted_row = 1;
	grid_rows_delete_input.deleted_row_count = 1;
	grid_plan_info.handled = 0;
	grid_plan_info.set_active = 0;
	grid_plan_info.set_selection = 0;
	grid_plan_info.activate_media = 0;
	grid_plan_info.make_active_visible = 0;
	grid_plan_info.active_row = 0;
	grid_plan_info.anchor_row = 0;
	grid_plan_info.selected_row_count = 0;
	abi_version_fn = aegisub_core_abi_version;
	create_context_fn = aegisub_core_context_create;
	register_builtin_provider_factories_fn = aegisub_core_register_builtin_provider_factories;
	provider_catalog_descriptors_get_fn = aegisub_core_provider_catalog_descriptors_get;
	provider_report_attempts_get_fn = aegisub_core_provider_open_report_attempts_get;
	video_open_fn = aegisub_core_video_open;
	video_frame_bgra_info_get_fn = aegisub_core_video_frame_bgra_info_get;
	video_frame_bgra_get_fn = aegisub_core_video_frame_bgra_get;
	video_keyframes_get_fn = aegisub_core_video_keyframes_get;
	audio_open_fn = aegisub_core_audio_open;
	subtitle_open_fn = aegisub_core_subtitle_open;
	subtitle_save_fn = aegisub_core_subtitle_save;
	subtitle_rows_get_fn = aegisub_core_subtitle_rows_get;
	subtitle_state_get_fn = aegisub_core_subtitle_state_get;
	subtitle_row_text_set_fn = aegisub_core_subtitle_row_text_set;
	subtitle_row_texts_set_fn = aegisub_core_subtitle_row_texts_set;
	subtitle_row_insert_fn = aegisub_core_subtitle_row_insert;
	subtitle_rows_delete_fn = aegisub_core_subtitle_rows_delete;
	subtitle_selected_rows_move_fn = aegisub_core_subtitle_selected_rows_move;
	subtitle_selected_rows_duplicate_fn = aegisub_core_subtitle_selected_rows_duplicate;
	subtitle_rows_sort_fn = aegisub_core_subtitle_rows_sort;
	subtitle_paste_over_apply_fn = aegisub_core_subtitle_paste_over_apply;
	subtitle_row_patches_apply_fn = aegisub_core_subtitle_row_patches_apply;
	grid_mouse_plan_fn = aegisub_core_grid_selection_plan_mouse;
	grid_keyboard_plan_fn = aegisub_core_grid_selection_plan_keyboard;
	grid_row_insert_plan_fn = aegisub_core_grid_selection_plan_row_insert;
	grid_rows_delete_plan_fn = aegisub_core_grid_selection_plan_rows_delete;
	grid_selected_rows_get_fn = aegisub_core_grid_selection_plan_selected_rows_get;
	smoke_host_task(0);

	return descriptor.available == 1
		&& report_info.kind == AEGISUB_CORE_PROVIDER_VIDEO
		&& options.abi_version == AEGISUB_CORE_ABI_VERSION
		&& video_options.abi_version == AEGISUB_CORE_ABI_VERSION
		&& video_info.should_set_video_properties == 1
		&& video_frame_info.flipped == 0
		&& audio_options.abi_version == AEGISUB_CORE_ABI_VERSION
		&& audio_info.float_samples == 0
		&& subtitle_options.abi_version == AEGISUB_CORE_ABI_VERSION
		&& subtitle_save_options.abi_version == AEGISUB_CORE_ABI_VERSION
		&& subtitle_info.row_count == 0
		&& subtitle_state.revision == 0
		&& subtitle_state.row_change_revision == 0
		&& subtitle_state.row_change_first_row == 0
		&& subtitle_state.row_change_row_count == 0
		&& subtitle_state.row_change_fields == (AEGISUB_CORE_SUBTITLE_CHANGE_TEXT
			| AEGISUB_CORE_SUBTITLE_CHANGE_TIME
			| AEGISUB_CORE_SUBTITLE_CHANGE_METADATA
			| AEGISUB_CORE_SUBTITLE_CHANGE_STRUCTURE)
		&& subtitle_state.dirty == 0
		&& subtitle_row.comment == 0
		&& subtitle_text_edit.row == 0
		&& subtitle_row_patch.fields == (AEGISUB_CORE_SUBTITLE_ROW_PATCH_START | AEGISUB_CORE_SUBTITLE_ROW_PATCH_TEXT)
		&& subtitle_paste_over_source.comment == 0
		&& grid_mouse_input.click == 1
		&& grid_keyboard_input.direction == 1
		&& grid_row_insert_input.inserted_row == 1
		&& grid_rows_delete_input.deleted_row_count == 1
		&& grid_plan_info.handled == 0
		&& abi_version_fn == aegisub_core_abi_version
		&& create_context_fn == aegisub_core_context_create
		&& register_builtin_provider_factories_fn == aegisub_core_register_builtin_provider_factories
		&& provider_catalog_descriptors_get_fn == aegisub_core_provider_catalog_descriptors_get
		&& provider_report_attempts_get_fn == aegisub_core_provider_open_report_attempts_get
		&& video_open_fn == aegisub_core_video_open
		&& video_frame_bgra_info_get_fn == aegisub_core_video_frame_bgra_info_get
		&& video_frame_bgra_get_fn == aegisub_core_video_frame_bgra_get
		&& video_keyframes_get_fn == aegisub_core_video_keyframes_get
		&& audio_open_fn == aegisub_core_audio_open
		&& subtitle_open_fn == aegisub_core_subtitle_open
		&& subtitle_save_fn == aegisub_core_subtitle_save
		&& subtitle_rows_get_fn == aegisub_core_subtitle_rows_get
		&& subtitle_state_get_fn == aegisub_core_subtitle_state_get
		&& subtitle_row_text_set_fn == aegisub_core_subtitle_row_text_set
		&& subtitle_row_texts_set_fn == aegisub_core_subtitle_row_texts_set
		&& subtitle_row_insert_fn == aegisub_core_subtitle_row_insert
		&& subtitle_rows_delete_fn == aegisub_core_subtitle_rows_delete
		&& subtitle_selected_rows_move_fn == aegisub_core_subtitle_selected_rows_move
		&& subtitle_selected_rows_duplicate_fn == aegisub_core_subtitle_selected_rows_duplicate
		&& subtitle_rows_sort_fn == aegisub_core_subtitle_rows_sort
		&& subtitle_paste_over_apply_fn == aegisub_core_subtitle_paste_over_apply
		&& subtitle_row_patches_apply_fn == aegisub_core_subtitle_row_patches_apply
		&& grid_mouse_plan_fn == aegisub_core_grid_selection_plan_mouse
		&& grid_keyboard_plan_fn == aegisub_core_grid_selection_plan_keyboard
		&& grid_row_insert_plan_fn == aegisub_core_grid_selection_plan_row_insert
		&& grid_rows_delete_plan_fn == aegisub_core_grid_selection_plan_rows_delete
		&& grid_selected_rows_get_fn == aegisub_core_grid_selection_plan_selected_rows_get
		? 0
		: 1;
}
