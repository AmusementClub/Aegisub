#include "translation_backend.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace aegisub::motion_track {

TrackSample const* FindSample(
	std::vector<TrackSample> const& samples, int frame) {
	auto it = std::lower_bound(
		samples.begin(), samples.end(), frame,
		[](TrackSample const& sample, int wanted) {
			return sample.frame < wanted;
		});
	if (it != samples.end() && it->frame == frame)
		return &*it;
	return nullptr;
}

namespace {

std::uint64_t HashBytes(std::uint8_t const* data, size_t size) {
	std::uint64_t hash = 14695981039346656037ull; // FNV-1a 64-bit offset basis
	for (size_t i = 0; i < size; ++i) {
		hash ^= data[i];
		hash *= 1099511628211ull;
	}
	return hash;
}

bool ValidView(GrayView const& view) {
	return view.data != nullptr && view.width > 0 && view.height > 0
	    && view.stride >= view.width;
}

// Median absolute difference between template and image window at (ox, oy),
// on the 0..255 gray scale. Window must lie fully inside the image.
double MadResidual(GrayView templ, GrayView image, int ox, int oy) {
	std::vector<int> diffs;
	diffs.reserve(size_t(templ.width) * size_t(templ.height));
	for (int y = 0; y < templ.height; ++y) {
		auto const* trow = templ.data + int64_t(y) * templ.stride;
		auto const* irow = image.data + int64_t(oy + y) * image.stride + ox;
		for (int x = 0; x < templ.width; ++x)
			diffs.push_back(std::abs(int(trow[x]) - int(irow[x])));
	}
	auto const mid = diffs.begin() + (diffs.size() - 1) / 2;
	std::nth_element(diffs.begin(), mid, diffs.end());
	return double(*mid);
}

// --- Occlusion-robust scoring -------------------------------------------
// Adapted from croni1012/Aegisub src/typesetting_auto_motion.cpp
// (ISC license): the upstream tracker
// scores a match as many independent small patches and lets the patch
// consensus decide the offset, so occluded patches cannot drag the result.
// RobustInlierSearch below is the whole-template analogue.

// Voting block edge; 8x8 gives each block enough pixels for a stable NCC
// while keeping ~20 independent votes per typical ROI.
constexpr int kOcclusionBlock = 8;

// Per-pixel |template - window| at (ox, oy), row-packed. Window must lie
// fully inside the image.
std::vector<double> PixelAbsDiffs(
	std::uint8_t const *templ, int roi_w, int roi_h,
	GrayView image, int ox, int oy) {
	std::vector<double> diffs(size_t(roi_w) * size_t(roi_h));
	for (int y = 0; y < roi_h; ++y) {
		auto const *trow = templ + int64_t(y) * roi_w;
		auto const *irow = image.data + int64_t(oy + y) * image.stride + ox;
		for (int x = 0; x < roi_w; ++x)
			diffs[size_t(y) * roi_w + x] =
				double(std::abs(int(trow[x]) - int(irow[x])));
	}
	return diffs;
}

// Zero-mean NCC and the median |difference| over the kept pixels only.
// Integer arithmetic mirrors ZeroMeanNccScalar. A window not lying fully
// inside the image scores false, like ZeroMeanNccScalar's own bounds check.
bool MaskedNccAt(std::uint8_t const *templ, int roi_w, int roi_h,
				 GrayView image, int ox, int oy,
				 std::vector<bool> const& kept,
				 double& out_ncc, double& out_median) {
	if (ox < 0 || oy < 0 || ox + roi_w > image.width || oy + roi_h > image.height)
		return false;
	long long sum_t = 0, sum_i = 0, sum_t2 = 0, sum_i2 = 0, sum_ti = 0;
	std::vector<double> diffs;
	long long kept_px = 0;
	for (int y = 0; y < roi_h; ++y) {
		auto const *trow = templ + int64_t(y) * roi_w;
		auto const *irow =
			image.data + int64_t(oy + y) * image.stride + ox;
		for (int x = 0; x < roi_w; ++x) {
			if (!kept[size_t(y) * roi_w + x])
				continue;
			int const tv = trow[x];
			int const iv = irow[x];
			sum_t += tv;
			sum_i += iv;
			sum_t2 += tv * tv;
			sum_i2 += iv * iv;
			sum_ti += tv * iv;
			diffs.push_back(std::abs(tv - iv));
			++kept_px;
		}
	}
	if (kept_px < 1)
		return false;
	long long const n = kept_px;
	long long const num = n * sum_ti - sum_t * sum_i;
	long long const factor_t = n * sum_t2 - sum_t * sum_t;
	long long const factor_i = n * sum_i2 - sum_i * sum_i;
	if (factor_t < 0 || factor_i < 0)
		return false;
	double const denom =
		std::sqrt(double(factor_t)) * std::sqrt(double(factor_i));
	if (denom < 1e-6)
		return false;
	out_ncc = double(num) / denom;
	auto const mid = diffs.begin() + (diffs.size() - 1) / 2;
	std::nth_element(diffs.begin(), mid, diffs.end());
	out_median = double(*mid);
	return true;
}

GrayView Downsample(std::vector<std::uint8_t>& storage, GrayView src) {
	int const w = src.width / 2;
	int const h = src.height / 2;
	if (w <= 0 || h <= 0)
		return GrayView{};
	storage.assign(size_t(w) * size_t(h), 0);
	for (int y = 0; y < h; ++y) {
		auto const* r0 = src.data + int64_t(2 * y) * src.stride;
		auto const* r1 = r0 + src.stride;
		auto* dst = storage.data() + size_t(y) * size_t(w);
		for (int x = 0; x < w; ++x)
			dst[x] = static_cast<std::uint8_t>(
				(unsigned(r0[2 * x]) + r0[2 * x + 1] + r1[2 * x] + r1[2 * x + 1])
				/ 4u);
	}
	return GrayView{storage.data(), w, w, h};
}

// --- Held/duplicate-frame pre-check -------------------------------------
// Adapted from croni1012/Aegisub src/typesetting_auto_motion.cpp
// (ISC license): the upstream tracker detects that a frame's tracked
// region is unchanged since the previous frame and reuses the previous
// transform verbatim, so sub-pixel correlation on (near-)identical pixels
// cannot introduce position noise. The gates below are this repo's
// analogue of upstream's IsUnchangedRegion, rescaled for full-frame
// decoder dither: upstream assumed sparse 1-4 level noise (mean <= 0.75,
// 0.5% of pixels beyond 4), while inter-frame encoders routinely dither
// every luma pixel by +-1..3.

// Stage 2 gate: mean |difference| (0..255 scale) at which the window still
// counts as held. Calibrated against the actual dither distribution: a
// full-frame encoder nudge moves every luma pixel by a signed 1..3 — no
// pixel stays unchanged — so two independent frames' dithers
// a, b ~ U{+-1,+-2,+-3} differ by E|a-b| = 88/36 ~= 2.44, not the ~1.9 a
// noise model that leaves pixels unchanged suggests. The window mean over
// N pixels concentrates at sd ~= 3.56/sqrt(N) (0.11 for a 40x28 ROI; the
// noisy-static suite's realized frame-to-frame comparisons span
// 2.34..2.54), so 3.0 clears pure noise by ~5 sd. Real motion stays far
// beyond it: 1 px/frame on the synthetic suites' textures measures
// 8.3..13 mean |difference|, and slower persistent motion cannot chain
// under the gate because held steps keep comparing against the last
// full-search window, so accumulated displacement crosses 3.0 and the full
// search recovers.
constexpr double kHeldMeanAbsDiff = 3.0;
// Stage 2 gate: pixels differing by more than this count as materially
// changed. Double-sided +-3 dither can never exceed 6, so pure noise
// scores zero; real motion pushes a large fraction of edge pixels past it.
constexpr int kHeldMateriallyChanged = 6;
// Stage 2 gate: fraction of materially changed pixels still allowed.
constexpr double kHeldChangedFraction = 0.01;

// --- Fade detection, phase A ---------------------------------------------
// A gated frame only counts as faded when the window at the measurement
// position is a certified scaled copy of the template: its plain NCC must
// stay >= this floor. A fade is exactly a*x + b, so its NCC survives near
// 1.0 until 8-bit quantization collapses the contrast (below roughly 2% on
// typical content -- signal sigma 0.02*35 against +-0.5 rounding noise),
// while occlusion or misalignment pushes the window's NCC far below the
// floor by the depths that trip the NCC/residual gates. Without this floor
// a bland occluder (least-squares slope ~0) would hijack the fade path and
// silently disable the occlusion rescue.
constexpr double kFadeCorrelationFloor = 0.85;
// A recovery probe's confident match must sit more than this many pixels
// (Chebyshev) from the held position before the target counts as moved
// while invisible; within it, subpixel and pyramid noise are the norm.
constexpr int kFadeProbeLostPx = 2;

// FNV-1a over the win_w x win_h window of `image` at (ox, oy)
// (stride-aware), the Stage 1 signature of a held frame. Window must lie
// fully inside the image.
std::uint64_t HashWindow(GrayView image, int ox, int oy, int win_w, int win_h) {
	std::uint64_t hash = 14695981039346656037ull; // FNV-1a 64-bit basis
	for (int y = 0; y < win_h; ++y) {
		auto const *irow = image.data + int64_t(oy + y) * image.stride + ox;
		for (int x = 0; x < win_w; ++x) {
			hash ^= irow[x];
			hash *= 1099511628211ull;
		}
	}
	return hash;
}

// Stage 1 (exact signature) + Stage 2 (dither gate) over the whole window:
// `prev` and the window at (ox, oy) must agree in size. Fills
// out_mean_abs_diff with the mean |difference| for the held result's
// residual. Window must lie fully inside the image.
bool WindowIsHeld(GrayView prev, GrayView image, int ox, int oy,
				  double& out_mean_abs_diff) {
	if (HashWindow(prev, 0, 0, prev.width, prev.height)
		== HashWindow(image, ox, oy, prev.width, prev.height)) {
		out_mean_abs_diff = 0.0;
		return true;
	}
	long long sum = 0;
	long long changed = 0;
	for (int y = 0; y < prev.height; ++y) {
		auto const *prow = prev.data + int64_t(y) * prev.stride;
		auto const *irow = image.data + int64_t(oy + y) * image.stride + ox;
		for (int x = 0; x < prev.width; ++x) {
			int const diff = std::abs(int(prow[x]) - int(irow[x]));
			sum += diff;
			if (diff > kHeldMateriallyChanged)
				++changed;
		}
	}
	double const count = double(prev.width) * double(prev.height);
	out_mean_abs_diff = double(sum) / count;
	return out_mean_abs_diff <= kHeldMeanAbsDiff && double(changed) <= kHeldChangedFraction * count;
}

} // namespace

