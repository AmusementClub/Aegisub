#pragma once

#include <span>
#include <string>

namespace agi::vfr {
class Framerate;
}

namespace aegisub::align_video_fade {

struct CurveFit {
	bool detected = false;
	int outer_index = -1;
	int inner_index = -1;
	double confidence = 0.0;
};

/// Fit an absent -> fade -> fully-visible curve. Samples must be ordered from
/// the outside of the candidate range toward the fully-visible anchor range;
/// the final plateau_samples entries must be confirmed fully-visible samples.
CurveFit FitVisibilityCurve(std::span<double const> samples, int plateau_samples);

struct PlateauReference {
	bool valid = false;
	double level = 0.0;
	double level_band = 0.0;
	double spread_limit = 0.0;
};

PlateauReference BuildPlateauReference(std::span<double const> platform_samples);

/// Return the first sample of the earliest consecutive window statistically
/// indistinguishable from a known fully-visible platform, or -1 if none is
/// confirmed. The reference platform determines the temporal noise band.
int FindConfirmedPlateauStart(
	std::span<double const> samples,
	PlateauReference const& platform,
	int confirmation_samples);

int FindConfirmedPlateauStart(
	std::span<double const> samples,
	std::span<double const> platform_samples,
	int confirmation_samples);

enum class AssFadeEncoding {
	Fad,
	Fade,
	AlphaTransform
};

struct AssFadeUpdate {
	std::string text;
	AssFadeEncoding encoding = AssFadeEncoding::Fad;
};

struct AssFadeTiming {
	int start_ms = 0;
	int end_ms = 0;
	int fade_in_ms = 0;
	int fade_out_ms = 0;
};

/// Project detected frame samples to ASS event and fade times. Event bounds
/// use subtitle START/END semantics so the first and last visible samples are
/// inside the event. Fade control points use EXACT sample times so the first
/// fully-visible sample is exactly 100%, rather than completing half a frame
/// early.
AssFadeTiming BuildAssFadeTiming(
	agi::vfr::Framerate const& timecodes,
	int first_visible_frame,
	int last_visible_frame,
	int fade_in_full_frame,
	int fade_out_full_frame,
	bool fade_in_detected,
	bool fade_out_detected);

/// Replace an existing line-wide fade representation, preserving \fad,
/// \fade, or the common leading \alpha + \t form when possible. Lines with
/// no existing representation receive \fad.
AssFadeUpdate ApplyAssFade(
	std::string const& text,
	int duration_ms,
	int fade_in_ms,
	int fade_out_ms);

} // namespace aegisub::align_video_fade
