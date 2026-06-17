#pragma once

#include "provider_selection_diagnostics.h"

#include <libaegisub/fs_fwd.h>

#include <optional>
#include <string>
#include <utility>

namespace aegisub::media_open {

enum class MediaKind {
	Audio,
	Video,
	Subtitles,
};

enum class OpenStatus {
	NotStarted,
	Opened,
	Cancelled,
	FileNotFound,
	FileSystemError,
	NotSupported,
	NoMedia,
	Error,
};

struct MediaOpenRequest {
	MediaKind kind = MediaKind::Video;
	agi::fs::path path;
	std::optional<std::string> preferred_provider;
	std::optional<int> track_index;
	std::string color_matrix;
	bool use_cache = true;
};

struct MediaOpenResult {
	MediaKind kind = MediaKind::Video;
	OpenStatus status = OpenStatus::NotStarted;
	bool opened = false;
	std::string selected_provider;
	std::string decoder_name;
	std::string warning;
	std::string error;
	provider_selection_diagnostics::SelectionReport provider_report;
};

inline MediaOpenResult Opened(MediaKind kind,
                              std::string selected_provider,
                              provider_selection_diagnostics::SelectionReport provider_report = {}) {
	MediaOpenResult result;
	result.kind = kind;
	result.status = OpenStatus::Opened;
	result.opened = true;
	result.selected_provider = std::move(selected_provider);
	result.provider_report = std::move(provider_report);
	return result;
}

inline MediaOpenResult Failed(MediaKind kind,
                              OpenStatus status,
                              std::string error,
                              provider_selection_diagnostics::SelectionReport provider_report = {}) {
	MediaOpenResult result;
	result.kind = kind;
	result.status = status;
	result.error = std::move(error);
	result.provider_report = std::move(provider_report);
	return result;
}

} // namespace aegisub::media_open