TranslationTrackerBackend::TranslationTrackerBackend(
	TranslationTrackerConfig config)
	: config_(config) {}

TrackModel TranslationTrackerBackend::Model() const {
	return TrackModel::Translation;
}

std::string_view TranslationTrackerBackend::Name() const noexcept {
	return "ncc-pyramid";
}

void TranslationTrackerBackend::ClearTemplate() {
	has_template_ = false;
	roi_w_ = 0;
	roi_h_ = 0;
	template_pixels_.clear();
	template_pixels_.shrink_to_fit();
	held_has_previous_ = false;
	prev_window_.clear();
	prev_window_.shrink_to_fit();
	fade_phase_ = FadePhase::NotFaded;
	fade_frames_since_probe_ = 0;
	fade_last_slope_ = -1.0;
	fade_last_mean_abs_ = 0.0;
}

void TranslationTrackerBackend::BeginDirectionArm() {
	// A new arm starts from the seed with no tracking history: the previous
	// arm's last match, held window and fade state describe frames on the
	// other side of the seed and must not bias this arm's first steps (the
	// template and config stay -- the seed is the same object).
	held_has_previous_ = false;
	prev_window_.clear();
	prev_window_.shrink_to_fit();
	fade_phase_ = FadePhase::NotFaded;
	fade_frames_since_probe_ = 0;
	fade_last_slope_ = -1.0;
	fade_last_mean_abs_ = 0.0;
}

