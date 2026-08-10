#pragma once

#include <string_view>

namespace aegisub::grid {

enum class SubtitleGridRendererBackend {
	Wx,
	Skia
};

enum class SubtitleGridRendererFailureAction {
	None,
	QueueWxFullRefresh
};

enum class SubtitleGridParagraphDirection {
	LeftToRight,
	RightToLeft
};

/// Resolve the Unicode paragraph base direction. Neutral-only and malformed
/// input uses the UI's left-to-right default.
SubtitleGridParagraphDirection ResolveSubtitleGridParagraphDirection(
	std::string_view utf8);

/// The current CPU raster bridge is one-to-one. Ports whose logical client
/// coordinates differ from device pixels must use wx until a scaled backing
/// surface and presentation bridge are implemented.
bool SupportsSkiaGridContentScale(
	double content_scale,
	bool logical_coordinates_are_device_pixels) noexcept;

class SubtitleGridRendererState final {
	SubtitleGridRendererBackend requested_ = SubtitleGridRendererBackend::Wx;
	SubtitleGridRendererBackend active_ = SubtitleGridRendererBackend::Wx;
	bool presented_ = false;
	bool failed_ = false;

public:
	explicit SubtitleGridRendererState(bool skia_requested) noexcept;

	SubtitleGridRendererBackend Requested() const noexcept { return requested_; }
	SubtitleGridRendererBackend Active() const noexcept { return active_; }
	bool Presented() const noexcept { return presented_; }
	bool Failed() const noexcept { return failed_; }

	void MarkPresented() noexcept;
	SubtitleGridRendererFailureAction MarkFailed(std::string_view reason) noexcept;
};

char const *SubtitleGridRendererBackendName(SubtitleGridRendererBackend backend) noexcept;

} // namespace aegisub::grid
