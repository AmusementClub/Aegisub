#include "core_api_facade.h"

#include "ass_attachment.h"
#include "ass_dialogue.h"
#include "ass_file.h"
#include "ass_info.h"
#include "ass_io_core.h"
#include "ass_style.h"
#include "audio_provider_factory.h"
#include "avisynth_provider_registration.h"
#include "include/aegisub/subtitles_provider.h"
#include "include/aegisub/video_provider.h"
#include "provider_factory_registry.h"
#include "subtitle_editor_ops.h"
#include "subtitle_grid_ops.h"
#include "subtitle_format.h"
#include "ui_services.h"
#include "video_frame.h"
#include "video_provider_manager.h"

#include <libaegisub/audio/provider.h>
#include <libaegisub/fs.h>
#include <libaegisub/vfr.h>

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <utility>

namespace aegisub::core_api {

namespace {

ProviderKind ConvertProviderKind(provider_catalog::ProviderKind kind) {
	switch (kind) {
	case provider_catalog::ProviderKind::Audio:
		return ProviderKind::Audio;
	case provider_catalog::ProviderKind::Video:
		return ProviderKind::Video;
	case provider_catalog::ProviderKind::Subtitles:
		return ProviderKind::Subtitles;
	}
	throw std::invalid_argument("unknown provider catalog kind");
}

provider_catalog::ProviderCatalog QueryProviderCatalog(ProviderKind kind, std::string_view preferred_provider) {
	auto preferred = std::string(preferred_provider);
	switch (kind) {
	case ProviderKind::Audio:
		return GetAudioProviderCatalog(preferred);
	case ProviderKind::Video:
		return VideoProviderFactory::GetCatalog(preferred);
	case ProviderKind::Subtitles:
		return SubtitlesProviderFactory::GetCatalog(preferred);
	}
	throw std::invalid_argument("unknown provider kind");
}

ProviderCatalogSnapshot SnapshotCatalog(provider_catalog::ProviderCatalog const& source) {
	ProviderCatalogSnapshot snapshot;
	snapshot.kind = ConvertProviderKind(source.kind);
	snapshot.preferred_provider = source.preferred_provider;
	snapshot.providers.reserve(source.providers.size());
	for (auto const& provider : source.providers) {
		snapshot.providers.push_back({
			ConvertProviderKind(provider.kind),
			provider.name,
			provider.display_name,
			provider.unavailable_reason,
			provider.hidden,
			provider.available,
		});
	}
	return snapshot;
}

ProviderOpenReportSnapshot SnapshotProviderOpenReport(
	ProviderKind kind,
	provider_selection_diagnostics::SelectionReport source) {
	ProviderOpenReportSnapshot snapshot;
	snapshot.kind = kind;
	snapshot.preferred_provider = std::move(source.preferred_provider);
	snapshot.selected_provider = std::move(source.selected_provider);
	snapshot.attempts.reserve(source.attempts.size());
	for (auto& attempt : source.attempts) {
		snapshot.attempts.push_back({
			std::move(attempt.provider_name),
			std::move(attempt.outcome),
			std::move(attempt.detail),
		});
	}
	return snapshot;
}

VideoInfoSnapshot SnapshotVideoInfo(VideoProvider const& provider,
                                    std::string selected_provider) {
	auto fps = provider.GetFPS();
	return {
		provider.GetFrameCount(),
		provider.GetWidth(),
		provider.GetHeight(),
		provider.GetDAR(),
		fps.FPS(),
		fps.IsVFR(),
		provider.HasAudio(),
		provider.ShouldSetVideoProperties(),
		provider.WantsCaching(),
		std::move(selected_provider),
		provider.GetDecoderName(),
		provider.GetColorSpace(),
		provider.GetRealColorSpace(),
		provider.GetWarning(),
		provider.GetNativeFormatDescription(),
		provider.GetKeyFrames(),
	};
}

AudioInfoSnapshot SnapshotAudioInfo(agi::AudioProvider const& provider,
                                    bool source_needs_cache,
                                    std::string selected_provider) {
	auto stats = provider.GetMemoryStats();
	return {
		provider.GetNumSamples(),
		provider.GetDecodedSamples(),
		provider.GetSampleRate(),
		provider.GetBytesPerSample(),
		provider.GetChannels(),
		provider.AreSamplesFloat(),
		source_needs_cache,
		stats.logical_bytes,
		stats.decoded_bytes,
		std::move(selected_provider),
		stats.storage_kind,
	};
}

SubtitleRowSnapshot SnapshotSubtitleRow(AssDialogue const& line, int row_index) {
	return {
		line.Id,
		row_index,
		line.Comment,
		line.Layer,
		static_cast<int>(line.Start),
		static_cast<int>(line.End),
		line.Margin[0],
		line.Margin[1],
		line.Margin[2],
		line.Style.get(),
		line.Actor.get(),
		line.Effect.get(),
		line.Text.get(),
	};
}

bool HasField(std::uint32_t fields, SubtitleRowPatchFields field) {
	return (fields & static_cast<std::uint32_t>(field)) != 0;
}

std::uint32_t ChangeField(SubtitleChangeFields field) {
	return static_cast<std::uint32_t>(field);
}

std::uint32_t ChangedFieldsFromPatch(AssDialogue const& line, SubtitleRowPatch const& patch) {
	std::uint32_t result = 0;
	if (HasField(patch.fields, SubtitleRowPatchFields::Text)
		&& std::string_view(line.Text.get()) != patch.text)
		result |= ChangeField(SubtitleChangeFields::Text);
	if ((HasField(patch.fields, SubtitleRowPatchFields::Start)
			&& static_cast<int>(line.Start) != patch.start_ms)
		|| (HasField(patch.fields, SubtitleRowPatchFields::End)
			&& static_cast<int>(line.End) != patch.end_ms))
		result |= ChangeField(SubtitleChangeFields::Time);
	if ((HasField(patch.fields, SubtitleRowPatchFields::Comment)
			&& line.Comment != patch.comment)
		|| (HasField(patch.fields, SubtitleRowPatchFields::Layer)
			&& line.Layer != patch.layer)
		|| (HasField(patch.fields, SubtitleRowPatchFields::Margins)
			&& (line.Margin[0] != patch.margin_left
				|| line.Margin[1] != patch.margin_right
				|| line.Margin[2] != patch.margin_vertical))
		|| (HasField(patch.fields, SubtitleRowPatchFields::Style)
			&& std::string_view(line.Style.get()) != patch.style)
		|| (HasField(patch.fields, SubtitleRowPatchFields::Actor)
			&& std::string_view(line.Actor.get()) != patch.actor)
		|| (HasField(patch.fields, SubtitleRowPatchFields::Effect)
			&& std::string_view(line.Effect.get()) != patch.effect))
		result |= ChangeField(SubtitleChangeFields::Metadata);
	return result;
}

bool HasPasteField(std::uint32_t fields, SubtitlePasteOverFields field) {
	return (fields & static_cast<std::uint32_t>(field)) != 0;
}

std::uint32_t ChangedFieldsFromPasteOver(AssDialogue const& target,
                                         SubtitlePasteOverSource const& source,
                                         std::uint32_t fields) {
	std::uint32_t result = 0;
	if (HasPasteField(fields, SubtitlePasteOverFields::Text)
		&& std::string_view(target.Text.get()) != source.text)
		result |= ChangeField(SubtitleChangeFields::Text);
	if ((HasPasteField(fields, SubtitlePasteOverFields::Start)
			&& static_cast<int>(target.Start) != source.start_ms)
		|| (HasPasteField(fields, SubtitlePasteOverFields::End)
			&& static_cast<int>(target.End) != source.end_ms))
		result |= ChangeField(SubtitleChangeFields::Time);
	if ((HasPasteField(fields, SubtitlePasteOverFields::Comment)
			&& target.Comment != source.comment)
		|| (HasPasteField(fields, SubtitlePasteOverFields::Layer)
			&& target.Layer != source.layer)
		|| (HasPasteField(fields, SubtitlePasteOverFields::Style)
			&& std::string_view(target.Style.get()) != source.style)
		|| (HasPasteField(fields, SubtitlePasteOverFields::Actor)
			&& std::string_view(target.Actor.get()) != source.actor)
		|| (HasPasteField(fields, SubtitlePasteOverFields::MarginLeft)
			&& target.Margin[0] != source.margin_left)
		|| (HasPasteField(fields, SubtitlePasteOverFields::MarginRight)
			&& target.Margin[1] != source.margin_right)
		|| (HasPasteField(fields, SubtitlePasteOverFields::MarginVertical)
			&& target.Margin[2] != source.margin_vertical)
		|| (HasPasteField(fields, SubtitlePasteOverFields::Effect)
			&& std::string_view(target.Effect.get()) != source.effect))
		result |= ChangeField(SubtitleChangeFields::Metadata);
	return result;
}

AssFile::CompFunc SortComparator(SubtitleSortKey key) {
	switch (key) {
	case SubtitleSortKey::Start:
		return AssFile::CompStart;
	case SubtitleSortKey::End:
		return AssFile::CompEnd;
	case SubtitleSortKey::Style:
		return AssFile::CompStyle;
	case SubtitleSortKey::Actor:
		return AssFile::CompActor;
	case SubtitleSortKey::Effect:
		return AssFile::CompEffect;
	case SubtitleSortKey::Layer:
		return AssFile::CompLayer;
	}
	throw std::invalid_argument("unknown subtitle sort key");
}

std::vector<AssDialogue *> CollectSubtitleRows(AssFile& file) {
	std::vector<AssDialogue *> rows;
	for (auto& line : file.Events)
		rows.push_back(&line);
	return rows;
}

void RequireUniqueRows(std::vector<std::size_t> rows, char const *message) {
	std::sort(rows.begin(), rows.end());
	if (std::adjacent_find(rows.begin(), rows.end()) != rows.end())
		throw std::invalid_argument(message);
}

SubtitleInfoSnapshot SnapshotSubtitleInfo(AssFile const& file,
                                          std::string format_name,
                                          int row_count) {
	int width = 0;
	int height = 0;
	auto resolution_type = file.GetResolutionType(ScriptResolutionType::PlayRes, width, height);
	return {
		row_count,
		static_cast<int>(file.Styles.size()),
		static_cast<int>(file.Attachments.size()),
		static_cast<int>(file.Info.size()),
		width,
		height,
		static_cast<int>(resolution_type),
		std::move(format_name),
	};
}

} // namespace

Context::Context(ContextOptions options)
: thread_context(options.use_thread_hooks
	? core::CoreHostThreadContext(std::move(options.thread_hooks))
	: core::CoreHostThreadContext::FromGlobalDispatch()) {
}

core::CoreHostThreadContext const& Context::ThreadContext() const {
	return thread_context;
}

std::string const& Context::LastError() const {
	return last_error;
}

void Context::ClearLastError() {
	last_error.clear();
}

void Context::SetLastError(std::string error) {
	last_error = std::move(error);
}

void Context::SetLastProviderOpenReport(ProviderKind kind, provider_selection_diagnostics::SelectionReport report) {
	switch (kind) {
	case ProviderKind::Audio:
		last_audio_open_report = SnapshotProviderOpenReport(kind, std::move(report));
		return;
	case ProviderKind::Video:
		last_video_open_report = SnapshotProviderOpenReport(kind, std::move(report));
		return;
	case ProviderKind::Subtitles:
		return;
	}
}

ProviderOpenReportSnapshot const& Context::LastProviderOpenReport(ProviderKind kind) const {
	switch (kind) {
	case ProviderKind::Audio:
		return last_audio_open_report;
	case ProviderKind::Video:
		return last_video_open_report;
	case ProviderKind::Subtitles: {
		// Subtitle opens never go through a provider factory, so there is no
		// per-open selection report. Match GetLastProviderOpenReport's behavior
		// and return a stable empty snapshot rather than leaking the video
		// report through the fall-through.
		static const ProviderOpenReportSnapshot empty_subtitle_report{ProviderKind::Subtitles};
		return empty_subtitle_report;
	}
	}
	return last_video_open_report;
}

VideoSession::VideoSession(std::unique_ptr<VideoProvider> provider, VideoInfoSnapshot info)
: provider(std::move(provider))
, info(std::move(info)) {
}

VideoSession::~VideoSession() = default;

VideoInfoSnapshot const& VideoSession::Info() const {
	return info;
}

VideoFrameSnapshot VideoSession::FrameAt(int frame_number) const {
	if (frame_number < 0 || frame_number >= info.frame_count)
		throw std::invalid_argument("video frame number out of range");

	VideoFrame frame;
	provider->GetFrame(frame_number, frame);
	return {
		std::move(frame.data),
		frame.width,
		frame.height,
		frame.pitch,
		frame.flipped,
	};
}

AudioSession::AudioSession(std::unique_ptr<agi::AudioProvider> provider, AudioInfoSnapshot info)
: provider(std::move(provider))
, info(std::move(info)) {
}

AudioSession::~AudioSession() = default;

AudioInfoSnapshot const& AudioSession::Info() const {
	return info;
}

SubtitleSession::SubtitleSession(std::unique_ptr<AssFile> file,
                                 SubtitleInfoSnapshot info,
                                 std::vector<AssDialogue *> rows)
: file(std::move(file))
, info(std::move(info))
, rows(std::move(rows)) {
}

SubtitleSession::~SubtitleSession() = default;

SubtitleInfoSnapshot const& SubtitleSession::Info() const {
	return info;
}

SubtitleStateSnapshot SubtitleSession::State() const {
	return {
		revision,
		row_change_revision,
		row_change_first_row,
		row_change_row_count,
		row_change_fields,
		dirty,
	};
}

std::size_t SubtitleSession::RowCount() const {
	return rows.size();
}

SubtitleRowSnapshot SubtitleSession::RowAt(std::size_t index) const {
	return SnapshotSubtitleRow(*rows.at(index), static_cast<int>(index));
}

void SubtitleSession::MarkRowsChanged(std::size_t first_row, std::size_t row_count, std::uint32_t fields) {
	++revision;
	row_change_revision = revision;
	row_change_first_row = first_row;
	row_change_row_count = row_count;
	row_change_fields = fields;
	dirty = true;
}

void SubtitleSession::RefreshRows() {
	rows = CollectSubtitleRows(*file);
	info.row_count = static_cast<int>(rows.size());
	int row_index = 0;
	for (auto *row : rows)
		row->Row = row_index++;
}

void SubtitleSession::SetRowText(std::size_t index, std::string_view text) {
	auto& row_text = rows.at(index)->Text;
	if (row_text == text)
		return;
	row_text = std::string(text);
	MarkRowsChanged(index, 1, ChangeField(SubtitleChangeFields::Text));
}

std::size_t SubtitleSession::SetRowTexts(std::vector<SubtitleRowTextEdit> const& edits) {
	std::vector<std::size_t> edit_rows;
	edit_rows.reserve(edits.size());
	for (auto const& edit : edits) {
		if (edit.row >= rows.size())
			throw std::invalid_argument("subtitle row text edit row is out of range");
		edit_rows.push_back(edit.row);
	}
	RequireUniqueRows(std::move(edit_rows), "subtitle row text edit batch contains duplicate rows");

	auto first_row = rows.size();
	std::size_t last_row = 0;
	std::size_t applied = 0;
	for (auto const& edit : edits) {
		auto& text = rows[edit.row]->Text;
		if (text == edit.text)
			continue;
		text = edit.text;
		first_row = std::min(first_row, edit.row);
		last_row = std::max(last_row, edit.row);
		++applied;
	}
	if (applied != 0)
		MarkRowsChanged(first_row, last_row - first_row + 1, ChangeField(SubtitleChangeFields::Text));
	return applied;
}

std::size_t SubtitleSession::ApplyRowPatches(std::vector<SubtitleRowPatch> const& patches) {
	auto first_row = rows.size();
	std::size_t last_row = 0;
	std::uint32_t change_fields = 0;
	std::size_t applied = 0;
	std::vector<std::size_t> patch_rows;
	patch_rows.reserve(patches.size());
	for (auto const& patch : patches) {
		if (patch.row >= rows.size())
			throw std::invalid_argument("subtitle row patch row is out of range");
		patch_rows.push_back(patch.row);
		if (patch.fields == 0)
			continue;
		auto const& line = *rows[patch.row];
		auto const start = HasField(patch.fields, SubtitleRowPatchFields::Start)
			? patch.start_ms
			: static_cast<int>(line.Start);
		auto const end = HasField(patch.fields, SubtitleRowPatchFields::End)
			? patch.end_ms
			: static_cast<int>(line.End);
		if (start < 0 || end < 0 || start > end)
			throw std::invalid_argument("subtitle row patch has invalid time range");
		if (HasField(patch.fields, SubtitleRowPatchFields::Margins)
			&& (patch.margin_left < 0 || patch.margin_right < 0 || patch.margin_vertical < 0))
			throw std::invalid_argument("subtitle row patch has negative margins");
	}
	RequireUniqueRows(std::move(patch_rows), "subtitle row patch batch contains duplicate rows");

	for (auto const& patch : patches) {
		if (patch.fields == 0)
			continue;
		auto& line = *rows[patch.row];
		auto const actual_changes = ChangedFieldsFromPatch(line, patch);
		if (actual_changes == 0)
			continue;
		first_row = std::min(first_row, patch.row);
		last_row = std::max(last_row, patch.row);
		change_fields |= actual_changes;
		++applied;
		if (HasField(patch.fields, SubtitleRowPatchFields::Comment))
			line.Comment = patch.comment;
		if (HasField(patch.fields, SubtitleRowPatchFields::Layer))
			line.Layer = patch.layer;
		if (HasField(patch.fields, SubtitleRowPatchFields::Start))
			line.Start = patch.start_ms;
		if (HasField(patch.fields, SubtitleRowPatchFields::End))
			line.End = patch.end_ms;
		if (HasField(patch.fields, SubtitleRowPatchFields::Margins)) {
			line.Margin[0] = patch.margin_left;
			line.Margin[1] = patch.margin_right;
			line.Margin[2] = patch.margin_vertical;
		}
		if (HasField(patch.fields, SubtitleRowPatchFields::Style))
			line.Style = patch.style;
		if (HasField(patch.fields, SubtitleRowPatchFields::Actor))
			line.Actor = patch.actor;
		if (HasField(patch.fields, SubtitleRowPatchFields::Effect))
			line.Effect = patch.effect;
		if (HasField(patch.fields, SubtitleRowPatchFields::Text))
			line.Text = patch.text;
	}
	if (applied != 0)
		MarkRowsChanged(first_row, last_row - first_row + 1, change_fields);
	return applied;
}

std::size_t SubtitleSession::InsertRow(std::size_t row) {
	if (row > rows.size())
		throw std::invalid_argument("subtitle row insert position is out of range");

	std::unique_ptr<AssDialogue> line;
	if (rows.empty())
		line = std::make_unique<AssDialogue>();
	else if (row == 0)
		line = aegisub::subtitle_editor_ops::CreateLineBeforeActive(*rows.front(), file->Events, 5000);
	else
		line = aegisub::subtitle_editor_ops::CreateLineAfterActive(*rows[row - 1], file->Events, 5000);

	line->Text = "";
	auto *inserted = line.release();
	if (row == rows.size())
		file->Events.push_back(*inserted);
	else
		file->Events.insert(file->Events.iterator_to(*rows[row]), *inserted);

	RefreshRows();
	MarkRowsChanged(row, rows.size() - row, ChangeField(SubtitleChangeFields::Structure));
	return row;
}

void SubtitleSession::DeleteRows(std::size_t first_row, std::size_t row_count) {
	if (first_row > rows.size() || row_count > rows.size() - first_row)
		throw std::invalid_argument("subtitle row delete range is out of range");
	if (row_count == 0)
		return;

	for (std::size_t i = 0; i < row_count; ++i) {
		auto *row = rows[first_row + i];
		file->Events.erase(file->Events.iterator_to(*row));
		delete row;
	}

	RefreshRows();
	MarkRowsChanged(first_row, rows.size() - first_row, ChangeField(SubtitleChangeFields::Structure));
}

bool SubtitleSession::MoveSelectedRows(std::vector<std::size_t> const& selected_rows,
                                       int direction,
                                       std::vector<std::size_t>& moved_selected_rows) {
	moved_selected_rows.clear();
	if (direction != -1 && direction != 1)
		throw std::invalid_argument("subtitle row move direction must be -1 or 1");
	if (selected_rows.empty())
		return false;

	Selection selection;
	std::vector<std::size_t> old_selected_rows;
	old_selected_rows.reserve(selected_rows.size());
	for (auto row : selected_rows) {
		if (row >= rows.size())
			throw std::invalid_argument("subtitle row move selection row is out of range");
		selection.insert(rows[row]);
		old_selected_rows.push_back(row);
	}
	if (selection.size() != selected_rows.size())
		throw std::invalid_argument("subtitle row move selection contains duplicate rows");

	auto moved = direction < 0
		? aegisub::subtitle_grid_ops::MoveSelectionUp(file->Events, selection)
		: aegisub::subtitle_grid_ops::MoveSelectionDown(file->Events, selection);
	if (!moved)
		return false;

	RefreshRows();
	moved_selected_rows.reserve(selection.size());
	for (std::size_t row = 0; row < rows.size(); ++row) {
		if (selection.count(rows[row]))
			moved_selected_rows.push_back(row);
	}

	auto min_changed = rows.size();
	std::size_t max_changed = 0;
	for (auto row : old_selected_rows) {
		min_changed = std::min(min_changed, row);
		max_changed = std::max(max_changed, row);
	}
	for (auto row : moved_selected_rows) {
		min_changed = std::min(min_changed, row);
		max_changed = std::max(max_changed, row);
	}
	MarkRowsChanged(min_changed, max_changed - min_changed + 1, ChangeField(SubtitleChangeFields::Structure));
	return true;
}

bool SubtitleSession::DuplicateSelectedRows(std::vector<std::size_t> const& selected_rows,
                                            std::vector<std::size_t>& duplicated_rows) {
	duplicated_rows.clear();
	if (selected_rows.empty())
		return false;

	Selection selection;
	for (auto row : selected_rows) {
		if (row >= rows.size())
			throw std::invalid_argument("subtitle row duplicate selection row is out of range");
		selection.insert(rows[row]);
	}
	if (selection.size() != selected_rows.size())
		throw std::invalid_argument("subtitle row duplicate selection contains duplicate rows");

	Selection duplicated_lines;
	auto in_selection = [&](AssDialogue const& line) {
		return selection.count(const_cast<AssDialogue *>(&line)) != 0;
	};

	auto start = file->Events.begin();
	auto end = file->Events.end();
	while (start != end) {
		start = std::find_if(start, end, in_selection);
		if (start == end)
			break;

		auto insert_pos = std::find_if_not(start, end, in_selection);
		auto last = std::prev(insert_pos);
		do {
			auto copy = std::make_unique<AssDialogue>(*start);
			file->Events.insert(insert_pos, *copy);
			auto *inserted = copy.release();
			duplicated_lines.insert(inserted);
		} while (start++ != last);

		start = insert_pos;
	}

	if (duplicated_lines.empty())
		return false;

	RefreshRows();
	duplicated_rows.reserve(duplicated_lines.size());
	auto first_duplicated_row = rows.size();
	for (std::size_t row = 0; row < rows.size(); ++row) {
		if (duplicated_lines.count(rows[row])) {
			duplicated_rows.push_back(row);
			first_duplicated_row = std::min(first_duplicated_row, row);
		}
	}
	MarkRowsChanged(first_duplicated_row, rows.size() - first_duplicated_row, ChangeField(SubtitleChangeFields::Structure));
	return true;
}

bool SubtitleSession::SortRows(SubtitleSortKey key,
                               bool selected_only,
                               std::vector<std::size_t> const& selected_rows,
                               std::vector<std::size_t>& sorted_selected_rows) {
	sorted_selected_rows.clear();
	Selection selection;
	if (selected_only) {
		for (auto row : selected_rows) {
			if (row >= rows.size())
				throw std::invalid_argument("subtitle row sort selection row is out of range");
			selection.insert(rows[row]);
		}
		if (selection.size() != selected_rows.size())
			throw std::invalid_argument("subtitle row sort selection contains duplicate rows");
		if (selected_rows.size() <= 1)
			return false;
	}

	auto old_rows = rows;
	file->Sort(SortComparator(key), selection);
	RefreshRows();
	if (old_rows == rows)
		return false;

	auto first_changed = rows.size();
	std::size_t last_changed = 0;
	auto const count = std::min(old_rows.size(), rows.size());
	for (std::size_t row = 0; row < count; ++row) {
		if (old_rows[row] != rows[row]) {
			first_changed = std::min(first_changed, row);
			last_changed = std::max(last_changed, row);
		}
	}
	if (first_changed == rows.size())
		return false;

	if (selected_only) {
		sorted_selected_rows.reserve(selection.size());
		for (std::size_t row = 0; row < rows.size(); ++row) {
			if (selection.count(rows[row]))
				sorted_selected_rows.push_back(row);
		}
	}

	MarkRowsChanged(first_changed, last_changed - first_changed + 1, ChangeField(SubtitleChangeFields::Structure));
	return true;
}

std::size_t SubtitleSession::PasteOverRows(std::vector<std::size_t> const& target_rows,
                                           std::vector<SubtitlePasteOverSource> const& sources,
                                           std::uint32_t fields) {
	if (target_rows.size() != sources.size())
		throw std::invalid_argument("subtitle paste-over target/source count mismatch");

	constexpr auto known_fields =
		static_cast<std::uint32_t>(SubtitlePasteOverFields::Comment)
		| static_cast<std::uint32_t>(SubtitlePasteOverFields::Layer)
		| static_cast<std::uint32_t>(SubtitlePasteOverFields::Start)
		| static_cast<std::uint32_t>(SubtitlePasteOverFields::End)
		| static_cast<std::uint32_t>(SubtitlePasteOverFields::Style)
		| static_cast<std::uint32_t>(SubtitlePasteOverFields::Actor)
		| static_cast<std::uint32_t>(SubtitlePasteOverFields::MarginLeft)
		| static_cast<std::uint32_t>(SubtitlePasteOverFields::MarginRight)
		| static_cast<std::uint32_t>(SubtitlePasteOverFields::MarginVertical)
		| static_cast<std::uint32_t>(SubtitlePasteOverFields::Effect)
		| static_cast<std::uint32_t>(SubtitlePasteOverFields::Text);
	if ((fields & ~known_fields) != 0)
		throw std::invalid_argument("subtitle paste-over contains unknown field bits");
	if (target_rows.empty())
		return 0;

	Selection target_set;
	auto first_row = rows.size();
	std::size_t last_row = 0;
	std::uint32_t change_fields = 0;
	std::size_t applied = 0;
	for (std::size_t i = 0; i < target_rows.size(); ++i) {
		auto const row = target_rows[i];
		if (row >= rows.size())
			throw std::invalid_argument("subtitle paste-over target row is out of range");
		if (!target_set.insert(rows[row]).second)
			throw std::invalid_argument("subtitle paste-over target rows contain duplicates");

		auto const& source = sources[i];
		auto const& target = *rows[row];
		auto const start = HasPasteField(fields, SubtitlePasteOverFields::Start)
			? source.start_ms
			: static_cast<int>(target.Start);
		auto const end = HasPasteField(fields, SubtitlePasteOverFields::End)
			? source.end_ms
			: static_cast<int>(target.End);
		if (start < 0 || end < 0 || start > end)
			throw std::invalid_argument("subtitle paste-over has invalid time range");
		if ((HasPasteField(fields, SubtitlePasteOverFields::MarginLeft) && source.margin_left < 0)
			|| (HasPasteField(fields, SubtitlePasteOverFields::MarginRight) && source.margin_right < 0)
			|| (HasPasteField(fields, SubtitlePasteOverFields::MarginVertical) && source.margin_vertical < 0))
			throw std::invalid_argument("subtitle paste-over has negative margins");
	}

	for (std::size_t i = 0; i < target_rows.size(); ++i) {
		auto& target = *rows[target_rows[i]];
		auto const& source = sources[i];
		auto const actual_changes = ChangedFieldsFromPasteOver(target, source, fields);
		if (actual_changes == 0)
			continue;
		first_row = std::min(first_row, target_rows[i]);
		last_row = std::max(last_row, target_rows[i]);
		change_fields |= actual_changes;
		++applied;
		if (HasPasteField(fields, SubtitlePasteOverFields::Comment))
			target.Comment = source.comment;
		if (HasPasteField(fields, SubtitlePasteOverFields::Layer))
			target.Layer = source.layer;
		if (HasPasteField(fields, SubtitlePasteOverFields::Start))
			target.Start = source.start_ms;
		if (HasPasteField(fields, SubtitlePasteOverFields::End))
			target.End = source.end_ms;
		if (HasPasteField(fields, SubtitlePasteOverFields::Style))
			target.Style = source.style;
		if (HasPasteField(fields, SubtitlePasteOverFields::Actor))
			target.Actor = source.actor;
		if (HasPasteField(fields, SubtitlePasteOverFields::MarginLeft))
			target.Margin[0] = source.margin_left;
		if (HasPasteField(fields, SubtitlePasteOverFields::MarginRight))
			target.Margin[1] = source.margin_right;
		if (HasPasteField(fields, SubtitlePasteOverFields::MarginVertical))
			target.Margin[2] = source.margin_vertical;
		if (HasPasteField(fields, SubtitlePasteOverFields::Effect))
			target.Effect = source.effect;
		if (HasPasteField(fields, SubtitlePasteOverFields::Text))
			target.Text = source.text;
	}

	if (applied != 0)
		MarkRowsChanged(first_row, last_row - first_row + 1, change_fields);
	return applied;
}

void SubtitleSession::Save(SubtitleSaveOptions options) const {
	auto path = agi::fs::PathFromString(options.path);
	auto const encoding = options.encoding.empty() ? std::string("utf-8") : std::move(options.encoding);
	if (agi::fs::HasExtension(path, "ass")) {
		AssWriteOptions write_options;
		WriteAssFileForCore(file.get(), path, agi::vfr::Framerate(), encoding, write_options);
		dirty = false;
		return;
	}

	auto const* writer = SubtitleFormat::GetWriter(path);
	if (!writer)
		throw UnknownSubtitleFormatError("Unknown subtitle format for output path");

	auto const* save_source = file.get();
	std::unique_ptr<AssFile> save_copy;
	if (!file->Extradata.empty()) {
		save_copy = std::make_unique<AssFile>(*file);
		save_copy->CleanExtradata();
		save_source = save_copy.get();
	}

	writer->WriteFile(save_source, path, agi::vfr::Framerate(), encoding, std::make_shared<agi::NullSingleChoiceInteractionSink>());
	dirty = false;
}

std::unique_ptr<Context> CreateContext(ContextOptions options) {
	return std::make_unique<Context>(std::move(options));
}

Status TryCreateContext(ContextOptions options, std::unique_ptr<Context>& context, std::string *error) {
	context.reset();
	if (error)
		error->clear();
	if (options.abi_version != AbiVersion) {
		if (error)
			*error = "unsupported core API ABI version";
		return Status::InvalidArgument;
	}

	try {
		context = CreateContext(std::move(options));
		return Status::Ok;
	}
	catch (core::CoreHostTimeoutError const& err) {
		if (error)
			*error = err.what();
		return Status::TimedOut;
	}
	catch (std::exception const& err) {
		if (error)
			*error = err.what();
		return Status::Error;
	}
	catch (...) {
		if (error)
			*error = "unknown core API context creation error";
		return Status::Error;
	}
}

Status RegisterBuiltinProviderFactories(Context& context) {
	if (AreProviderFactoryRegistriesFinalized()) {
		context.SetLastError("provider registry is finalized");
		return Status::RegistryFinalized;
	}

	try {
		RegisterAvisynthProviderFactories();
		context.ClearLastError();
		return Status::Ok;
	}
	catch (std::logic_error const& err) {
		context.SetLastError(err.what());
		return Status::RegistryFinalized;
	}
	catch (core::CoreHostTimeoutError const& err) {
		context.SetLastError(err.what());
		return Status::TimedOut;
	}
	catch (std::exception const& err) {
		context.SetLastError(err.what());
		return Status::Error;
	}
	catch (...) {
		context.SetLastError("unknown builtin provider registration error");
		return Status::Error;
	}
}

Status FinalizeProviderRegistry(Context& context) {
	try {
		FinalizeProviderFactoryRegistries();
		context.ClearLastError();
		return Status::Ok;
	}
	catch (core::CoreHostTimeoutError const& err) {
		context.SetLastError(err.what());
		return Status::TimedOut;
	}
	catch (std::exception const& err) {
		context.SetLastError(err.what());
		return Status::Error;
	}
	catch (...) {
		context.SetLastError("unknown provider registry finalization error");
		return Status::Error;
	}
}

bool IsProviderRegistryFinalized(Context const&) {
	return AreProviderFactoryRegistriesFinalized();
}

Status GetProviderCatalog(Context& context,
                          ProviderKind kind,
                          std::string_view preferred_provider,
                          ProviderCatalogSnapshot& catalog) {
	try {
		catalog = SnapshotCatalog(QueryProviderCatalog(kind, preferred_provider));
		context.ClearLastError();
		return Status::Ok;
	}
	catch (std::invalid_argument const& err) {
		context.SetLastError(err.what());
		return Status::InvalidArgument;
	}
	catch (core::CoreHostTimeoutError const& err) {
		context.SetLastError(err.what());
		return Status::TimedOut;
	}
	catch (std::exception const& err) {
		context.SetLastError(err.what());
		return Status::Error;
	}
	catch (...) {
		context.SetLastError("unknown provider catalog error");
		return Status::Error;
	}
}

Status OpenVideo(Context& context,
                 VideoOpenOptions options,
                 std::unique_ptr<VideoSession>& session) {
	session.reset();
	try {
		auto runner = agi::InlineBackgroundRunnerFactory().Create("Open video", "Opening video");
		auto choice_sink = std::make_shared<agi::NullSingleChoiceInteractionSink>();
		auto provider = VideoProviderFactory::GetProviderWithPreferred(
			agi::fs::PathFromString(options.path),
			options.colormatrix,
			options.preferred_provider,
			runner.get(),
			std::move(choice_sink),
			options.max_cache_size_bytes);
		if (!provider) {
			context.SetLastProviderOpenReport(ProviderKind::Video, GetLastVideoProviderSelectionReport());
			context.SetLastError("video provider returned null");
			return Status::Error;
		}

		auto report = GetLastVideoProviderSelectionReport();
		auto info = SnapshotVideoInfo(*provider, report.selected_provider);
		context.SetLastProviderOpenReport(ProviderKind::Video, std::move(report));
		session = std::make_unique<VideoSession>(std::move(provider), std::move(info));
		context.ClearLastError();
		return Status::Ok;
	}
	catch (agi::fs::FileNotFound const& err) {
		context.SetLastProviderOpenReport(ProviderKind::Video, GetLastVideoProviderSelectionReport());
		context.SetLastError(err.GetMessage());
		return Status::FileNotFound;
	}
	catch (VideoNotSupported const& err) {
		context.SetLastProviderOpenReport(ProviderKind::Video, GetLastVideoProviderSelectionReport());
		context.SetLastError(err.GetMessage());
		return Status::NotSupported;
	}
	catch (VideoProviderError const& err) {
		context.SetLastProviderOpenReport(ProviderKind::Video, GetLastVideoProviderSelectionReport());
		context.SetLastError(err.GetMessage());
		return Status::Error;
	}
	catch (agi::Exception const& err) {
		context.SetLastProviderOpenReport(ProviderKind::Video, GetLastVideoProviderSelectionReport());
		context.SetLastError(err.GetMessage());
		return Status::Error;
	}
	catch (std::invalid_argument const& err) {
		context.SetLastProviderOpenReport(ProviderKind::Video, GetLastVideoProviderSelectionReport());
		context.SetLastError(err.what());
		return Status::InvalidArgument;
	}
	catch (core::CoreHostTimeoutError const& err) {
		context.SetLastProviderOpenReport(ProviderKind::Video, GetLastVideoProviderSelectionReport());
		context.SetLastError(err.what());
		return Status::TimedOut;
	}
	catch (std::exception const& err) {
		context.SetLastProviderOpenReport(ProviderKind::Video, GetLastVideoProviderSelectionReport());
		context.SetLastError(err.what());
		return Status::Error;
	}
	catch (...) {
		context.SetLastProviderOpenReport(ProviderKind::Video, GetLastVideoProviderSelectionReport());
		context.SetLastError("unknown video open error");
		return Status::Error;
	}
}

Status OpenAudio(Context& context,
                 AudioOpenOptions options,
                 std::unique_ptr<AudioSession>& session) {
	session.reset();
	try {
		auto runner = agi::InlineBackgroundRunnerFactory().Create("Open audio", "Opening audio");
		auto choice_sink = std::make_shared<agi::NullSingleChoiceInteractionSink>();
		bool source_needs_cache = false;
		auto provider = GetAudioProviderWithPreferred(
			agi::fs::PathFromString(options.path),
			options.preferred_provider,
			runner.get(),
			std::move(choice_sink),
			&source_needs_cache);
		if (!provider) {
			context.SetLastProviderOpenReport(ProviderKind::Audio, GetLastAudioProviderSelectionReport());
			context.SetLastError("audio provider returned null");
			return Status::Error;
		}

		auto report = GetLastAudioProviderSelectionReport();
		auto info = SnapshotAudioInfo(*provider, source_needs_cache, report.selected_provider);
		context.SetLastProviderOpenReport(ProviderKind::Audio, std::move(report));
		session = std::make_unique<AudioSession>(std::move(provider), std::move(info));
		context.ClearLastError();
		return Status::Ok;
	}
	catch (agi::fs::FileNotFound const& err) {
		context.SetLastProviderOpenReport(ProviderKind::Audio, GetLastAudioProviderSelectionReport());
		context.SetLastError(err.GetMessage());
		return Status::FileNotFound;
	}
	catch (agi::AudioDataNotFound const& err) {
		context.SetLastProviderOpenReport(ProviderKind::Audio, GetLastAudioProviderSelectionReport());
		context.SetLastError(err.GetMessage());
		return Status::NoMedia;
	}
	catch (agi::AudioProviderError const& err) {
		context.SetLastProviderOpenReport(ProviderKind::Audio, GetLastAudioProviderSelectionReport());
		context.SetLastError(err.GetMessage());
		return Status::Error;
	}
	catch (agi::Exception const& err) {
		context.SetLastProviderOpenReport(ProviderKind::Audio, GetLastAudioProviderSelectionReport());
		context.SetLastError(err.GetMessage());
		return Status::Error;
	}
	catch (std::invalid_argument const& err) {
		context.SetLastProviderOpenReport(ProviderKind::Audio, GetLastAudioProviderSelectionReport());
		context.SetLastError(err.what());
		return Status::InvalidArgument;
	}
	catch (core::CoreHostTimeoutError const& err) {
		context.SetLastProviderOpenReport(ProviderKind::Audio, GetLastAudioProviderSelectionReport());
		context.SetLastError(err.what());
		return Status::TimedOut;
	}
	catch (std::exception const& err) {
		context.SetLastProviderOpenReport(ProviderKind::Audio, GetLastAudioProviderSelectionReport());
		context.SetLastError(err.what());
		return Status::Error;
	}
	catch (...) {
		context.SetLastProviderOpenReport(ProviderKind::Audio, GetLastAudioProviderSelectionReport());
		context.SetLastError("unknown audio open error");
		return Status::Error;
	}
}

Status OpenSubtitles(Context& context,
                     SubtitleOpenOptions options,
                     std::unique_ptr<SubtitleSession>& session) {
	session.reset();
	try {
		auto path = agi::fs::PathFromString(options.path);
		auto const* reader = SubtitleFormat::GetReader(path, options.encoding);
		if (!reader) {
			context.SetLastError("no subtitle reader supports this file");
			return Status::NotSupported;
		}

		auto file = std::make_unique<AssFile>();
		reader->ReadFile(file.get(), path, agi::vfr::Framerate(), options.encoding, {});
		auto rows = CollectSubtitleRows(*file);
		auto info = SnapshotSubtitleInfo(*file, reader->GetName(), static_cast<int>(rows.size()));
		session = std::make_unique<SubtitleSession>(std::move(file), std::move(info), std::move(rows));
		context.ClearLastError();
		return Status::Ok;
	}
	catch (agi::fs::FileNotFound const& err) {
		context.SetLastError(err.GetMessage());
		return Status::FileNotFound;
	}
	catch (agi::InvalidInputException const& err) {
		context.SetLastError(err.GetMessage());
		return Status::InvalidArgument;
	}
	catch (agi::fs::FileSystemError const& err) {
		context.SetLastError(err.GetMessage());
		return Status::FileSystemError;
	}
	catch (agi::Exception const& err) {
		context.SetLastError(err.GetMessage());
		return Status::Error;
	}
	catch (std::invalid_argument const& err) {
		context.SetLastError(err.what());
		return Status::InvalidArgument;
	}
	catch (core::CoreHostTimeoutError const& err) {
		context.SetLastError(err.what());
		return Status::TimedOut;
	}
	catch (std::exception const& err) {
		context.SetLastError(err.what());
		return Status::Error;
	}
	catch (...) {
		context.SetLastError("unknown subtitle open error");
		return Status::Error;
	}
}

Status SaveSubtitles(Context& context,
                     SubtitleSession const& session,
                     SubtitleSaveOptions options) {
	try {
		session.Save(std::move(options));
		context.ClearLastError();
		return Status::Ok;
	}
	catch (UnknownSubtitleFormatError const& err) {
		context.SetLastError(err.GetMessage());
		return Status::NotSupported;
	}
	catch (agi::fs::FileSystemError const& err) {
		context.SetLastError(err.GetMessage());
		return Status::FileSystemError;
	}
	catch (agi::InvalidInputException const& err) {
		context.SetLastError(err.GetMessage());
		return Status::InvalidArgument;
	}
	catch (agi::Exception const& err) {
		context.SetLastError(err.GetMessage());
		return Status::Error;
	}
	catch (std::invalid_argument const& err) {
		context.SetLastError(err.what());
		return Status::InvalidArgument;
	}
	catch (core::CoreHostTimeoutError const& err) {
		context.SetLastError(err.what());
		return Status::TimedOut;
	}
	catch (std::exception const& err) {
		context.SetLastError(err.what());
		return Status::Error;
	}
	catch (...) {
		context.SetLastError("unknown subtitle save error");
		return Status::Error;
	}
}

Status GetLastProviderOpenReport(Context& context,
                                 ProviderKind kind,
                                 ProviderOpenReportSnapshot& report) {
	switch (kind) {
	case ProviderKind::Audio:
	case ProviderKind::Video:
		report = context.LastProviderOpenReport(kind);
		return Status::Ok;
	case ProviderKind::Subtitles:
		report = {};
		report.kind = kind;
		return Status::Ok;
	}
	return Status::InvalidArgument;
}

} // namespace aegisub::core_api
