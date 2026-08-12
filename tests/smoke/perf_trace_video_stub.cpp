#include "../../src/perf_trace.h"

namespace perf_trace {

VideoUiDurationScope::VideoUiDurationScope(char const*, int, int) noexcept {
}

VideoUiDurationScope::~VideoUiDurationScope() noexcept = default;

void VideoUiDurationScope::SetDetails(int, int) noexcept {
}

}
