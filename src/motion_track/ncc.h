#pragma once

// Integer accumulators are int64 and bit-exact between the scalar reference
// and the Highway kernel; the final normalized value is one double division,
// so both paths agree within 1e-6.

#include "types.h"

#include <cstddef>
#include <vector>

namespace aegisub::motion_track {

// Computes zero-mean NCC between templ (top-left anchored) and the image
// window whose top-left sits at (ox, oy) relative to the image view origin.
// The window must lie fully inside image. Returns false for degenerate input
// or flat sides (denominator < 1e-6).
//
// out_slope (optional metrics export, no extra pass): the least-squares
// contrast slope of the window against the template -- a in
// window ~= a * templ + b, so 1.0 means unchanged contrast and 0.0 a
// collapsed one. Zero-mean NCC itself is invariant to a*x+b brightness
// transforms, which is exactly why the slope is the observable signal of a
// fade. Filled whenever the input geometry is valid, independently of the
// return value: a flat window against a contrasted template reports exactly
// 0.0 (a completely faded window) even though its NCC is undefined, while a
// contrast-less template reports the pinned 1.0 -- with no template contrast
// there is no collapse to measure, so a flat seed can never read as faded.
// Not clamped; callers apply their own bounds.
bool ZeroMeanNccScalar(
	GrayView templ, GrayView image, int ox, int oy, double& out,
	double *out_slope = nullptr);

struct BestNccResult {
	double ncc = 0.0;
	int offset_x = 0; // template top-left relative to image top-left
	int offset_y = 0;
	bool found = false; // false when every candidate window was flat/invalid
};

// Tie-break preference for the offset scan. Defaulted construction keeps the
// historical contract (ties resolve to the first candidate in scan order:
// smallest oy, then ox). When enabled, a candidate whose score is within
// 1e-12 of the running best replaces it whenever it is strictly nearer to
// (center_x, center_y): on flat or periodic content a whole family of
// candidates scores identically, and first-in-scan-order then biases every
// frame toward the scan origin, which accumulates into a per-frame creep.
// The comparison runs on the same bit-exact doubles on the scalar and the
// Highway path, so both paths pick identical offsets with or without the
// preference.
// Adapted from croni1012/Aegisub src/typesetting_auto_motion.cpp
// (ISC license) -- FindPatch's
// "a tie goes to the smaller move" rule.
struct NccTieBreak {
	bool enabled = false;
	int center_x = 0;
	int center_y = 0;
};

// Scans inclusive offset rectangle [min_ox..max_ox] x [min_oy..max_oy],
// skipping flat windows. Ties resolve per NccTieBreak (default: first
// candidate in scan order, i.e. smallest y, then x).
BestNccResult FindBestNccScalar(GrayView templ, GrayView image,
								int min_ox, int min_oy, int max_ox, int max_oy,
								NccTieBreak tie = {});

// Production entry point: Highway kernel when compiled with
// AEGISUB_WITH_HIGHWAY, otherwise the scalar reference. Both implement the
// same NccTieBreak semantics on identical doubles.
BestNccResult FindBestNcc(GrayView templ, GrayView image,
						  int min_ox, int min_oy, int max_ox, int max_oy,
						  NccTieBreak tie = {});

// True when a candidate window displaced from (around_x, around_y) by more
// than (excl_x, excl_y) on either axis is a strict local maximum of the NCC
// surface and scores >= ratio * min_score — the signature of a repetitive
// texture where a whole period away matches as well as the accepted peak.
// The local-maximum requirement keeps the accepted peak's own shoulder from
// counting as a competitor. Returns early once such a peak is found.
bool HasCompetingNccPeak(GrayView templ, GrayView image,
	int min_ox, int min_oy, int max_ox, int max_oy,
	int around_x, int around_y, int excl_x, int excl_y,
	double ratio, double min_score);

// Parabolic subpixel refinement around an integer peak. Returns delta in
// [-0.75, 0.75]; 0 when the center is not a local maximum (e.g. a
// candidate clamped at the search-window edge) or the curvature
// denominator vanishes.
double ParabolicSubpixel(double left, double center, double right);

// --- Occlusion-robust acceptance scoring -------------------------------
//
// Adapted from croni1012/Aegisub src/typesetting_auto_motion.cpp
// (ISC license): the upstream tracker
// scores a match as many independent small patches and lets a robust
// statistic decide which patches still see the tracked texture, so a partly
// occluded target keeps tracking on its visible part instead of failing the
// whole-template score. Only that selection rule lives here; callers provide
// per-block residual scores and recompute their own metric on the
// surviving blocks.

struct RobustInlierParams {
	/// Robust cutoff multiplier: a block is an outlier when its residual
	/// exceeds median + outlier_k * 1.4826 * MAD. 3 keeps ~99% of Gaussian
	/// inliers while rejecting structurally different blocks (occluders).
	double outlier_k = 3.0;
	/// Selection rounds: the threshold is recomputed on the kept blocks
	/// after each round (trimmed re-estimate), capped to keep the cost
	/// bounded.
	int max_rounds = 2;
	/// Fail (report no inlier set) when more than this fraction of blocks
	/// would be rejected: past half the template the "match" is no longer a
	/// partially occluded one.
	double max_reject_fraction = 0.5;
	/// Fewer kept blocks than this carry too little evidence to rescore on.
	std::size_t min_blocks = 4;
};

// block_residuals: one robust score per spatial block (callers define the
// blocking; the backends bin 8x8 template blocks). On success fills `kept`
// (same size, true = inlier) and returns true. Returns false without
// touching `kept` when the input has fewer than min_blocks blocks, or when
// applying the cutoff would reject more than max_reject_fraction of them
// (or leave fewer than min_blocks).
bool SelectInlierBlocks(std::vector<double> const& block_residuals,
						RobustInlierParams const& params,
						std::vector<bool>& kept);

// Pixel-level inlier selection for the occlusion gate. The gate is a noise
// estimate anchored on the low side of the distribution — level at the
// lower quartile, scale over the lower half — so it stays on the
// visible-pixel side for any occlusion within max_reject_fraction (a plain
// median+MAD would flip once half the template is occluded and size the
// gate to the occluder), and it follows a global noise or brightness
// shift instead of a hardwired threshold. A hard block cutoff would
// quantize the occlusion boundary to whole blocks; per-pixel selection
// honours the area cap exactly. Returns false without touching `kept`
// when fewer than min_blocks pixels would survive or the rejection
// exceeds max_reject_fraction.
bool SelectInlierPixels(std::vector<double> const& pixel_diffs,
						RobustInlierParams const& params,
						std::vector<bool>& kept);

} // namespace aegisub::motion_track
