#pragma once

#include "audio_marker.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

struct AudioMarkerPixel {
	const AudioMarker *marker = nullptr;
	int x = 0;
	AudioMarker::FeetStyle feet = AudioMarker::Feet_None;
	int priority = 0;
	std::size_t order = 0;
};

inline int AudioMarkerPixelPriority(AudioMarker::Kind kind) noexcept {
	switch (kind) {
		case AudioMarker::Kind::VideoPosition: return 5;
		case AudioMarker::Kind::Keyframe: return 4;
		case AudioMarker::Kind::Active: return 3;
		case AudioMarker::Kind::Selected: return 2;
		case AudioMarker::Kind::Inactive: return 1;
		case AudioMarker::Kind::Generic: return 0;
	}
	return 0;
}

/// Collapse dense markers which rasterize to the same device pixel. Results
/// retain provider order so neighbouring thick strokes keep the legacy z-order.
template<typename Projector>
std::vector<AudioMarkerPixel> AggregateAudioMarkersByPixel(
	AudioMarkerVector const& markers,
	int first_pixel,
	int last_pixel,
	Projector &&project) {
	if (last_pixel < first_pixel)
		return {};

	std::vector<std::optional<AudioMarkerPixel>> pixels(
		static_cast<std::size_t>(last_pixel - first_pixel + 1));
	for (std::size_t order = 0; order < markers.size(); ++order) {
		auto const *marker = markers[order];
		if (!marker)
			continue;
		auto const pixel = static_cast<int>(project(*marker));
		if (pixel < first_pixel || pixel > last_pixel)
			continue;

		auto const priority = AudioMarkerPixelPriority(marker->GetKind());
		auto &slot = pixels[static_cast<std::size_t>(pixel - first_pixel)];
		if (!slot || priority > slot->priority) {
			slot = AudioMarkerPixel{
				marker,
				pixel,
				marker->GetFeet(),
				priority,
				order,
			};
		}
		else if (priority == slot->priority) {
			slot->marker = marker;
			slot->feet = static_cast<AudioMarker::FeetStyle>(
				static_cast<int>(slot->feet) | static_cast<int>(marker->GetFeet()));
			slot->order = order;
		}
	}

	std::vector<AudioMarkerPixel> result;
	result.reserve(std::min(markers.size(), pixels.size()));
	for (auto &pixel : pixels)
		if (pixel)
			result.push_back(*pixel);
	std::sort(result.begin(), result.end(), [](auto const& lhs, auto const& rhs) {
		return lhs.order < rhs.order;
	});
	return result;
}
