#include "subtitle_grid_renderer_contract.h"

#include <unicode/ubidi.h>
#include <unicode/stringpiece.h>
#include <unicode/unistr.h>

#include <cmath>
#include <cstdint>
#include <limits>

namespace aegisub::grid {

SubtitleGridParagraphDirection ResolveSubtitleGridParagraphDirection(
	std::string_view utf8) {
	if (utf8.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
		return SubtitleGridParagraphDirection::LeftToRight;
	auto const text = icu::UnicodeString::fromUTF8(icu::StringPiece(
		utf8.data(), static_cast<std::int32_t>(utf8.size())));
	return ubidi_getBaseDirection(text.getBuffer(), text.length()) == UBIDI_RTL
		? SubtitleGridParagraphDirection::RightToLeft
		: SubtitleGridParagraphDirection::LeftToRight;
}

bool SupportsSkiaGridContentScale(
	double content_scale,
	bool logical_coordinates_are_device_pixels) noexcept {
	if (!std::isfinite(content_scale) || content_scale <= 0.0)
		return false;
	return logical_coordinates_are_device_pixels
		|| std::abs(content_scale - 1.0) <= 0.001;
}

SubtitleGridRendererState::SubtitleGridRendererState(bool skia_requested) noexcept
:	requested_(skia_requested ? SubtitleGridRendererBackend::Skia : SubtitleGridRendererBackend::Wx),
	active_(requested_) {
}

void SubtitleGridRendererState::MarkPresented() noexcept {
	if (active_ == SubtitleGridRendererBackend::Skia && !failed_)
		presented_ = true;
}

SubtitleGridRendererFailureAction SubtitleGridRendererState::MarkFailed(std::string_view) noexcept {
	if (active_ != SubtitleGridRendererBackend::Skia || failed_)
		return SubtitleGridRendererFailureAction::None;
	failed_ = true;
	active_ = SubtitleGridRendererBackend::Wx;
	return SubtitleGridRendererFailureAction::QueueWxFullRefresh;
}

char const *SubtitleGridRendererBackendName(SubtitleGridRendererBackend backend) noexcept {
	return backend == SubtitleGridRendererBackend::Skia ? "skia" : "wx";
}

} // namespace aegisub::grid
