#pragma once

#include "source_frame.h"
#include "video_frame.h"

#include <libaegisub/color.h>

#include <utility>

namespace aegisub::color_pick {

/// Statistic applied to the pixels of the accepted flood-fill region.
enum class Statistic {
	Median,
	Mean,
};

struct Options {
	/// Per-channel BT.601 Y tolerance used both for edge grouping and region
	/// growth. Half of a just-noticeable delta keeps mild compression noise
	/// out while stopping at real colour changes.
	int luma_tolerance = 12;
	/// Cb/Cr tolerance in the same units.
	int chroma_tolerance = 18;
	/// Radius of the window around the clicked pixel used to derive the seed
	/// colour (1 = 3x3).
	int seed_radius = 1;
	/// Radius of the window used when the connected region degenerates and the
	/// pick falls back to a plain local median (1 = 3x3).
	int fallback_radius = 1;
	/// Upper bound on the number of pixels admitted to the region; BFS stops
	/// (and reports capped) once reached.
	int max_region_pixels = 32768;
	/// Upper bound on the distance between any seed patch pixel and the farthest
	/// corner of the region bounding box; BFS stops beyond it.
	int max_region_extent = 320;
	Statistic statistic = Statistic::Median;
};

struct Result {
	/// Picked colour from the raw BGRA frame; alpha is always zero.
	agi::Color color;
	/// Number of pixels that made up the accepted region (or the fallback
	/// window when fallback is true).
	int pixels = 0;
	int bbox_w = 0;
	int bbox_h = 0;
	/// The seed was judged an anti-alias/compression blend of two stable blocks
	/// and snapped onto one of them before growing.
	bool edge_snapped = false;
	/// The region degenerated and a local-median fallback was used instead.
	bool fallback = false;
	/// Growth stopped at one of the configured caps.
	bool capped = false;
	/// 0..1 separation score between the picked region and its surrounding
	/// ring: high on strong flat-on-flat boundaries, low near low-contrast or
	/// noisy edges where the result may be unstable.
	double confidence = 0.0;
};

/// Pick a colour from `frame` at source pixel (x, y).
///
/// The frame is read as BGRA with row stride `pitch`; `flipped` frames store
/// their first scanline at the end of the buffer, matching VideoFrame.
///
/// A structurally unusable frame (no data, no extent, or stride smaller than
/// width*4) returns an empty Result{} without attempting anything. A valid
/// frame with out-of-range coordinates uses only the clamped fallback window.
Result PickColor(VideoFrame const& frame, int x, int y, Options const& options = {});

/// Map a point in the provider's displayed space — the coordinate space of the
/// video display and of VideoProvider::GetWidth/GetHeight, i.e. the visible
/// rect — to full-storage-frame pixel coordinates for indexing a raw BGRA
/// VideoFrame, which still contains any clean-aperture/crop margins. The
/// geometry may be the unbaked provider geometry; it is baked here. Input
/// coordinates are clamped into the visible rect. A degenerate geometry
/// yields (-1, -1) so callers can fail the pick instead of sampling garbage.
std::pair<int, int> MapDisplayPointToStorage(SourceFrameGeometry const& geometry, double x, double y);

}
