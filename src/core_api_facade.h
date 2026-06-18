#pragma once

#include "core_host_context.h"
#include "provider_catalog.h"
#include "provider_selection_diagnostics.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class VideoProvider;
class AssFile;
class AssDialogue;
namespace agi { class AudioProvider; }

namespace aegisub::core_api {

inline constexpr std::uint32_t AbiVersion = 16;

enum class Status {
	Ok = 0,
	Cancelled = 1,
	InvalidArgument = 2,
	FileNotFound = 3,
	FileSystemError = 4,
	NotSupported = 5,
	NoMedia = 6,
	RegistryFinalized = 7,
	TimedOut = 8,
	Error = 100,
};

enum class ProviderKind {
	Audio = 0,
	Video = 1,
	Subtitles = 2,
};

struct ContextOptions {
	std::uint32_t abi_version = AbiVersion;
	bool use_thread_hooks = false;
	core::CoreHostThreadHooks thread_hooks;
};

struct ProviderDescriptorSnapshot {
	ProviderKind kind = ProviderKind::Audio;
	std::string name;
	std::string display_name;
	std::string unavailable_reason;
	bool hidden = false;
	bool available = true;
};

struct ProviderCatalogSnapshot {
	ProviderKind kind = ProviderKind::Audio;
	std::string preferred_provider;
	std::vector<ProviderDescriptorSnapshot> providers;
};

struct ProviderOpenAttemptSnapshot {
	std::string provider_name;
	std::string outcome;
	std::string detail;
};

struct ProviderOpenReportSnapshot {
	ProviderKind kind = ProviderKind::Audio;
	std::string preferred_provider;
	std::string selected_provider;
	std::vector<ProviderOpenAttemptSnapshot> attempts;
};

struct VideoOpenOptions {
	std::string path;
	std::string colormatrix;
	std::string preferred_provider;
	std::optional<std::size_t> max_cache_size_bytes = std::nullopt;
};

struct VideoInfoSnapshot {
	int frame_count = 0;
	int width = 0;
	int height = 0;
	double dar = 0.0;
	double fps = 0.0;
	bool is_vfr = false;
	bool has_audio = false;
	bool should_set_video_properties = true;
	bool wants_caching = false;
	std::string selected_provider;
	std::string decoder_name;
	std::string color_space;
	std::string real_color_space;
	std::string warning;
	std::string native_format_description;
	std::vector<int> keyframes;
};

struct VideoFrameSnapshot {
	std::vector<unsigned char> data;
	std::size_t width = 0;
	std::size_t height = 0;
	std::size_t pitch = 0;
	bool flipped = false;
};

struct AudioOpenOptions {
	std::string path;
	std::string preferred_provider;
};

struct AudioInfoSnapshot {
	std::int64_t num_samples = 0;
	std::int64_t decoded_samples = 0;
	int sample_rate = 0;
	int bytes_per_sample = 0;
	int channels = 0;
	bool float_samples = false;
	bool source_needs_cache = false;
	std::size_t logical_bytes = 0;
	std::size_t decoded_bytes = 0;
	std::string selected_provider;
	std::string storage_kind;
};

struct SubtitleOpenOptions {
	std::string path;
	std::string encoding;
};

struct SubtitleSaveOptions {
	std::string path;
	std::string encoding;
};

struct SubtitleInfoSnapshot {
	int row_count = 0;
	int style_count = 0;
	int attachment_count = 0;
	int script_info_count = 0;
	int width = 0;
	int height = 0;
	int resolution_type = 0;
	std::string format_name;
};

struct SubtitleStateSnapshot {
	std::uint64_t revision = 0;
	std::uint64_t row_change_revision = 0;
	std::size_t row_change_first_row = 0;
	std::size_t row_change_row_count = 0;
	std::uint32_t row_change_fields = 0;
	bool dirty = false;
};

struct SubtitleRowSnapshot {
	int line_id = -1;
	int row_index = -1;
	bool comment = false;
	int layer = 0;
	int start_ms = 0;
	int end_ms = 0;
	int margin_left = 0;
	int margin_right = 0;
	int margin_vertical = 0;
	std::string_view style;
	std::string_view actor;
	std::string_view effect;
	std::string_view text;
};

struct SubtitleRowTextEdit {
	std::size_t row = 0;
	std::string text;
};

enum class SubtitleRowPatchFields : std::uint32_t {
	None = 0,
	Comment = 1u << 0,
	Layer = 1u << 1,
	Start = 1u << 2,
	End = 1u << 3,
	Margins = 1u << 4,
	Style = 1u << 5,
	Actor = 1u << 6,
	Effect = 1u << 7,
	Text = 1u << 8,
};

enum class SubtitleChangeFields : std::uint32_t {
	None = 0,
	Text = 1u << 0,
	Time = 1u << 1,
	Metadata = 1u << 2,
	Structure = 1u << 3,
};

enum class SubtitleSortKey {
	Start = 0,
	End = 1,
	Style = 2,
	Actor = 3,
	Effect = 4,
	Layer = 5,
};

enum class SubtitlePasteOverFields : std::uint32_t {
	None = 0,
	Comment = 1u << 0,
	Layer = 1u << 1,
	Start = 1u << 2,
	End = 1u << 3,
	Style = 1u << 4,
	Actor = 1u << 5,
	MarginLeft = 1u << 6,
	MarginRight = 1u << 7,
	MarginVertical = 1u << 8,
	Effect = 1u << 9,
	Text = 1u << 10,
};

struct SubtitleRowPatch {
	std::size_t row = 0;
	std::uint32_t fields = 0;
	bool comment = false;
	int layer = 0;
	int start_ms = 0;
	int end_ms = 0;
	int margin_left = 0;
	int margin_right = 0;
	int margin_vertical = 0;
	std::string style;
	std::string actor;
	std::string effect;
	std::string text;
};

struct SubtitlePasteOverSource {
	bool comment = false;
	int layer = 0;
	int start_ms = 0;
	int end_ms = 0;
	int margin_left = 0;
	int margin_right = 0;
	int margin_vertical = 0;
	std::string style;
	std::string actor;
	std::string effect;
	std::string text;
};

class Context {
	core::CoreHostThreadContext thread_context;
	std::string last_error;
	ProviderOpenReportSnapshot last_audio_open_report;
	ProviderOpenReportSnapshot last_video_open_report;

public:
	explicit Context(ContextOptions options = {});

