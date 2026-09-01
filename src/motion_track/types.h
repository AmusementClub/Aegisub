#pragma once

// Frozen sample layout for the translation tracking slice: later slices may
// only add fields, never repurpose them.

#include "../raw_frame_view.h"

#include <array>
#include <cstdint>
#include <vector>

namespace aegisub::motion_track {

struct FrameInterval {
	int first = -1;
	int last = -1; // inclusive
};

enum class AnalyzeStopReason : std::uint8_t {
	None,
	Completed,
	UserCanceled,
	ConsecutiveFailures,
	ProviderChanged,
	FrameUnavailable,
	DecodeError,
	Error,
};

enum class TrackStatus : std::int32_t {
	Ok = 0,
	Failed = 1,
	InvalidInput = 2,
	Unsupported = 3,
	Missing = 4,
};

enum class TrackModel : std::int32_t {
	Translation = 1,
	Similarity = 2, // built-in backends return Unsupported
	Homography = 3,
};

// Config key "Apply Mode" stores this value directly.
enum class ApplyMode : std::int32_t {
	Compact = 0, // default
	Exact = 1,
};

// Only meaningful while status == Failed; diagnostics/logging only, never an
// input to Apply decisions.
enum class TrackFailureReason : std::int32_t {
	None = 0,
	NccLow,
	ResidualHigh,
	Offscreen,
	JumpTooLarge, // per-step pose bound violated (similarity model)
	AmbiguousPeak, // competing NCC peak: repetitive texture in the ROI
};

enum class TrackDirection : std::uint8_t {
	Forward = 0,
	Backward = 1,
	Bidirectional = 2,
};

// Non-owning view. Valid only for the duration of the single Reset/Step/
// FetchGray call that received it. Backends must copy pixels they keep.
struct GrayView {
	std::uint8_t const* data = nullptr;
	int stride = 0;
	int width = 0;
	int height = 0;
};

struct GrayPatch {
	int frame = -1;
	int origin_x = 0; // requested top-left corner in storage pixels; may be
	int origin_y = 0; // negative and is never rewritten by clamping
	int width = 0;
	int height = 0;
	int stride = 0;
	std::vector<std::uint8_t> gray; // owning, stride >= width rows

	GrayView View() const { return GrayView{gray.data(), stride, width, height}; }
};

struct TrackTransform {
	std::array<double, 9> matrix{
		1.0, 0.0, 0.0,
		0.0, 1.0, 0.0,
		0.0, 0.0, 1.0};
};

// Stable meaning: published transform is origin-seed-relative storage delta;
// center_x/y are absolute storage pixel-center coordinates of the ROI center.
struct TrackSample {
	int frame = -1;
	TrackModel model = TrackModel::Translation;
	TrackStatus status = TrackStatus::Missing;
	TrackFailureReason failure = TrackFailureReason::None; // only when Failed
	double confidence = 0.0;     // NCC peak [0,1]; display/export only — failure
	                             // decisions use the individual metric thresholds
	/// Median |template - window| at the accepted match, in the same
	/// lower-median convention the backend's residual gate is calibrated
	/// against (held and fade-held steps included). Display/export only.
	double residual = 0.0;
	TrackTransform transform;
	double center_x = 0.0;
	double center_y = 0.0;
	/// Fade visibility against the seed template: the least-squares contrast
	/// slope clamped to [0, 2] (1.0 = full contrast, -> 0 = faded out);
	/// -1.0 = not measured on this sample (fade detection off, or the window
	/// at the reported position fell outside the fetched crop). Seed samples
	/// report 1.0 -- the template is its own fully-visible reference.
	/// Display/export metric for the fade slice (naming follows the
	/// align_video_fade visibility semantics); never an acceptance input.
	double fade_visibility = -1.0;
};

struct TrackStepRequest {
	int frame = -1;
	GrayView image; // valid for the duration of the call
	int image_origin_x = 0;
	int image_origin_y = 0;
	double search_center_x = 0.0; // last accepted ROI pixel-center, storage
	double search_center_y = 0.0;
	// Pose carried from the previous accepted step (rotation in radians,
	// uniform scale), seed-relative. Similarity backends use it as the
	// starting point of the per-frame refinement; translation backends
	// ignore it.
	double init_rotation = 0.0;
	double init_scale = 1.0;
};

struct TrackStepResult {
	int frame = -1;
	TrackStatus status = TrackStatus::Missing;
	TrackFailureReason failure = TrackFailureReason::None; // only when Failed
	double confidence = 0.0;
	double residual = 0.0;
	// Seed-relative linear part: identity for translation backends,
	// s*R(rotation) for similarity. The translation entries stay zero here —
	// the session anchors them at the origin center.
	TrackTransform transform;
	double candidate_center_x = 0.0; // absolute storage; session normalizes
	double candidate_center_y = 0.0; // into a TrackSample
	/// Contrast slope of the measured window against the seed template,
	/// clamped to [0, 2]; -1.0 = not measured this step. Set on fade-held
	/// steps (the fade gate's slope), on fade recovery steps (the probe
	/// slope that ended the hold) and on every accepted plain step (the
	/// slope at its accepted match position -- the phase B full curve);
	/// -1.0 survives only where measurement is impossible or fade detection
	/// is off.
	double fade_visibility = -1.0;
};

// Axis-aligned ROI in storage pixels, half-open [x, x+w) x [y, y+h).
struct RoiRect {
	int x = 0, y = 0, w = 0, h = 0;
};

// Non-owning view of one raw BGRA video frame; storage is owned by the
// provider and stays valid until the next fetch or the end of the batch
// callback. Defined in src/raw_frame_view.h -- re-exported here because the
// motion_track API and async_video_provider both spell it
// motion_track::RawBgraView.
using aegisub::RawBgraView;

// samples are stored in ascending frame order; linear scan by design
// (n <= 10000). Returns nullptr when not present.
TrackSample const* FindSample(
	std::vector<TrackSample> const& samples, int frame);

} // namespace aegisub::motion_track