TrackStatus TranslationTrackerBackend::Reset(TrackerSeed const& seed) {
	ClearTemplate();

	if (seed.model != TrackModel::Translation)
		return TrackStatus::Unsupported;
	if (!ValidView(seed.template_gray))
		return TrackStatus::InvalidInput;

	roi_w_ = seed.template_gray.width;
	roi_h_ = seed.template_gray.height;
	template_pixels_.resize(size_t(roi_w_) * size_t(roi_h_));
	for (int y = 0; y < roi_h_; ++y) {
		std::copy_n(
			seed.template_gray.data + int64_t(y) * seed.template_gray.stride,
			roi_w_,
			template_pixels_.begin() + int64_t(y) * roi_w_);
	}
	// Fade reference statistic: the worst-case median |template - window| a
	// certified scaled copy of this template can reach while fading, matching
	// MadResidual's median semantics. A copy at slope a fading toward level L
	// differs by (1 - a) * median |t - L|; that median is maximal at the
	// extrema L in {0, 255} (fade to black/white: median |t - 0| = med and
	// median |t - 255| = 255 - med), while median |t - med| covers only fades
	// toward the template's own median. Take the max of the three so the
	// residual gate's fade exemption is armed for every fade direction (see
	// the ResidualHigh exemption in Step).
	{
		std::vector<double> vals(template_pixels_.size());
		for (size_t i = 0; i < vals.size(); ++i)
			vals[i] = double(template_pixels_[i]);
		auto const mid = vals.begin() + vals.size() / 2;
		std::nth_element(vals.begin(), mid, vals.end());
		double const med = *mid;
		std::vector<double> devs(vals.size());
		for (size_t i = 0; i < vals.size(); ++i)
			devs[i] = std::abs(vals[i] - med);
		std::nth_element(devs.begin(), devs.begin() + devs.size() / 2,
						 devs.end());
		template_mad_ = std::max(
			{devs[devs.size() / 2], med, 255.0 - med});
	}
	has_template_ = true;
	return TrackStatus::Ok;
}