	core::CoreHostThreadContext const& ThreadContext() const;
	std::string const& LastError() const;
	void ClearLastError();
	void SetLastError(std::string error);
	void SetLastProviderOpenReport(ProviderKind kind, provider_selection_diagnostics::SelectionReport report);
	ProviderOpenReportSnapshot const& LastProviderOpenReport(ProviderKind kind) const;
};

class VideoSession {
	std::unique_ptr<VideoProvider> provider;
	VideoInfoSnapshot info;

public:
	VideoSession(std::unique_ptr<VideoProvider> provider, VideoInfoSnapshot info);
	~VideoSession();

	VideoInfoSnapshot const& Info() const;
	VideoFrameSnapshot FrameAt(int frame_number) const;
};

class AudioSession {
	std::unique_ptr<agi::AudioProvider> provider;
	AudioInfoSnapshot info;

public:
	AudioSession(std::unique_ptr<agi::AudioProvider> provider, AudioInfoSnapshot info);
	~AudioSession();

	AudioInfoSnapshot const& Info() const;
};

class SubtitleSession {
	std::unique_ptr<AssFile> file;
	SubtitleInfoSnapshot info;
	// Borrowed pointers into file->Events (an intrusive list). Every structural
	// mutation (InsertRow/DeleteRows/MoveSelectedRows/DuplicateSelectedRows/
	// SortRows/PasteOverRows) MUST be followed by RefreshRows(), otherwise these
	// pointers dangle. SubtitleRowSnapshot string_views returned by RowAt point
	// into AssDialogue members and are invalidated by ANY mutation; callers
	// across the C ABI must copy borrowed data before mutating or destroying the
	// session (documented in core_c_api.h). Keep this cache synchronized with
	// Events by routing all structural edits through the methods below.
	std::vector<AssDialogue *> rows;
	std::uint64_t revision = 0;
	std::uint64_t row_change_revision = 0;
	std::size_t row_change_first_row = 0;
	std::size_t row_change_row_count = 0;
	std::uint32_t row_change_fields = 0;
	mutable bool dirty = false;

	void RefreshRows();
	void MarkRowsChanged(std::size_t first_row, std::size_t row_count, std::uint32_t fields);

public:
	SubtitleSession(std::unique_ptr<AssFile> file,
	                SubtitleInfoSnapshot info,
	                std::vector<AssDialogue *> rows);
	~SubtitleSession();

	SubtitleInfoSnapshot const& Info() const;
	SubtitleStateSnapshot State() const;
	std::size_t RowCount() const;
	SubtitleRowSnapshot RowAt(std::size_t index) const;
	void SetRowText(std::size_t index, std::string_view text);
	std::size_t SetRowTexts(std::vector<SubtitleRowTextEdit> const& edits);
	std::size_t ApplyRowPatches(std::vector<SubtitleRowPatch> const& patches);
	std::size_t InsertRow(std::size_t row);
	void DeleteRows(std::size_t first_row, std::size_t row_count);
	bool MoveSelectedRows(std::vector<std::size_t> const& selected_rows,
	                      int direction,
	                      std::vector<std::size_t>& moved_selected_rows);
	bool DuplicateSelectedRows(std::vector<std::size_t> const& selected_rows,
	                           std::vector<std::size_t>& duplicated_rows);
	bool SortRows(SubtitleSortKey key,
	              bool selected_only,
	              std::vector<std::size_t> const& selected_rows,
	              std::vector<std::size_t>& sorted_selected_rows);
	std::size_t PasteOverRows(std::vector<std::size_t> const& target_rows,
	                          std::vector<SubtitlePasteOverSource> const& sources,
	                          std::uint32_t fields);
	void Save(SubtitleSaveOptions options) const;
};

std::unique_ptr<Context> CreateContext(ContextOptions options = {});
Status TryCreateContext(ContextOptions options, std::unique_ptr<Context>& context, std::string *error = nullptr);
Status RegisterBuiltinProviderFactories(Context& context);
Status FinalizeProviderRegistry(Context& context);
bool IsProviderRegistryFinalized(Context const& context);
Status GetProviderCatalog(Context& context,
                          ProviderKind kind,
                          std::string_view preferred_provider,
                          ProviderCatalogSnapshot& catalog);
Status OpenVideo(Context& context,
                 VideoOpenOptions options,
                 std::unique_ptr<VideoSession>& session);
Status OpenAudio(Context& context,
                 AudioOpenOptions options,
                 std::unique_ptr<AudioSession>& session);
Status OpenSubtitles(Context& context,
                     SubtitleOpenOptions options,
                     std::unique_ptr<SubtitleSession>& session);
Status SaveSubtitles(Context& context,
                     SubtitleSession const& session,
                     SubtitleSaveOptions options);
Status GetLastProviderOpenReport(Context& context,
                                 ProviderKind kind,
                                 ProviderOpenReportSnapshot& report);

} // namespace aegisub::core_api
