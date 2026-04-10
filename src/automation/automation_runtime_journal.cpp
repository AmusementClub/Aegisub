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
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS; WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

#include "automation_runtime_journal.h"

#include "automation_json_utils.h"

#include <optional>
#include <string>
#include <utility>

namespace Automation4 {
namespace {
using namespace Automation4::json;

std::string SerializeInvocation(AutomationInvocation const& invocation) {
	JsonObjectBuilder builder;
	builder.AddRaw("kind", JsonString(ToString(invocation.kind)));
	builder.AddRaw("feature_name", JsonString(invocation.feature_name));
	JsonObjectBuilder capabilities;
	capabilities.AddRaw("allow_modify", JsonBool(invocation.capabilities.allow_modify));
	capabilities.AddRaw("allow_undo", JsonBool(invocation.capabilities.allow_undo));
	capabilities.AddRaw("allow_dialog", JsonBool(invocation.capabilities.allow_dialog));
	builder.AddRaw("capabilities", capabilities.Build());
	return builder.Build();
}

std::string SerializeContextSnapshot(AutomationContextSnapshot const& snapshot) {
	JsonObjectBuilder builder;

	JsonObjectBuilder selection;
	selection.AddRaw("selected_rows", SerializeIntArray(snapshot.selection.selected_rows));
	selection.AddRaw("active_row", JsonInteger(snapshot.selection.active_row));
	builder.AddRaw("selection", selection.Build());

	JsonObjectBuilder media;
	media.AddRaw("has_video", JsonBool(snapshot.media.has_video));
	media.AddRaw("has_audio", JsonBool(snapshot.media.has_audio));
	media.AddRaw("video_width", JsonInteger(snapshot.media.video_width));
	media.AddRaw("video_height", JsonInteger(snapshot.media.video_height));
	media.AddRaw("video_aspect_ratio", JsonDouble(snapshot.media.video_aspect_ratio));
	media.AddRaw("video_aspect_ratio_type", JsonInteger(snapshot.media.video_aspect_ratio_type));
	media.AddRaw("has_timecodes", JsonBool(snapshot.media.has_timecodes));
	media.AddRaw("has_keyframes", JsonBool(snapshot.media.has_keyframes));
	media.AddRaw("keyframes", SerializeIntArray(snapshot.media.keyframes));
	media.AddRaw("has_audio_selection", JsonBool(snapshot.media.has_audio_selection));
	media.AddRaw("audio_selection_begin", JsonInteger(snapshot.media.audio_selection_begin));
	media.AddRaw("audio_selection_end", JsonInteger(snapshot.media.audio_selection_end));
	builder.AddRaw("media", media.Build());

	JsonObjectBuilder project;
	project.AddRaw("script_filename", JsonString(snapshot.project.script_filename));
	project.AddRaw("subtitle_file", JsonString(snapshot.project.subtitle_file));
	project.AddRaw("automation_scripts", JsonString(snapshot.project.automation_scripts));
	project.AddRaw("export_filters", JsonString(snapshot.project.export_filters));
	project.AddRaw("export_encoding", JsonString(snapshot.project.export_encoding));
	project.AddRaw("style_storage", JsonString(snapshot.project.style_storage));
	project.AddRaw("audio_file", JsonString(snapshot.project.audio_file));
	project.AddRaw("video_file", JsonString(snapshot.project.video_file));
	project.AddRaw("timecodes_file", JsonString(snapshot.project.timecodes_file));
	project.AddRaw("keyframes_file", JsonString(snapshot.project.keyframes_file));
	project.AddRaw("play_res_x", JsonInteger(snapshot.project.play_res_x));
	project.AddRaw("play_res_y", JsonInteger(snapshot.project.play_res_y));
	project.AddRaw("info_count", JsonInteger(snapshot.project.info_count));
	project.AddRaw("style_count", JsonInteger(snapshot.project.style_count));
	project.AddRaw("event_count", JsonInteger(snapshot.project.event_count));
	project.AddRaw("dialogue_count", JsonInteger(snapshot.project.dialogue_count));
	project.AddRaw("comment_count", JsonInteger(snapshot.project.comment_count));
	project.AddRaw("attachment_count", JsonInteger(snapshot.project.attachment_count));
	project.AddRaw("extradata_count", JsonInteger(snapshot.project.extradata_count));
	project.AddRaw("active_row", JsonInteger(snapshot.project.active_row));
	project.AddRaw("scroll_position", JsonInteger(snapshot.project.scroll_position));
	project.AddRaw("video_position", JsonInteger(snapshot.project.video_position));
	builder.AddRaw("project", project.Build());

	builder.AddRaw("has_project_context", JsonBool(snapshot.has_project_context));
	return builder.Build();
}

std::string SerializeLineSnapshot(AutomationTemplateLineSnapshot const& snapshot) {
	JsonObjectBuilder builder;
	AddOptional(builder, "index", snapshot.index, JsonInteger<int>);
	AddOptional(builder, "line_class", snapshot.line_class, JsonString);
	AddOptional(builder, "layer", snapshot.layer, JsonInteger<int>);
	AddOptional(builder, "style", snapshot.style, JsonString);
	AddOptional(builder, "actor", snapshot.actor, JsonString);
	AddOptional(builder, "effect", snapshot.effect, JsonString);
	AddOptional(builder, "comment", snapshot.comment, JsonBool);
	AddOptional(builder, "text", snapshot.text, JsonString);
	AddOptional(builder, "start_time", snapshot.start_time, JsonInteger<int>);
	AddOptional(builder, "end_time", snapshot.end_time, JsonInteger<int>);
	return builder.Build();
}

std::string SerializeSyllableSnapshot(AutomationTemplateSyllableSnapshot const& snapshot) {
	JsonObjectBuilder builder;
	AddOptional(builder, "index", snapshot.index, JsonInteger<int>);
	AddOptional(builder, "text", snapshot.text, JsonString);
	AddOptional(builder, "text_stripped", snapshot.text_stripped, JsonString);
	AddOptional(builder, "inline_fx", snapshot.inline_fx, JsonString);
	AddOptional(builder, "start_time", snapshot.start_time, JsonInteger<int>);
	AddOptional(builder, "end_time", snapshot.end_time, JsonInteger<int>);
	AddOptional(builder, "duration", snapshot.duration, JsonInteger<int>);
	AddOptional(builder, "is_furi", snapshot.is_furi, JsonBool);
	AddOptional(builder, "left", snapshot.left, JsonDouble);
	AddOptional(builder, "center", snapshot.center, JsonDouble);
	AddOptional(builder, "right", snapshot.right, JsonDouble);
	AddOptional(builder, "width", snapshot.width, JsonDouble);
	AddOptional(builder, "height", snapshot.height, JsonDouble);
	return builder.Build();
}

std::string SerializeHighlightSnapshot(AutomationTemplateHighlightSnapshot const& snapshot) {
	JsonObjectBuilder builder;
	AddOptional(builder, "index", snapshot.index, JsonInteger<int>);
	AddOptional(builder, "start_time", snapshot.start_time, JsonInteger<int>);
	AddOptional(builder, "end_time", snapshot.end_time, JsonInteger<int>);
	AddOptional(builder, "duration", snapshot.duration, JsonInteger<int>);
	return builder.Build();
}

std::string SerializeCharSnapshot(AutomationTemplateCharSnapshot const& snapshot) {
	JsonObjectBuilder builder;
	AddOptional(builder, "index", snapshot.index, JsonInteger<int>);
	AddOptional(builder, "text", snapshot.text, JsonString);
	return builder.Build();
}

std::string SerializeSourceFragment(AutomationTemplateSourceFragment const& fragment) {
	JsonObjectBuilder builder;
	AddOptional(builder, "source_line_index", fragment.source_line_index, JsonInteger<int>);
	AddOptional(builder, "fragment_kind", fragment.fragment_kind, JsonString);
	AddOptional(builder, "text", fragment.text, JsonString);
	AddOptional(builder, "effect", fragment.effect, JsonString);
	return builder.Build();
}

std::string SerializeTemplateIdentity(AutomationTemplateIdentity const& identity) {
	JsonObjectBuilder builder;
	AddOptional(builder, "owner_script", identity.owner_script, JsonString);
	AddOptional(builder, "template_debug_id", identity.template_debug_id, JsonInteger<int>);
	AddOptional(builder, "template_kind", identity.template_kind, JsonString);
	if (!identity.template_kinds.empty())
		builder.AddRaw("template_kinds", SerializeStringArray(identity.template_kinds));
	AddOptional(builder, "fragment_kind", identity.fragment_kind, JsonString);
	AddOptional(builder, "template_id", identity.template_id, JsonString);
	AddOptional(builder, "source_line_index", identity.source_line_index, JsonInteger<int>);
	if (!identity.source_line_indices.empty())
		builder.AddRaw("source_line_indices", SerializeIntArray(identity.source_line_indices));
	return builder.Build();
}

std::string SerializeTemplateSource(AutomationTemplateSource const& source) {
	JsonObjectBuilder builder;
	AddOptional(builder, "style", source.style, JsonString);
	AddOptional(builder, "effect", source.effect, JsonString);
	AddOptional(builder, "text", source.text, JsonString);
	if (!source.fragments.empty())
		builder.AddRaw("fragments", SerializeObjectArray(source.fragments, SerializeSourceFragment));
	return builder.Build();
}

std::string SerializeTargetSnapshot(AutomationTemplateTargetSnapshot const& target) {
	JsonObjectBuilder builder;
	AddOptional(builder, "scope_kind", target.scope_kind, JsonString);
	AddOptional(builder, "original_line", target.original_line, SerializeLineSnapshot);
	AddOptional(builder, "line", target.line, SerializeLineSnapshot);
	AddOptional(builder, "syllable", target.syllable, SerializeSyllableSnapshot);
	AddOptional(builder, "base_syllable", target.base_syllable, SerializeSyllableSnapshot);
	AddOptional(builder, "highlight", target.highlight, SerializeHighlightSnapshot);
	AddOptional(builder, "character", target.character, SerializeCharSnapshot);
	return builder.Build();
}

std::string SerializeGeneratedLineSnapshot(AutomationGeneratedLineSnapshot const& snapshot) {
	JsonObjectBuilder builder;
	AddOptional(builder, "generated_index", snapshot.generated_index, JsonInteger<int>);
	AddOptional(builder, "text", snapshot.text, JsonString);
	AddOptional(builder, "style", snapshot.style, JsonString);
	AddOptional(builder, "layer", snapshot.layer, JsonInteger<int>);
	AddOptional(builder, "effect", snapshot.effect, JsonString);
	AddOptional(builder, "start_time", snapshot.start_time, JsonInteger<int>);
	AddOptional(builder, "end_time", snapshot.end_time, JsonInteger<int>);
	AddOptional(builder, "source_line_index", snapshot.source_line_index, JsonInteger<int>);
	AddOptional(builder, "template_debug_id", snapshot.template_debug_id, JsonInteger<int>);
	AddOptional(builder, "template_kind", snapshot.template_kind, JsonString);
	AddOptional(builder, "scope_kind", snapshot.scope_kind, JsonString);
	AddOptional(builder, "syllable_index", snapshot.syllable_index, JsonInteger<int>);
	AddOptional(builder, "highlight_index", snapshot.highlight_index, JsonInteger<int>);
	AddOptional(builder, "char_index", snapshot.char_index, JsonInteger<int>);
	return builder.Build();
}

std::string SerializeGeneratedLinesSnapshot(AutomationGeneratedLinesSnapshot const& snapshot) {
	JsonObjectBuilder builder;
	AddOptional(builder, "count", snapshot.count, JsonInteger<int>);
	AddOptional(builder, "last_line", snapshot.last_line, SerializeGeneratedLineSnapshot);
	return builder.Build();
}

std::string SerializeTemplateDebugState(AutomationTemplateDebugState const& state) {
	JsonObjectBuilder builder;
	AddOptional(builder, "kind", state.kind, JsonString);
	AddOptional(builder, "template_code", state.template_code, JsonString);
	AddOptional(builder, "template_text", state.template_text, JsonString);
	AddOptional(builder, "loop_index", state.loop_index, JsonInteger<int>);
	AddOptional(builder, "loop_count", state.loop_count, JsonInteger<int>);
	AddOptional(builder, "line_text", state.line_text, JsonString);
	AddOptional(builder, "line_style", state.line_style, JsonString);
	AddOptional(builder, "syllable_text", state.syllable_text, JsonString);
	AddOptional(builder, "syllable_index", state.syllable_index, JsonInteger<int>);
	AddOptional(builder, "base_syllable_text", state.base_syllable_text, JsonString);
	AddOptional(builder, "phase", state.phase, JsonString);
	AddOptional(builder, "scope_kind", state.scope_kind, JsonString);
	AddOptional(builder, "highlight_index", state.highlight_index, JsonInteger<int>);
	AddOptional(builder, "char_index", state.char_index, JsonInteger<int>);
	AddOptional(builder, "char_text", state.char_text, JsonString);
	AddOptional(builder, "expression", state.expression, JsonString);
	AddOptional(builder, "parse_error", state.parse_error, JsonString);
	AddOptional(builder, "runtime_error", state.runtime_error, JsonString);
	AddOptional(builder, "identity", state.identity, SerializeTemplateIdentity);
	AddOptional(builder, "source", state.source, SerializeTemplateSource);
	AddOptional(builder, "target", state.target, SerializeTargetSnapshot);
	AddOptional(builder, "generated", state.generated, SerializeGeneratedLinesSnapshot);
	return builder.Build();
}

std::string SerializeTraceRecord(AutomationRuntimeTraceRecord const& record) {
	JsonObjectBuilder builder;
	builder.AddRaw("sequence", JsonInteger(record.sequence));
	builder.AddRaw("invocation", SerializeInvocation(record.snapshot.invocation));
	builder.AddRaw("context_snapshot", SerializeContextSnapshot(record.snapshot.context_snapshot));
	if (record.snapshot.template_debug)
		builder.AddRaw("template_debug", SerializeTemplateDebugState(*record.snapshot.template_debug));
	else
		builder.AddRaw("template_debug", JsonNull());
	return builder.Build();
}

std::optional<std::string> BuildGeneratedLineSignature(AutomationRuntimeStateSnapshot const& snapshot) {
	if (!snapshot.template_debug || !snapshot.template_debug->generated || !snapshot.template_debug->generated->last_line)
		return std::nullopt;

	auto const& line = *snapshot.template_debug->generated->last_line;
	std::ostringstream out;
	out << line.generated_index.value_or(-1) << "|"
		<< line.template_debug_id.value_or(-1) << "|"
		<< line.source_line_index.value_or(-1) << "|"
		<< line.syllable_index.value_or(-1) << "|"
		<< line.highlight_index.value_or(-1) << "|"
		<< line.char_index.value_or(-1) << "|"
		<< line.text.value_or("");
	return out.str();
}

}

AutomationRuntimeJournal::AutomationRuntimeJournal(size_t max_records)
: max_records(max_records) {
}

void AutomationRuntimeJournal::OnRuntimeStateSnapshot(AutomationRuntimeStateSnapshot const& snapshot) {
	std::lock_guard<std::mutex> lock(mutex);

	auto const next_sequence = ++total_record_count;
	records.push_back({ next_sequence, snapshot });
	if (max_records) {
		while (records.size() > max_records) {
			records.pop_front();
			++dropped_record_count;
		}
	}

	if (snapshot.template_debug)
		++template_event_count;

	auto signature = BuildGeneratedLineSignature(snapshot);
	if (signature && signature != last_generated_line_signature) {
		last_generated_line_signature = std::move(signature);
		++generated_line_event_count;
	}
}

size_t AutomationRuntimeJournal::RecordCount() const {
	std::lock_guard<std::mutex> lock(mutex);
	return total_record_count;
}

size_t AutomationRuntimeJournal::RetainedRecordCount() const {
	std::lock_guard<std::mutex> lock(mutex);
	return records.size();
}

size_t AutomationRuntimeJournal::DroppedRecordCount() const {
	std::lock_guard<std::mutex> lock(mutex);
	return dropped_record_count;
}

size_t AutomationRuntimeJournal::TemplateEventCount() const {
	std::lock_guard<std::mutex> lock(mutex);
	return template_event_count;
}

size_t AutomationRuntimeJournal::GeneratedLineEventCount() const {
	std::lock_guard<std::mutex> lock(mutex);
	return generated_line_event_count;
}

std::optional<AutomationRuntimeStateSnapshot> AutomationRuntimeJournal::LastSnapshot() const {
	std::lock_guard<std::mutex> lock(mutex);
	if (records.empty())
		return std::nullopt;
	return records.back().snapshot;
}

std::vector<AutomationRuntimeTraceRecord> AutomationRuntimeJournal::GetRecords() const {
	std::lock_guard<std::mutex> lock(mutex);
	return { records.begin(), records.end() };
}

bool AutomationRuntimeJournal::WriteTraceFile(agi::fs::path const& path, std::string& error) const {
	std::vector<AutomationRuntimeTraceRecord> snapshot_records;
	{
		std::lock_guard<std::mutex> lock(mutex);
		snapshot_records.assign(records.begin(), records.end());
	}

	return WriteJsonLinesFile(
		path,
		snapshot_records,
		SerializeTraceRecord,
		"could not open automation runtime trace file for writing",
		error);
}

}