TranslationTrackerBackend::Estimate
TranslationTrackerBackend::SpatialPyramidFallback(
	GrayView image, int anchor_x, int anchor_y, int radius_x, int radius_y,
	bool allow_fade_enter) {
	Estimate est;
	GrayView templ_full{template_pixels_.data(), roi_w_, roi_w_, roi_h_};

	// Level 1: half resolution over half radius.
	std::vector<std::uint8_t> templ_half_buf;
	std::vector<std::uint8_t> image_half_buf;
	GrayView const templ_half = Downsample(templ_half_buf, templ_full);
	GrayView const image_half = Downsample(image_half_buf, image);

	int found_x = -1;
	int found_y = -1;
	bool have_guess = false;
	if (templ_half.data && image_half.data) {
		int const ahx = anchor_x / 2;
		int const ahy = anchor_y / 2;
		int const rhx = std::max(1, radius_x / 2);
		int const rhy = std::max(1, radius_y / 2);
		int const min_ox = std::max(0, ahx - rhx);
		int const min_oy = std::max(0, ahy - rhy);
		int const max_ox = std::min(image_half.width - templ_half.width,
		                            ahx + rhx);
		int const max_oy = std::min(image_half.height - templ_half.height,
		                            ahy + rhy);
		if (min_ox <= max_ox && min_oy <= max_oy) {
			// Ties resolve toward the prediction (the anchor): a flat or
			// periodic template scores a whole family of offsets identically,
			// and the scan-order default would walk the estimate toward the
			// scan origin a little every frame.
			auto best = FindBestNcc(templ_half, image_half,
									min_ox, min_oy, max_ox, max_oy,
									NccTieBreak{true, ahx, ahy});
			if (best.found) {
				if (config_.ambiguity_check
				    && HasCompetingNccPeak(
				        templ_half, image_half, min_ox, min_oy, max_ox,
				        max_oy, best.offset_x, best.offset_y,
				        /*excl_x*/ 3, /*excl_y*/ 3,
				        config_.ambiguity_ratio, best.ncc)) {
					est.failure = TrackFailureReason::AmbiguousPeak;
					return est;
				}
				found_x = best.offset_x * 2;
				found_y = best.offset_y * 2;
				have_guess = true;
			}
		}
	}

	// Level 2: full resolution +-refine_radius around the level-1 guess, or
	// a window centered on the anchor spanning the whole radius when no
	// guess exists.
	int const r = have_guess ? config_.refine_radius : std::max(radius_x, radius_y);
	int const min_ox = have_guess ? std::max(0, found_x - r)
	                              : std::max(0, anchor_x - radius_x);
	int const min_oy = have_guess ? std::max(0, found_y - r)
	                              : std::max(0, anchor_y - radius_y);
	int const max_ox = have_guess
	    ? std::min(image.width - roi_w_, found_x + r)
	    : std::min(image.width - roi_w_, anchor_x + radius_x);
	int const max_oy = have_guess
	    ? std::min(image.height - roi_h_, found_y + r)
	    : std::min(image.height - roi_h_, anchor_y + radius_y);
	if (min_ox > max_ox || min_oy > max_oy) {
		est.failure = TrackFailureReason::Offscreen;
		return est;
	}

	auto best = FindBestNcc(templ_full, image,
							min_ox, min_oy, max_ox, max_oy,
							NccTieBreak{true, anchor_x, anchor_y});
	if (!best.found) {
		est.failure = TrackFailureReason::NccLow;
		return est;
	}
	if (config_.ambiguity_check
	    && HasCompetingNccPeak(templ_full, image, min_ox, min_oy, max_ox,
	                           max_oy, best.offset_x, best.offset_y,
	                           /*excl_x*/ 3, /*excl_y*/ 3,
	                           config_.ambiguity_ratio, best.ncc)) {
		est.failure = TrackFailureReason::AmbiguousPeak;
		return est;
	}

	est.plain_ncc = best.ncc;
	// Fade gate (config_.fade_detection), strictly before the occlusion
	// rescue: the caller already certified the window at the held position
	// as a scaled copy of the template whose contrast collapsed below
	// fade_enter_slope, and this plain peak is weak -- a fade frame, not an
	// occluded one. The occlusion rescue, subpixel fit and refresh are all
	// skipped; Step assembles the held result from backend state.
	if (allow_fade_enter && best.ncc < config_.ncc_min) {
		est.ok = true;
		est.fade_held = true;
		return est;
	}
	// Occlusion fallback for the search: a partly occluded target's
	// whole-template surface can prefer a wrong basin (background or
	// occluder alignment) that buries the true offset entirely, so the
	// rescue must re-search on the surviving blocks, not just re-score the
	// peak it landed on. Only frames whose plain peak is too weak take
	// this path; clean frames stay on the historical pipeline.
	bool masked = false;
	std::vector<bool> kept;
	double masked_median = 0.0;
	if (config_.robust_inlier_scoring && best.ncc < config_.ncc_min) {
		int mx = best.offset_x, my = best.offset_y;
		double mn = 0.0;
		if (RobustInlierSearch(image, anchor_x, anchor_y, radius_x, radius_y,
							   mx, my, mn, masked_median, kept)) {
			best.offset_x = mx;
			best.offset_y = my;
			best.ncc = mn;
			masked = true;
		}
	}

	// Subpixel parabola neighbours: masked scores in the occlusion path so
	// the peak the parabola fits is the same peak that was accepted. An
	// out-of-image or degenerate neighbour keeps the peak's own score
	// (parabolic delta 0), matching the historical behaviour.
	auto score_at = [&](int ox, int oy) -> double {
		double v = 0.0;
		if (masked) {
			double median = 0.0;
			return MaskedNccAt(template_pixels_.data(), roi_w_, roi_h_,
							   image, ox, oy, kept, v, median)
					   ? v
					   : best.ncc;
		}
		return ZeroMeanNccScalar(templ_full, image, ox, oy, v) ? v : best.ncc;
	};
	double ncc_left = score_at(best.offset_x - 1, best.offset_y);
	double ncc_right = score_at(best.offset_x + 1, best.offset_y);
	double ncc_up = score_at(best.offset_x, best.offset_y - 1);
	double ncc_down = score_at(best.offset_x, best.offset_y + 1);

	// A perfect integer match has not moved by a fraction of anything, and
	// fitting a parabola through its unequal neighbours invents a same-sign
	// fraction on every frame, which accumulates into a creep. The
	// correlation reaches one only where both pictures are identical, so the
	// integer offset is already the whole answer.
	// Adapted from croni1012/Aegisub src/typesetting_auto_motion.cpp
	// (ISC license).
	bool const exact_integer_match = best.ncc > 1.0 - 1e-9;
	double const dx = exact_integer_match
						  ? 0.0
						  : ParabolicSubpixel(ncc_left, best.ncc, ncc_right);
	double const dy = exact_integer_match
						  ? 0.0
						  : ParabolicSubpixel(ncc_up, best.ncc, ncc_down);
	est.ok = true;
	est.local_dx = double(best.offset_x - anchor_x) + dx;
	est.local_dy = double(best.offset_y - anchor_y) + dy;
	est.ncc = best.ncc;
	est.residual = masked
					   ? masked_median
					   : MadResidual(templ_full, image, best.offset_x, best.offset_y);
	est.match_ox = best.offset_x;
	est.match_oy = best.offset_y;
	return est;
}

