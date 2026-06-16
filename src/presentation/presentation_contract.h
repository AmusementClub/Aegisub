#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace aegisub::presentation {

using Revision = std::uint64_t;

enum class CommandFlag : std::uint32_t {
	Validate = 1u << 0,
	Toggle = 1u << 1,
	Radio = 1u << 2,
	DynamicName = 1u << 3,
	DynamicHelp = 1u << 4,
	DynamicIcon = 1u << 5,
};

constexpr std::uint32_t ToMask(CommandFlag flag) noexcept {
	return static_cast<std::uint32_t>(flag);
}

constexpr bool HasFlag(std::uint32_t flags, CommandFlag flag) noexcept {
	return (flags & ToMask(flag)) != 0;
}

constexpr std::uint32_t operator|(CommandFlag left, CommandFlag right) noexcept {
	return ToMask(left) | ToMask(right);
}

constexpr std::uint32_t operator|(std::uint32_t left, CommandFlag right) noexcept {
	return left | ToMask(right);
}

struct CommandDescriptor {
	std::string id;
	std::string menu_text_key;
	std::string display_text_key;
	std::string help_text_key;
	std::string icon_name;
	std::uint32_t flags = 0;
};

struct CommandState {
	std::string id;
	bool enabled = true;
	bool active = false;
	Revision revision = 0;
	std::optional<std::string> display_text;
	std::optional<std::string> help_text;
	std::optional<std::string> icon_name;
};

enum class SubtitleGridColumnKind {
	Layer,
	Start,
	End,
	Style,
	Actor,
	Effect,
	MarginLeft,
	MarginRight,
	MarginVertical,
	Text,
	LineNumber,
	Cps,
};

inline constexpr auto SubtitleGridColumnIdLineNumber = "line_number";
inline constexpr auto SubtitleGridColumnIdLayer = "layer";
inline constexpr auto SubtitleGridColumnIdStart = "start";
inline constexpr auto SubtitleGridColumnIdEnd = "end";
inline constexpr auto SubtitleGridColumnIdStyle = "style";
inline constexpr auto SubtitleGridColumnIdActor = "actor";
inline constexpr auto SubtitleGridColumnIdEffect = "effect";
inline constexpr auto SubtitleGridColumnIdMarginLeft = "margin_left";
inline constexpr auto SubtitleGridColumnIdMarginRight = "margin_right";
inline constexpr auto SubtitleGridColumnIdMarginVertical = "margin_vertical";
inline constexpr auto SubtitleGridColumnIdCps = "cps";
inline constexpr auto SubtitleGridColumnIdText = "text";

struct SubtitleGridColumnDescriptor {
	std::string id;
	std::string title_key;
	SubtitleGridColumnKind kind = SubtitleGridColumnKind::Text;
	bool visible = true;
};

struct SubtitleGridRowState {
	bool selected = false;
	bool active = false;
	bool visible_at_current_frame = false;
	bool collides_with_active = false;
};

struct SubtitleGridRow {
	int line_id = -1;
	int row_index = -1;
	bool comment = false;
	int layer = 0;
	int start_ms = 0;
	int end_ms = 0;
	std::array<int, 3> margins{{0, 0, 0}};
	std::string style;
	std::string actor;
	std::string effect;
	std::string text;
	SubtitleGridRowState state;
};

struct VisibleSubtitleRowsRequest {
	int first_row = 0;
	int row_count = 0;
	Revision known_revision = 0;
	std::vector<std::string> column_ids;
};

struct SubtitleGridWindow {
	Revision revision = 0;
	int first_row = 0;
	int total_rows = 0;
	std::vector<SubtitleGridRow> rows;
};

enum class SubtitleGridDiffKind {
	Unknown,
	Reset,
	RowsChanged,
	SelectionChanged,
};

struct SubtitleGridDiff {
	Revision before_revision = 0;
	Revision after_revision = 0;
	SubtitleGridDiffKind kind = SubtitleGridDiffKind::Unknown;
	bool requires_full_refresh = false;
	std::vector<SubtitleGridRow> upserted_rows;
	std::vector<int> removed_line_ids;
};

enum class RenderPayloadStorage {
	InlineBytes,
	SharedMemory,
	NativeTextureHandle,
};

enum class PixelFormat {
	Bgra8Premultiplied,
	Rgba8,
	Gray8,
	Native,
};

struct RenderPayload {
	RenderPayloadStorage storage = RenderPayloadStorage::InlineBytes;
	std::string handle;
	std::vector<std::uint8_t> bytes;
};

struct RenderedBitmap {
	int width = 0;
	int height = 0;
	int stride = 0;
	PixelFormat pixel_format = PixelFormat::Bgra8Premultiplied;
	RenderPayload payload;
};

struct AudioTileRequest {
	int start_ms = 0;
	int duration_ms = 0;
	double ms_per_pixel = 1.0;
	int width = 0;
	int height = 0;
	Revision style_revision = 0;
};

struct VideoFrameRequest {
	int frame_number = 0;
	int target_width = 0;
	int target_height = 0;
	bool include_subtitles = true;
	Revision subtitle_revision = 0;
};

}
