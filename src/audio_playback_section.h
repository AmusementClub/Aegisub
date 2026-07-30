#pragma once

#include <algorithm>
#include <limits>

namespace aegisub::audio_playback_section {
inline constexpr int DefaultDurationMs = 500;
inline constexpr int MaximumDurationMs = 36000;

inline constexpr char BeforeOption[] = "Audio/Play/Section/Before";
inline constexpr char AfterOption[] = "Audio/Play/Section/After";
inline constexpr char BeginOption[] = "Audio/Play/Section/Begin";
inline constexpr char EndOption[] = "Audio/Play/Section/End";

struct Range {
	int begin;
	int end;
};

inline int NormalizeDuration(int duration_ms) {
	return std::clamp(duration_ms, 0, MaximumDurationMs);
}

inline Range Before(int selection_begin, int duration_ms) {
	auto const begin = std::max<long long>(
		static_cast<long long>(selection_begin) - NormalizeDuration(duration_ms),
		std::numeric_limits<int>::min());
	return {static_cast<int>(begin), selection_begin};
}

inline Range After(int selection_end, int duration_ms) {
	auto const end = std::min<long long>(
		static_cast<long long>(selection_end) + NormalizeDuration(duration_ms),
		std::numeric_limits<int>::max());
	return {selection_end, static_cast<int>(end)};
}

inline Range Begin(int selection_begin, int selection_end, int duration_ms) {
	return {
		selection_begin,
		selection_begin + std::min(NormalizeDuration(duration_ms), selection_end - selection_begin)};
}

inline Range End(int selection_begin, int selection_end, int duration_ms) {
	return {
		selection_end - std::min(NormalizeDuration(duration_ms), selection_end - selection_begin),
		selection_end};
}
}