TrackStepResult TranslationTrackerBackend::Step(
	TrackStepRequest const& request) {
	TrackStepResult result;
	result.frame = request.frame;

	if (!has_template_ || !ValidView(request.image)) {
		result.status = TrackStatus::InvalidInput;
		return result;
	}
	GrayView const image = request.image;

	// Geometry contract: expected template top-left from round-to-nearest of
	// the search center; the local anchor is its offset inside the crop.
	auto const expected_left_x = static_cast<int>(std::floor(
		request.search_center_x - (roi_w_ - 1) / 2.0 + 0.5));
	auto const expected_left_y = static_cast<int>(std::floor(
		request.search_center_y - (roi_h_ - 1) / 2.0 + 0.5));
	int const anchor_x = expected_left_x - request.image_origin_x;
	int const anchor_y = expected_left_y - request.image_origin_y;

	if (anchor_x < 0 || anchor_y < 0 || anchor_x + roi_w_ > image.width
	    || anchor_y + roi_h_ > image.height) {
		result.status = TrackStatus::Failed;
		result.failure = TrackFailureReason::Offscreen;
		return result;
	}
	int const radius_x = std::min(anchor_x, image.width - roi_w_ - anchor_x);
	int const radius_y = std::min(anchor_y, image.height - roi_h_ - anchor_y);

	// Held/duplicate-frame pre-check (config_.held_frame_check): when the
	// previous accepted match window is (near-)identical to the same
	// absolute region of this frame, the target has not moved and the
	// previous integer offset is the whole answer — keep it with zero
	// subpixel delta and skip the pyramid search, occlusion fallback and
	// subpixel fit, so encoder luma noise on static content cannot jitter
	// the estimate into drift or false NccLow/ResidualHigh failures. The
	// plain whole-template NCC at the kept offset is still computed (one
	// cheap pass): the template-refresh gate keeps judging the plain match,
	// and the acceptance gates below keep deciding this step like any
	// other. The first Step after Reset has no previous match and always
	// runs the full search; a previous window outside this crop cannot be
	// compared and also runs the full search.
	// Adapted from croni1012/Aegisub src/typesetting_auto_motion.cpp
	// (ISC license) -- the held-frame fast path reusing the previous
	// transform.
	bool held = false;
	int held_ox = 0;
	int held_oy = 0;
	double held_mean_diff = 0.0;
	if (config_.held_frame_check && held_has_previous_) {
		held_ox = prev_match_abs_x_ - request.image_origin_x;
		held_oy = prev_match_abs_y_ - request.image_origin_y;
		if (held_ox >= 0 && held_oy >= 0 && held_ox + roi_w_ <= image.width && held_oy + roi_h_ <= image.height) {
			GrayView const prev_view{prev_window_.data(), roi_w_, roi_w_,
									 roi_h_};
			held = WindowIsHeld(prev_view, image, held_ox, held_oy,
								held_mean_diff);
		}
	}

	// Fade signal (config_.fade_detection), one scalar pass at the hold
	// position: the held-check's previous match window -- the first Step
	// after Reset has none and measures at the anchor instead. A mid-fade
	// frame differs from the previous window (its contrast is shrinking),
	// so it flows past the held pre-check and into the fade machinery
	// below; the two fast paths compose because both keep the position and
	// neither updates the held-window state. ZeroMeanNccScalar exports the
	// contrast slope on the pass that already computes the NCC, so no
	// second search is added.
	int const fade_hold_ox = held_has_previous_
								 ? prev_match_abs_x_ - request.image_origin_x
								 : anchor_x;
	int const fade_hold_oy = held_has_previous_
								 ? prev_match_abs_y_ - request.image_origin_y
								 : anchor_y;
	double hold_ncc = -1.0;
	double hold_slope = 1.0;
	double hold_mean_abs = 0.0;
	bool const fade_hold_in_crop = config_.fade_detection && MeasureFadeSignal(image, fade_hold_ox, fade_hold_oy, hold_ncc,
																			   hold_slope, hold_mean_abs);

	// Fade-held step assembly: Ok at the last accepted position with the
	// measured contrast slope as visibility. Confidence is the slope
	// clamped into the documented [0, 1] range -- it is the exact quantity
	// the fade gates trust, so the published confidence tracks how visible
	// the target still is instead of the brightness-invariant (and hence
	// fade-blind) NCC. Residual is the mean |template - window| difference
	// at the reported position. The stored accepted center is re-reported
	// verbatim, so the hold is exactly static and the session's velocity
	// prior decays on natural zero deltas. Held-window state and template
	// are left untouched: no refresh on fade frames, and the held pre-check
	// keeps comparing against the last full-search window. `measured` says
	// whether (slope, mean_abs_diff) is a real measurement from this step;
	// an unmeasured hold reuses the last measured signal, and when none
	// exists yet it reports unmeasured visibility (-1) with a bounded
	// confidence instead of inventing a fully-visible slope.
	auto make_fade_held = [&](bool measured, double slope, double mean_abs_diff) {
		fade_phase_ = FadePhase::Fading;
		result.status = TrackStatus::Ok;
		if (!measured) {
			if (fade_last_slope_ < 0.0) {
				result.confidence = 0.0;
				result.fade_visibility = -1.0;
				result.residual = 0.0;
			}
			else {
				slope = fade_last_slope_;
				mean_abs_diff = fade_last_mean_abs_;
				result.confidence = std::clamp(slope, 0.0, 1.0);
				result.residual = mean_abs_diff;
				result.fade_visibility = std::clamp(slope, 0.0, 2.0);
			}
		}
		else {
			fade_last_slope_ = slope;
			fade_last_mean_abs_ = mean_abs_diff;
			result.confidence = std::clamp(slope, 0.0, 1.0);
			result.residual = mean_abs_diff;
			result.fade_visibility = std::clamp(slope, 0.0, 2.0);
		}
		if (held_has_previous_) {
			result.candidate_center_x = prev_center_abs_x_;
			result.candidate_center_y = prev_center_abs_y_;
		}
		else {
			result.candidate_center_x = request.image_origin_x + fade_hold_ox + (roi_w_ - 1) / 2.0;
			result.candidate_center_y = request.image_origin_y + fade_hold_oy + (roi_h_ - 1) / 2.0;
		}
	};

	Estimate est;
	bool pending_recovery = false;
	double probe_slope = -1.0;
	if (held) {
		GrayView const templ_full{template_pixels_.data(), roi_w_, roi_w_,
								  roi_h_};
		double ncc = 0.0;
		// A degenerate (flat) window at the kept offset has no plain score;
		// fall through to the full search, which owns that failure mode.
		if (ZeroMeanNccScalar(templ_full, image, held_ox, held_oy, ncc)) {
			est.ok = true;
			est.local_dx = double(held_ox - anchor_x);
			est.local_dy = double(held_oy - anchor_y);
			est.ncc = ncc;
			est.plain_ncc = ncc;
			est.residual = held_mean_diff;
			est.match_ox = held_ox;
			est.match_oy = held_oy;
		}
		else {
			held = false;
		}
	}
	if (!held && config_.fade_detection && fade_phase_ == FadePhase::Fading) {
		// While fading: hold the last accepted position and re-probe it with
		// one full search every fade_probe_interval frames. There is
		// deliberately no fixed cap on the hold duration -- a fade may last
		// to the end of the domain.
		int const probe_interval = std::max(1, config_.fade_probe_interval);
		bool const probe = !fade_hold_in_crop || ++fade_frames_since_probe_ >= probe_interval;
		if (!probe) {
			make_fade_held(fade_hold_in_crop, hold_slope, hold_mean_abs);
			return result;
		}
		fade_frames_since_probe_ = 0;
		est = SpatialPyramidFallback(image, anchor_x, anchor_y, radius_x,
									 radius_y, /*allow_fade_enter=*/false);
		double match_ncc = 0.0;
		double match_slope = 2.0;
		double match_mean_abs = 0.0;
		if (est.ok)
			MeasureFadeSignal(image, est.match_ox, est.match_oy, match_ncc,
							  match_slope, match_mean_abs);
		if (!est.ok) {
			// The probe found nothing holdable (a fully flat deep fade scores
			// no peak at all): keep holding. A lost target needs a confident
			// displaced match to fail.
			make_fade_held(fade_hold_in_crop, hold_slope, hold_mean_abs);
			return result;
		}
		if (match_slope >= config_.fade_exit_slope) {
			// Recovery candidate: contrast is back at the probe match, so
			// resume normal tracking from it -- but only after the shared
			// acceptance gates below accept the estimate: a slope spike
			// whose match still fails ncc_min or the residual gate must not
			// flip the phase to NotFaded (and then hard-fail the step);
			// it stays Fading and keeps holding instead. Hysteresis: only a
			// probe may end the hold, so single-frame slope spikes during
			// the fade cannot exit early.
			pending_recovery = true;
			probe_slope = match_slope;
			// Fall through to the shared acceptance path with this estimate.
		}
		else if (est.ncc >= config_.ncc_min && std::max(std::abs(est.match_ox - fade_hold_ox), std::abs(est.match_oy - fade_hold_oy)) > kFadeProbeLostPx && match_slope <= config_.fade_enter_slope) {
			// The target moved while invisible: the probe found a confident
			// match well away from the held position while the contrast is
			// still collapsed. Report the target lost instead of
			// teleporting to the new location.
			result.status = TrackStatus::Failed;
			result.failure = TrackFailureReason::NccLow;
			return result;
		}
		else {
			make_fade_held(fade_hold_in_crop, hold_slope, hold_mean_abs);
			return result;
		}
	}
	else if (!held) {
		// Not faded (or fade detection off): the plain pyramid search. The
		// fade-enter flag is pre-certified here -- the window at the hold
		// position must be a scaled copy of the template (NCC >=
		// kFadeCorrelationFloor: a fade is a*x+b so its correlation survives,
		// occlusion and misalignment do not) whose contrast already
		// collapsed below fade_enter_slope -- so the gate inside the search
		// can claim the frame before the occlusion rescue runs.
		est = SpatialPyramidFallback(image, anchor_x, anchor_y, radius_x,
									 radius_y,
									 fade_hold_in_crop && hold_ncc >= kFadeCorrelationFloor && hold_slope <= config_.fade_enter_slope);
	}

	if (!est.ok) {
		result.status = TrackStatus::Failed;
		result.failure = est.failure == TrackFailureReason::None
							 ? TrackFailureReason::NccLow
							 : est.failure;
		return result;
	}
	if (est.fade_held) {
		// The search's fade gate claimed this weak frame; assemble the held
		// result from backend state.
		make_fade_held(fade_hold_in_crop, hold_slope, hold_mean_abs);
		return result;
	}
	if (est.ncc < config_.ncc_min) {
		if (pending_recovery) {
			// The recovery candidate failed the acceptance gate; keep
			// holding rather than flipping out of the fade and failing.
			make_fade_held(fade_hold_in_crop, hold_slope, hold_mean_abs);
			return result;
		}
		result.status = TrackStatus::Failed;
		result.failure = TrackFailureReason::NccLow;
		return result;
	}
	if (est.residual > config_.residual_max) {
		// ResidualHigh exemption (config_.fade_detection): a found match
		// whose window is a certified scaled copy of the template with
		// collapsed contrast is a fade frame, not a bad match -- hold it
		// exactly like the weak-peak path (the slope and mean difference
		// are measured at the match position, the position the gate
		// certified). The plain score, never the occlusion-rescored one,
		// certifies the copy; genuinely bad matches keep hard-failing
		// because their slope or correlation does not read as a fade.
		// The exemption must be armed by the time the residual gate can
		// first trip: a certified copy fading toward level L trips
		// residual_max at slope 1 - residual_max / median|t - L|, worst at
		// the fade extrema L in {0, 255} (template_mad_ covers those), so
		// for a fade to black/white the band between fade_enter_slope and
		// that trip slope is several frames deep on a gradual fade --
		// enough consecutive failures to still kill the arm if the
		// exemption were armed only at fade_enter_slope. Arm from the
		// earlier threshold instead, capped at 0.95 so an extreme template
		// cannot widen the gate to near-full contrast.
		double const enter_gate = std::max(
			config_.fade_enter_slope,
			std::min(1.0 - config_.residual_max / std::max(template_mad_, 1.0),
					 0.95));
		if (config_.fade_detection && est.plain_ncc >= kFadeCorrelationFloor) {
			double match_ncc = 0.0;
			double match_slope = 2.0;
			double match_mean_abs = 0.0;
			if (MeasureFadeSignal(image, est.match_ox, est.match_oy,
								  match_ncc, match_slope, match_mean_abs) &&
				match_slope <= enter_gate) {
				make_fade_held(true, match_slope, match_mean_abs);
				return result;
			}
		}
		if (pending_recovery) {
			// The recovery candidate failed the residual gate; keep holding
			// rather than flipping out of the fade and failing.
			make_fade_held(fade_hold_in_crop, hold_slope, hold_mean_abs);
			return result;
		}
		result.status = TrackStatus::Failed;
		result.failure = TrackFailureReason::ResidualHigh;
		return result;
	}

	result.status = TrackStatus::Ok;
	result.confidence = est.ncc;
	result.residual = est.residual;
	result.candidate_center_x = request.image_origin_x + anchor_x + est.local_dx + (roi_w_ - 1) / 2.0;
	result.candidate_center_y = request.image_origin_y + anchor_y + est.local_dy + (roi_h_ - 1) / 2.0;
	// Recovery probe acceptance: the estimate passed the ncc and residual
	// gates above, so the hold ends here (fade_visibility carries the probe
	// slope measured at this accepted match position) and normal tracking
	// resumes from it.
	if (pending_recovery) {
		fade_phase_ = FadePhase::NotFaded;
		result.fade_visibility = std::clamp(probe_slope, 0.0, 2.0);
	}
	// Full visibility curve, fade phase B: one MeasureFadeSignal pass at the
	// accepted match position (the occlusion-rescored offset when that path
	// claimed the peak; the kept offset on held fast-path steps), so every
	// accepted Ok step reports visibility instead of only fade-gated ones
	// and the session records a per-frame curve. O(roi), negligible against
	// the search it follows. The recovery-probe path already reported the
	// slope it measured at this same offset, so only fill while unset;
	// -1 survives only where measurement is impossible or fade detection is
	// off. No acceptance gate above is affected.
	else if (config_.fade_detection && result.fade_visibility < 0.0) {
		double vis_ncc = 0.0;
		double vis_slope = 0.0;
		double vis_mean_abs = 0.0;
		if (MeasureFadeSignal(image, est.match_ox, est.match_oy, vis_ncc,
							  vis_slope, vis_mean_abs))
			result.fade_visibility = std::clamp(vis_slope, 0.0, 2.0);
	}
	// Slow template refresh after confident matches only: a low-NCC match is
	// exactly the frame where the "match" may be partly background, and
	// blending that in would poison the template. The gate judges the plain
	// whole-template score (est.plain_ncc), never the occlusion-rescored
	// one, for the same reason. Fade-held steps never reach this point, so
	// fade frames never refresh the template.
	if (config_.template_refresh && est.plain_ncc >= config_.refresh_min_ncc)
		RefreshTemplate(image, est.match_ox, est.match_oy);

	// Track the last full-search match for the held-frame pre-check. Held
	// steps leave it untouched — the window they accepted is the same one —
	// which also bounds the drift the noise gate could chain: every frame
	// is compared against the last real match, never against a running
	// sequence of noise realizations. Fade-held steps return above, so a
	// hold always measures and compares against the last full-search match
	// as well. Maintained whatever the held_frame_check flag says, so
	// toggling it on mid-run is immediate.
	if (!held) {
		held_has_previous_ = true;
		prev_match_abs_x_ = request.image_origin_x + est.match_ox;
		prev_match_abs_y_ = request.image_origin_y + est.match_oy;
		prev_center_abs_x_ = result.candidate_center_x;
		prev_center_abs_y_ = result.candidate_center_y;
		prev_window_.resize(size_t(roi_w_) * size_t(roi_h_));
		for (int y = 0; y < roi_h_; ++y)
			std::copy_n(
				image.data + int64_t(est.match_oy + y) * image.stride + est.match_ox,
				roi_w_, prev_window_.begin() + int64_t(y) * roi_w_);
	}
	return result;
}

