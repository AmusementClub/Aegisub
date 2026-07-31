#include "../../src/perf_trace.h"

namespace perf_trace {

namespace {
bool audio_category_enabled = false;
}

bool IsCategoryEnabled(Category) {
	return audio_category_enabled;
}

void SetAudioCategoryEnabledForSmoke(bool enabled) noexcept {
	audio_category_enabled = enabled;
}

void ObserveAudioContentTileEvent(AudioContentTileEvent const&) noexcept {
}

AudioUiDurationScope::AudioUiDurationScope(char const*, int, int) noexcept {
}

AudioUiDurationScope::~AudioUiDurationScope() noexcept = default;

void AudioUiDurationScope::SetDetails(int, int) noexcept {
}

}
