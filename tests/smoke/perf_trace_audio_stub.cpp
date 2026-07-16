#include "../../src/perf_trace.h"

namespace perf_trace {

AudioUiDurationScope::AudioUiDurationScope(char const*, int, int) noexcept {
}

AudioUiDurationScope::~AudioUiDurationScope() noexcept = default;

void AudioUiDurationScope::SetDetails(int, int) noexcept {
}

}