bool TranslationTrackerBackend::MeasureFadeSignal(
	GrayView image, int ox, int oy, double& out_ncc, double& out_slope,
	double& out_mean_abs) const {
	if (ox < 0 || oy < 0 || ox + roi_w_ > image.width || oy + roi_h_ > image.height)
		return false;
	GrayView const templ{
		.data = template_pixels_.data(), .stride = roi_w_, .width = roi_w_, .height = roi_h_};
	double ncc = 0.0;
	// ZeroMeanNccScalar fills the slope even when the NCC itself is
	// undefined: a flat window reports the exact fully-faded 0.0, a flat
	// template the pinned 1.0.
	out_ncc = ZeroMeanNccScalar(templ, image, ox, oy, ncc, &out_slope)
				  ? ncc
				  : -1.0;
	long long sum = 0;
	for (int y = 0; y < roi_h_; ++y) {
		auto const *trow =
			template_pixels_.data() + static_cast<int64_t>(y) * roi_w_;
		auto const *irow =
			image.data + static_cast<int64_t>(oy + y) * image.stride + ox;
		for (int x = 0; x < roi_w_; ++x)
			sum += std::abs(static_cast<int>(trow[x]) - static_cast<int>(irow[x]));
	}
	out_mean_abs = static_cast<double>(sum) / (static_cast<double>(roi_w_) * static_cast<double>(roi_h_));
	return true;
}

bool TranslationTrackerBackend::RobustInlierSearch(
	GrayView image, int anchor_x, int anchor_y, int radius_x, int radius_y,
	int& out_offset_x, int& out_offset_y, double& out_ncc,
	double& out_median, std::vector<bool>& out_kept) {
	// The vote window is a neighbourhood of the prediction, not the full
	// search rect: the pyramid failure mode being rescued is a wrong basin
	// far from the prediction (occluder/background alignment), while motion
	// continuity keeps the true offset near it. A small window also makes
	// the per-block tie-break (nearest the prediction) meaningful for the
	// smooth low-discrimination blocks, which then abstain-or-agree instead
	// of scattering across the whole rect.
	int const vote_r = std::clamp(std::min(radius_x, radius_y) / 2, 2, 6);
	int const min_ox = std::max(0, anchor_x - vote_r);
	int const min_oy = std::max(0, anchor_y - vote_r);
	int const max_ox = std::min(image.width - roi_w_, anchor_x + vote_r);
	int const max_oy = std::min(image.height - roi_h_, anchor_y + vote_r);
	if (min_ox > max_ox || min_oy > max_oy)
		return false;

	int const blocks_x = (roi_w_ + kOcclusionBlock - 1) / kOcclusionBlock;
	int const blocks_y = (roi_h_ + kOcclusionBlock - 1) / kOcclusionBlock;
	size_t const block_count = size_t(blocks_x) * blocks_y;

	// Stage 1: every block of the seed template searches the prediction
	// neighbourhood on its own. A block's view starts at template pixel
	// (x0, y0), so its window and tie-break centre are the whole-template
	// ones shifted by (x0, y0); every vote then lands in whole-template
	// offset coordinates and blocks can agree with each other. Occluded
	// blocks land wherever their local texture happens to correlate; blocks
	// that still see the tracked texture agree with the prediction. A block
	// whose NCC surface is flat (near-1.0 against smooth windows everywhere —
	// the degenerate small-patch case on low-frequency textures) carries no
	// location evidence and abstains: the vote only counts when the peak
	// stands at least `kVoteSeparation` above the best score displaced by
	// more than 1 px.
	// Adapted from croni1012/Aegisub src/typesetting_auto_motion.cpp
	// (ISC license) — patch voting with
	// minimum_correlation_separation.
	constexpr double kVoteSeparation = 0.025;
	struct Vote {
		int x = 0;
		int y = 0;
		bool confident = false;
	};
	std::vector<Vote> votes(block_count);
	for (int by = 0; by < blocks_y; ++by)
		for (int bx = 0; bx < blocks_x; ++bx) {
			int const x0 = bx * kOcclusionBlock;
			int const y0 = by * kOcclusionBlock;
			int const bw = std::min(kOcclusionBlock, roi_w_ - x0);
			int const bh = std::min(kOcclusionBlock, roi_h_ - y0);
			GrayView const block(
				template_pixels_.data() + int64_t(y0) * roi_w_ + x0, roi_w_,
				bw, bh);
			// x0 + bw <= roi_w_ and the window is clamped against the whole
			// ROI, so the shifted bounds stay inside the image for the block.
			auto const best = FindBestNccScalar(
				block, image, min_ox + x0, min_oy + y0, max_ox + x0,
				max_oy + y0, NccTieBreak{true, anchor_x + x0, anchor_y + y0});
			auto& vote = votes[size_t(by) * blocks_x + bx];
			if (!best.found || best.ncc < config_.ncc_min)
				continue;
			// Report the displacement from the block's own predicted
			// position, so votes from blocks at different ROI positions are
			// comparable whole-template offsets.
			vote.x = best.offset_x - x0;
			vote.y = best.offset_y - y0;
			double displaced = -2.0;
			for (int y = min_oy + y0; y <= max_oy + y0; ++y)
				for (int x = min_ox + x0; x <= max_ox + x0; ++x) {
					if (std::max(std::abs(x - best.offset_x),
								 std::abs(y - best.offset_y)) <= 1)
						continue;
					double ncc = 0.0;
					if (ZeroMeanNccScalar(block, image, x, y, ncc) && ncc > displaced)
						displaced = ncc;
				}
			vote.confident = best.ncc - displaced >= kVoteSeparation;
		}

	// Stage 2: consensus — the offset (within +-1 px) the most confident
	// blocks agree on, with croni's minimum inlier floor of 4 among the
	// participating blocks.
	size_t participants = 0;
	for (auto const& v : votes)
		if (v.confident)
			++participants;
	size_t const min_agree = std::max<std::size_t>(
		4, static_cast<size_t>(std::ceil(
			   double(participants) * (1.0 - config_.max_occlusion_fraction))));
	size_t best_agree = 0;
	int cx = min_ox;
	int cy = min_oy;
	for (size_t i = 0; i < block_count; ++i) {
		auto const& candidate = votes[i];
		if (!candidate.confident)
			continue;
		size_t agree = 0;
		for (size_t j = 0; j < block_count; ++j) {
			auto const& v = votes[j];
			if (!v.confident)
				continue;
			if (std::abs(v.x - candidate.x) <= 1 && std::abs(v.y - candidate.y) <= 1)
				++agree;
		}
		if (agree > best_agree) {
			best_agree = agree;
			cx = candidate.x;
			cy = candidate.y;
		}
	}
	if (best_agree < min_agree)
		return false;

	// Stage 3: verify the consensus with a pixel-level inlier mask. Blocks
	// were only the voting mechanism; the occlusion cap itself is on area:
	// at the consensus offset the per-pixel |difference| must stay inside a
	// robust noise gate (median + k * 1.4826 * MAD — with the occluded
	// fraction under the cap the median sits on visible pixels, so the gate
	// tracks the actual noise scale instead of a hardwired threshold) on at
	// least 1 - max_occlusion_fraction of the template pixels. The kept mask
	// travels out so the subpixel stage fits the same peak it accepted.
	auto const diffs =
		PixelAbsDiffs(template_pixels_.data(), roi_w_, roi_h_, image, cx, cy);
	RobustInlierParams params;
	params.max_reject_fraction = config_.max_occlusion_fraction;
	std::vector<bool> kept;
	if (!SelectInlierPixels(diffs, params, kept))
		return false;
	double ncc = 0.0;
	double median = 0.0;
	if (!MaskedNccAt(template_pixels_.data(), roi_w_, roi_h_, image, cx, cy,
					 kept, ncc, median))
		return false;
	if (ncc < config_.ncc_min)
		return false;
	out_offset_x = cx;
	out_offset_y = cy;
	out_ncc = ncc;
	out_median = median;
	out_kept = std::move(kept);
	return true;
}

void TranslationTrackerBackend::RefreshTemplate(GrayView image, int ox, int oy) {
	if (ox < 0 || oy < 0 || ox + roi_w_ > image.width
	    || oy + roi_h_ > image.height)
		return;
	double const keep = 1.0 - config_.refresh_alpha;
	for (int y = 0; y < roi_h_; ++y) {
		auto const* irow = image.data + int64_t(oy + y) * image.stride + ox;
		auto* trow = template_pixels_.data() + int64_t(y) * roi_w_;
		for (int x = 0; x < roi_w_; ++x) {
			double const blended =
			    keep * trow[x] + config_.refresh_alpha * irow[x];
			trow[x] = static_cast<std::uint8_t>(blended + 0.5);
		}
	}
}

std::uint64_t TranslationTrackerBackend::TemplateHash() const noexcept {
	if (!has_template_ || template_pixels_.empty())
		return 0;
	return HashBytes(template_pixels_.data(), template_pixels_.size());
}

} // namespace aegisub::motion_track
