#include "apply_plan.h"

#include "../align_video_fade.h"

#include "ass_dialogue.h"
#include "ass_file.h"

#include "ass_style.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <numbers>
#include <optional>
#include <string_view>

namespace aegisub::motion_track {

namespace {

using Blocks = std::vector<std::unique_ptr<AssDialogueBlock>>;

struct PositionInfo {
	bool has_pos = false;
	double pos_x = 0.0, pos_y = 0.0;
	bool has_move = false;
	double move_x1 = 0.0, move_y1 = 0.0, move_x2 = 0.0, move_y2 = 0.0;
	int move_t1 = 0, move_t2 = 0;
	int alignment_override = 0; // \\an / \\a (already converted to \\an space)
	// Inline transform tags (last occurrence wins, matching ASS override
	// semantics). Used as the similarity apply base so user styling survives.
	bool has_frz = false;
	double frz = 0.0;
	bool has_fscx = false;
	double fscx = 100.0;
	bool has_fscy = false;
	double fscy = 100.0;
	// Border/shadow/blur family, same last-wins rule. Growth compensation
	// composes these with the style base (croni growth semantics).
	bool has_bord = false;
	double bord = 0.0;
	bool has_xbord = false;
	double xbord = 0.0;
	bool has_ybord = false;
	double ybord = 0.0;
	bool has_shad = false;
	double shad = 0.0;
	bool has_xshad = false;
	double xshad = 0.0;
	bool has_yshad = false;
	double yshad = 0.0;
	bool has_blur = false;
	double blur = 0.0;
};

bool IsTagNameChar(char c) {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
	    || (c >= '0' && c <= '9');
}

PositionInfo ExtractPositionInfo(AssDialogue const& line) {
	PositionInfo info;
	auto blocks = line.ParseTags();
	for (auto const& block : blocks) {
		if (block->GetType() != AssBlockType::OVERRIDE) continue;
		auto const* ov = static_cast<AssDialogueBlockOverride const*>(block.get());
		for (auto const& tag : ov->Tags) {
			if (tag.Name == "\\pos" && tag.Params.size() >= 2
			    && !info.has_pos) {
				info.has_pos = true;
				info.pos_x = tag.Params[0].Get<double>(0.0);
				info.pos_y = tag.Params[1].Get<double>(0.0);
			} else if (tag.Name == "\\move" && tag.Params.size() >= 4
			           && !info.has_move) {
				info.has_move = true;
				info.move_x1 = tag.Params[0].Get<double>(0.0);
				info.move_y1 = tag.Params[1].Get<double>(0.0);
				info.move_x2 = tag.Params[2].Get<double>(0.0);
				info.move_y2 = tag.Params[3].Get<double>(0.0);
				info.move_t1 = tag.Params.size() > 4
				    ? tag.Params[4].Get<int>(0) : 0;
				info.move_t2 = tag.Params.size() > 5
				    ? tag.Params[5].Get<int>(0) : 0;
			} else if (tag.Name == "\\an" && tag.Params.size() >= 1
			           && info.alignment_override == 0) {
				info.alignment_override = tag.Params[0].Get<int>(0);
			} else if (tag.Name == "\\a" && tag.Params.size() >= 1
			           && info.alignment_override == 0) {
				info.alignment_override =
					AssStyle::SsaToAss(tag.Params[0].Get<int>(2));
			} else if (tag.Name == "\\frz" && tag.Params.size() >= 1) {
				info.has_frz = true;
				info.frz = tag.Params[0].Get<double>(0.0);
			} else if (tag.Name == "\\fscx" && tag.Params.size() >= 1) {
				info.has_fscx = true;
				info.fscx = tag.Params[0].Get<double>(100.0);
			} else if (tag.Name == "\\fscy" && tag.Params.size() >= 1) {
				info.has_fscy = true;
				info.fscy = tag.Params[0].Get<double>(100.0);
			}
			else if (tag.Name == "\\bord" && tag.Params.size() >= 1) {
				info.has_bord = true;
				info.bord = tag.Params[0].Get<double>(0.0);
			}
			else if (tag.Name == "\\xbord" && tag.Params.size() >= 1) {
				info.has_xbord = true;
				info.xbord = tag.Params[0].Get<double>(0.0);
			}
			else if (tag.Name == "\\ybord" && tag.Params.size() >= 1) {
				info.has_ybord = true;
				info.ybord = tag.Params[0].Get<double>(0.0);
			}
			else if (tag.Name == "\\shad" && tag.Params.size() >= 1) {
				info.has_shad = true;
				info.shad = tag.Params[0].Get<double>(0.0);
			}
			else if (tag.Name == "\\xshad" && tag.Params.size() >= 1) {
				info.has_xshad = true;
				info.xshad = tag.Params[0].Get<double>(0.0);
			}
			else if (tag.Name == "\\yshad" && tag.Params.size() >= 1) {
				info.has_yshad = true;
				info.yshad = tag.Params[0].Get<double>(0.0);
			}
			else if (tag.Name == "\\blur" && tag.Params.size() >= 1) {
				info.has_blur = true;
				info.blur = tag.Params[0].Get<double>(0.0);
			}
		}
	}
	return info;
}

} // namespace

bool ResolveDialogueOrigin(AssFile const& file, AssDialogue const& line,
                           int seed_time_ms, int script_width,
                           int script_height, double& out_x, double& out_y) {
	PositionInfo const info = ExtractPositionInfo(line);

	if (info.has_pos) {
		out_x = info.pos_x;
		out_y = info.pos_y;
		return true;
	}

	if (info.has_move) {
		// VSFilter treats omitted/non-positive times as whole-event window;
		// an explicit window must be strictly ordered to be valid.
		int const rel = seed_time_ms - int(line.Start);
		int w1 = info.move_t1;
		int w2 = info.move_t2;
		if (w1 <= 0 || w2 <= 0) {
			w1 = 0;
			w2 = int(line.End) - int(line.Start);
		} else if (w2 <= w1) {
			return false;
		}
		if (rel <= w1) {
			out_x = info.move_x1;
			out_y = info.move_y1;
		} else if (rel >= w2) {
			out_x = info.move_x2;
			out_y = info.move_y2;
		} else {
			double const t = double(rel - w1) / double(w2 - w1);
			out_x = info.move_x1 + (info.move_x2 - info.move_x1) * t;
			out_y = info.move_y1 + (info.move_y2 - info.move_y1) * t;
		}
		return true;
	}

	// Style / margins / alignment fallback (mirrors VisualToolBase::
	// GetLinePosition without the wx-bound context lookups).
	auto margin = line.Margin;
	int align = 2;
	if (AssStyle const* style = const_cast<AssFile&>(file).GetStyle(line.Style)) {
		align = style->alignment;
		for (int i = 0; i < 3; ++i)
			if (margin[i] == 0) margin[i] = style->Margin[i];
	}
	if (info.alignment_override > 0 && info.alignment_override <= 9)
		align = info.alignment_override;

	int const hor = (align - 1) % 3;
	int const vert = (align - 1) / 3;

	if (hor == 0)
		out_x = margin[0];
	else if (hor == 1)
		out_x = (script_width + margin[0] - margin[1]) / 2.0;
	else
		out_x = script_width - margin[1];

	if (vert == 0)
		out_y = script_height - margin[2];
	else if (vert == 1)
		out_y = script_height / 2.0;
	else
		out_y = margin[2];

	return true;
}

namespace {

// Strips the named tags from every override block and inserts `tag` into the
// first block (creating one when absent), matching SetOverride's placement
// convention. `extra_drops` names additional tags to strip wholesale (the
// growth-compensated emission owns its tag families).
std::string ReplaceTagsDropping(std::string const& text,
								std::string const& tag,
								bool drop_transform_tags,
								std::vector<std::string_view> extra_drops = {}) {
	std::string out;
	out.reserve(text.size() + tag.size() + 4);

	auto is_dropped_tag = [&](size_t at, size_t name_len) {
		auto matches = [&](std::string_view name) {
			return name_len == name.size() && text.compare(at, name.size(), name) == 0;
		};
		if (matches("\\pos") || matches("\\move"))
			return true;
		if (drop_transform_tags && (matches("\\frz") || matches("\\fscx") || matches("\\fscy")))
			return true;
		for (auto name : extra_drops)
			if (matches(name))
				return true;
		return false;
	};

	size_t i = 0;
	bool inserted = false;
	while (i < text.size()) {
		char const c = text[i];
		if (c != '{') {
			out.push_back(c);
			++i;
			continue;
		}

		out.push_back('{');
		++i;

		// Copy the block contents, dropping the selected tags entirely.
		while (i < text.size() && text[i] != '}') {
			if (text[i] == '\\') {
				size_t j = i + 1;
				while (j < text.size() && IsTagNameChar(text[j])) ++j;
				size_t const name_len = j - i;
				if (is_dropped_tag(i, name_len)) {
					// Consume the argument list up to ')' or the next token.
					size_t k = j;
					while (k < text.size() && text[k] != '}'
					       && text[k] != '\\') {
						if (text[k] == ')') {
							++k;
							break;
						}
						++k;
					}
					i = k;
					continue;
				}
				out.append(text, i, name_len);
				i = j;
				continue;
			}
			out.push_back(text[i]);
			++i;
		}

		if (!inserted) {
			out += tag;
			inserted = true;
		}
	}

	if (!inserted)
		return "{" + tag + "}" + out;
	return out;
}
}

std::string ReplacePositionTag(std::string const& text, std::string const& tag) {
	return ReplaceTagsDropping(text, tag, false);
}

std::string ReplaceMotionTags(std::string const& text, std::string const& tag) {
	return ReplaceTagsDropping(text, tag, true);
}

namespace {

// Inserts `tag` at the head of the text's first override block, creating a
// leading block when the text starts with plain text -- the same placement
// convention ApplyAssFade uses for its fade tags. The per-part \fade
// emission below runs after every part's stale fade representation has been
// stripped (align_video_fade::StripClaimableFade), so the inserted tag is
// the only fade representation the part carries.
std::string InsertTagAtBlockStart(std::string const& text,
								  std::string const& tag) {
	if (!text.empty() && text.front() == '{')
		return "{" + tag + text.substr(1);
	return "{" + tag + "}" + text;
}

struct ResolvedPoint {
	int frame = -1;
	double x = 0.0; // script px
	double y = 0.0;
	bool held = false;
	// Similarity only: emitted \frz / \fscx / \fscy values for this frame
	// (already composed with the style base). Identity defaults.
	double rot_deg = 0.0;
	double scale_pct_x = 100.0;
	double scale_pct_y = 100.0;
	// Similarity only: local stretch the frame's transform applies along the
	// script x / y axes at the line anchor (|M * e_x| and |M * e_y|). Uniform
	// 1.0 for translation. Drives border/shadow/blur growth compensation.
	double growth_x = 1.0;
	double growth_y = 1.0;
};

std::string FormatCoord(double v, int decimals) {
	char buf[64];
	std::snprintf(buf, sizeof(buf), "%.*f", decimals, v);
	return buf;
}

TrackSample const* FindOkAt(std::vector<TrackSample> const& samples, int frame) {
	auto const* s = FindSample(samples, frame);
	return s && s->status == TrackStatus::Ok ? s : nullptr;
}

// Nearest Ok sample scanning away from `from` within decode bounds; nullptr
// when none exists on that side.
TrackSample const* FindNearestOk(std::vector<TrackSample> const& samples,
                                 int from, int step, FrameInterval const& decode) {
	for (int g = from; g >= decode.first && g <= decode.last; g += step) {
		if (auto const* ok = FindOkAt(samples, g)) return ok;
	}
	return nullptr;
}

// Local-linear (order-1 Savitzky-Golay) smoothing over a ±half_window frame
// window, applied inside one run so hold boundaries never mix tracked motion
// into a hold (or vice versa). Constant-velocity input reproduces exactly;
// white per-frame jitter shrinks by roughly 1/sqrt(window). Frames inside a
// run are contiguous integers, so the window is frame-indexed.
void SmoothRun(std::vector<ResolvedPoint>& pts, size_t i0, size_t i1,
               int half_window) {
	if (half_window <= 0 || i1 - i0 + 1 < 3) return;
	int const f_first = pts[i0].frame;
	int const f_last = pts[i1].frame;
	std::vector<double> sx(i1 - i0 + 1), sy(i1 - i0 + 1);
	for (size_t i = i0; i <= i1; ++i) {
		int const f = pts[i].frame;
		int const lo = std::max(f - half_window, f_first);
		int const hi = std::min(f + half_window, f_last);
		// Least-squares line v = a + b*(g-f) over the window, evaluated at
		// f itself; the centred abscissa keeps the normal matrix small and
		// means a is already the wanted value.
		double n = 0, sg = 0, sgg = 0, sxs = 0, sys = 0, sgx = 0, sgy = 0;
		for (int g = lo; g <= hi; ++g) {
			ResolvedPoint const& p = pts[i0 + size_t(g - f_first)];
			double const dg = g - f;
			n += 1;
			sg += dg;
			sgg += dg * dg;
			sxs += p.x;
			sys += p.y;
			sgx += dg * p.x;
			sgy += dg * p.y;
		}
		double const den = n * sgg - sg * sg;
		double const bx = den > 1e-12 ? (n * sgx - sg * sxs) / den : 0.0;
		double const by = den > 1e-12 ? (n * sgy - sg * sys) / den : 0.0;
		sx[i - i0] = (sxs - bx * sg) / n;
		sy[i - i0] = (sys - by * sg) / n;
	}
	for (size_t i = i0; i <= i1; ++i) {
		if (pts[i].held) continue; // holds are constant by construction
		pts[i].x = sx[i - i0];
		pts[i].y = sy[i - i0];
	}
}

// Trajectory stabilization chain.
//
// Adapted from croni1012/Aegisub src/typesetting_auto_motion.cpp
// (ISC license) -- StabilisedPlanar: the
// upstream post-pass over tracked planar observations. Ported as pure
// functions over one hold-separated run of values:
//   1. angle unwrap (adjacent differences normalized into (-180, 180]),
//   2. median-of-3 smoothing (clamped edges; exact for monotone input),
//   3. [1,2,3,2,1]-weighted smoothing, applied only where the full symmetric
//      window exists so constant-velocity input passes through exactly at the
//      run ends too,
//   4. seed-frame pinning (the reference observation is restored verbatim),
//   5. noise-floor flattening: a run whose total excursion and deviation from
//      the reference stay below the floor is pinned to the reference as pure
//      sensor noise.
// The existing local-linear SmoothRun / Savitzky-Golay pass is a separate
// feature (options.smooth_frames, Translation positions); this chain is
// additive and gated by options.stabilization, and deliberately leaves the
// Compact translation fit untouched.

// Step 1: returns the sequence with adjacent jumps of exactly +-360 deg
// removed so smoothing sees a continuous ramp.
std::vector<double> UnwrapDegrees(std::vector<double> const& v) {
	std::vector<double> out(v.size(), 0.0);
	double offset = 0.0;
	for (size_t i = 0; i < v.size(); ++i) {
		if (i > 0) {
			double const d = v[i] + offset - out[i - 1];
			if (d > 180.0)
				offset -= 360.0;
			else if (d < -180.0)
				offset += 360.0;
		}
		out[i] = v[i] + offset;
	}
	return out;
}

// Step 2: median of the clamped neighbourhood {i-1, i, i+1}.
std::vector<double> MedianSmooth3(std::vector<double> const& v) {
	if (v.size() < 3)
		return v;
	std::vector<double> out(v.size(), 0.0);
	for (size_t i = 0; i < v.size(); ++i) {
		double const a = v[i > 0 ? i - 1 : i];
		double const b = v[i];
		double const c = v[i + 1 < v.size() ? i + 1 : i];
		out[i] = std::max(std::min(a, b), std::min(std::max(a, b), c));
	}
	return out;
}

// Step 3: [1,2,3,2,1]/9 kernel where the full window fits; run ends keep
// their median-smoothed values. Symmetric weights reproduce any linear
// sequence exactly, which is what keeps perfectly linear input unchanged.
std::vector<double> WeightedSmooth512(std::vector<double> const& v) {
	if (v.size() < 5)
		return v;
	constexpr int w[5] = {1, 2, 3, 2, 1};
	std::vector<double> out = v;
	for (size_t i = 2; i + 2 < v.size(); ++i) {
		double sum = 0.0;
		for (int k = -2; k <= 2; ++k)
			sum += w[k + 2] * v[size_t(int(i) + k)];
		out[i] = sum / 9.0;
	}
	return out;
}

// Steps 4 + 5 applied to one run: `seed_index` is the run-local index of the
// seed frame (-1 when the seed frame is outside this run). `floor` is in the
// value's own units; flattening compares relative deviation against the
// reference for ratio-like quantities (floor_is_ratio).
void StabilizeRun(std::vector<double>& v, int seed_index, double floor,
				  bool floor_is_ratio) {
	if (v.empty())
		return;
	int const ref_index = seed_index >= 0 && size_t(seed_index) < v.size() ? seed_index : 0;
	double const reference = v[size_t(ref_index)];
	// Ratio floors are meaningless against a zero reference (degenerate
	// style); fall back to absolute deviation there.
	bool const ratio = floor_is_ratio && std::abs(reference) > 1e-9;

	std::vector<double> const original = v;
	v = WeightedSmooth512(MedianSmooth3(v));

	// Pin the seed frame back to its exact observation: the seed pose is the
	// anchor everything else was tracked relative to.
	if (seed_index >= 0 && size_t(seed_index) < v.size())
		v[size_t(seed_index)] = original[size_t(seed_index)];

	// Noise floor: total excursion and deviation from the reference both
	// below the floor means the run is a noisy hold of the reference. For
	// ratio floors the excursion is a ratio too — an absolute range on a
	// ~100%-scale channel would never compare against a 0.012-style floor.
	auto const bounds = std::minmax_element(v.begin(), v.end());
	double const range = *bounds.second - *bounds.first;
	double const effective_range =
		ratio ? range / std::abs(reference) : range;
	double const ref_dev = ratio
							   ? std::max(std::abs(*bounds.first / reference - 1.0),
										  std::abs(*bounds.second / reference - 1.0))
							   : std::max(std::abs(*bounds.first - reference),
										  std::abs(*bounds.second - reference));
	if (effective_range < floor && ref_dev < floor)
		std::fill(v.begin(), v.end(), reference);
}

// Extracts one channel of a hold-separated run as a value sequence.
std::vector<double> ChannelOf(std::vector<ResolvedPoint> const& points,
							  std::pair<size_t, size_t> const& run,
							  double ResolvedPoint::*field) {
	std::vector<double> v(run.second - run.first + 1);
	for (size_t i = run.first; i <= run.second; ++i)
		v[i - run.first] = points[i].*field;
	return v;
}

// One emitted segment of the Compact fit: knots are sample indices, and the
// coordinates are FITTED knot positions (which adjacent pieces share exactly),
// not the sample values.
struct FitPiece {
	size_t i0 = 0, i1 = 0;
	double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;
};

// Solves the continuous piecewise-linear least-squares fit for one axis.
// `knots` are ascending sample indices; the unknowns are the knot values, the
// basis functions are the hats over adjacent knot frames, so the normal
// matrix is tridiagonal (symmetric positive definite; Thomas, no pivoting)
// and position continuity between segments holds by construction.
std::vector<double> FitKnotAxis(std::vector<ResolvedPoint> const& points,
                                std::vector<size_t> const& knots,
                                bool y_axis) {
	size_t const K = knots.size();
	std::vector<double> diag(K, 0.0), off(K - 1, 0.0), rhs(K, 0.0);
	auto const sample = [&](ResolvedPoint const& p) {
		return y_axis ? p.y : p.x;
	};
	size_t seg = 0;
	for (size_t s = knots.front(); s <= knots.back(); ++s) {
		int const f = points[s].frame;
		while (points[knots[seg + 1]].frame < f) ++seg;
		int const fa = points[knots[seg]].frame;
		int const fb = points[knots[seg + 1]].frame;
		double wa = 1.0, wb = 0.0;
		if (fb > fa) {
			wb = double(f - fa) / double(fb - fa);
			wa = 1.0 - wb;
		}
		double const v = sample(points[s]);
		diag[seg] += wa * wa;
		diag[seg + 1] += wb * wb;
		off[seg] += wa * wb;
		rhs[seg] += wa * v;
		rhs[seg + 1] += wb * v;
	}
	for (size_t i = 1; i < K; ++i) {
		double const m = off[i - 1] / diag[i - 1];
		diag[i] -= m * off[i - 1];
		rhs[i] -= m * rhs[i - 1];
	}
	std::vector<double> c(K);
	c[K - 1] = rhs[K - 1] / diag[K - 1];
	for (size_t i = K - 1; i-- > 0;)
		c[i] = (rhs[i] - off[i] * c[i + 1]) / diag[i];
	return c;
}

// Max storage-space deviation of the samples from the fitted spline over
// `knots`; optionally reports the worst sample index.
double MaxDeviation(std::vector<ResolvedPoint> const& points,
                    std::vector<size_t> const& knots,
                    std::vector<double> const& cx,
                    std::vector<double> const& cy,
                    double inv_scale_x, double inv_scale_y,
                    size_t* worst = nullptr) {
	double worst_d = -1.0;
	size_t worst_s = knots.front();
	size_t seg = 0;
	for (size_t s = knots.front(); s <= knots.back(); ++s) {
		int const f = points[s].frame;
		while (points[knots[seg + 1]].frame < f) ++seg;
		int const fa = points[knots[seg]].frame;
		int const fb = points[knots[seg + 1]].frame;
		double wa = 1.0, wb = 0.0;
		if (fb > fa) {
			wb = double(f - fa) / double(fb - fa);
			wa = 1.0 - wb;
		}
		double const ex = cx[seg] * wa + cx[seg + 1] * wb;
		double const ey = cy[seg] * wa + cy[seg + 1] * wb;
		double const d = std::hypot(
		    (points[s].x - ex) * inv_scale_x,
		    (points[s].y - ey) * inv_scale_y);
		if (d > worst_d) {
			worst_d = d;
			worst_s = s;
		}
	}
	if (worst) *worst = worst_s;
	return worst_d;
}

// Adaptive fit over one forced run: start with a single segment, solve for
// knot values, and add knots at the frame of maximum deviation — measured in
// storage pixels, so the threshold means on-screen error — until EVERY frame
// is within `eps_storage` or every frame is a knot (the exact interpolant,
// which the loop reaches first for finite data). When the worst frame is
// already a knot, the nearest non-knot neighbour is added instead:
// least-squares residuals peak at knots, and only added freedom next to them
// can shrink those.
//
// A backward-elimination pass then greedily drops every interior knot whose
// removal keeps all frames within eps. The forward loop alone over-splits
// badly on curved paths (residuals shuffle around as knots are added, so it
// keeps adding until nearly every frame is a knot); pruning that feasible
// set lands below the old Douglas-Peucker segment count while keeping the
// least-squares fit and the per-frame epsilon guarantee.
std::vector<FitPiece> FitRunPieces(std::vector<ResolvedPoint> const& points,
                                   std::pair<size_t, size_t> const& run,
                                   double eps_storage, double inv_scale_x,
                                   double inv_scale_y) {
	std::vector<FitPiece> pieces;
	size_t const n = run.second - run.first + 1;
	if (n < 2) {
		FitPiece solo;
		solo.i0 = solo.i1 = run.first;
		solo.x0 = solo.x1 = points[run.first].x;
		solo.y0 = solo.y1 = points[run.first].y;
		pieces.push_back(solo);
		return pieces;
	}

	std::vector<size_t> knots{run.first, run.second};
	std::vector<double> cx, cy;
	auto is_knot = [&](size_t s) {
		return std::binary_search(knots.begin(), knots.end(), s);
	};
	while (true) {
		cx = FitKnotAxis(points, knots, false);
		cy = FitKnotAxis(points, knots, true);
		size_t worst = run.first;
		double const worst_d =
		    MaxDeviation(points, knots, cx, cy, inv_scale_x, inv_scale_y,
		                 &worst);
		if (worst_d <= eps_storage || knots.size() >= n)
			break;
		size_t add = worst;
		if (is_knot(worst)) {
			// The worst frame is itself a knot: its least-squares residual
			// can only shrink by giving the fit more freedom next to it, so
			// add the nearest frame that is not a knot yet.
			add = SIZE_MAX;
			for (size_t r = 1; r <= n; ++r) {
				if (worst >= run.first + r && !is_knot(worst - r)) {
					add = worst - r;
					break;
				}
				if (worst + r <= run.second && !is_knot(worst + r)) {
					add = worst + r;
					break;
				}
			}
			if (add == SIZE_MAX) break; // every frame is a knot already
		}
		knots.insert(std::lower_bound(knots.begin(), knots.end(), add), add);
	}

	bool removed_any = true;
	while (removed_any && knots.size() > 2) {
		removed_any = false;
		size_t k = 1;
		while (k + 1 < knots.size()) {
			std::vector<size_t> cand(knots);
			cand.erase(cand.begin() + k);
			auto chk_x = FitKnotAxis(points, cand, false);
			auto chk_y = FitKnotAxis(points, cand, true);
			if (MaxDeviation(points, cand, chk_x, chk_y, inv_scale_x,
			                 inv_scale_y)
			    <= eps_storage) {
				knots = std::move(cand);
				cx = std::move(chk_x);
				cy = std::move(chk_y);
				removed_any = true;
				// A new knot slides into slot k; try it too.
			} else {
				++k;
			}
		}
	}

	for (size_t k = 0; k + 1 < knots.size(); ++k) {
		FitPiece p;
		p.i0 = knots[k];
		p.i1 = knots[k + 1];
		p.x0 = cx[k];
		p.y0 = cy[k];
		p.x1 = cx[k + 1];
		p.y1 = cy[k + 1];
		pieces.push_back(p);
	}
	return pieces;
}

} // namespace

MotionTrackApplyPlan BuildApplyPlan(
	AssFile const& file,
	std::vector<AssDialogue*> const& targets,
	ApplyPlanInput const& input) {
	MotionTrackApplyPlan plan;

	if (input.model != TrackModel::Translation
	    && input.model != TrackModel::Similarity) {
		plan.status = ApplyPlanStatus::UnsupportedModel;
		plan.message = "only Translation and Similarity trajectories can be "
		               "applied";
		return plan;
	}
	if (input.model == TrackModel::Similarity
	    && input.options.mode != ApplyMode::Exact) {
		plan.status = ApplyPlanStatus::UnsupportedMode;
		plan.message = "similarity trajectories support Exact apply mode only";
		return plan;
	}
	if (input.video_frame_count <= 0) {
		plan.status = ApplyPlanStatus::InvalidInput;
		plan.message = "no video frames";
		return plan;
	}
	if (input.storage_width <= 0 || input.storage_height <= 0
	    || input.script_width <= 0 || input.script_height <= 0) {
		plan.status = ApplyPlanStatus::InvalidInput;
		plan.message = "missing resolution information";
		return plan;
	}

	double const scale_x =
	    double(input.script_width) / double(input.storage_width);
	double const scale_y =
	    double(input.script_height) / double(input.storage_height);
	// Compact's deviation threshold is defined in storage pixels; deviations
	// are measured in script space, so convert per axis for the comparison.
	double const inv_scale_x = 1.0 / scale_x;
	double const inv_scale_y = 1.0 / scale_y;

	// \fad composition (options.apply_fad): project the session-detected
	// interval's full-visibility boundaries once per plan, with the event
	// semantics BuildAssFadeTiming uses for its event bounds -- a frame is
	// fully displayed over its [START, END) window, so the visibility
	// plateau is exactly [START(fade-in full), END(fade-out full)]. Each
	// applied line then clamps these global anchors into its OWN [Start,
	// End] (below): the old composition derived the durations from the
	// fitted event span while Start/End stayed the source line's, so a line
	// shorter than / offset from the fitted ramp got wrong, even spurious,
	// fades.
	struct FadeAnchors {
		bool has_in = false;
		int in_full_ms = 0; // visibility reaches the plateau at this time
		bool has_out = false;
		int out_full_ms = 0; // visibility starts dropping at this time
	};
	FadeAnchors fade_anchors;
	bool const fade_active =
		input.options.apply_fad && input.fade_interval && (input.fade_interval->fade_in.detected || input.fade_interval->fade_out.detected);
	if (fade_active) {
		auto const& interval = *input.fade_interval;
		if (interval.fade_in.detected) {
			fade_anchors.has_in = true;
			fade_anchors.in_full_ms = input.timecodes.TimeAtFrame(
				interval.fade_in.full_visibility_frame,
				agi::vfr::Time::START);
		}
		if (interval.fade_out.detected) {
			fade_anchors.has_out = true;
			fade_anchors.out_full_ms = input.timecodes.TimeAtFrame(
				interval.fade_out.full_visibility_frame,
				agi::vfr::Time::END);
		}
	}

	std::vector<PlannedLine> planned_lines;
	std::vector<LineUncovered> uncovered_lines;
	size_t event_count = 0;
	bool any_uncovered = false;

	for (AssDialogue* line : targets) {
		if (!line || line->Comment) continue;

		auto const line_start = int(line->Start);
		auto const line_end = int(line->End);
		if (line_end <= line_start) continue;

		int const lf = std::clamp(
			input.timecodes.FrameAtTime(line_start, agi::vfr::Time::START),
			0, input.video_frame_count - 1);
		int const ll = std::clamp(
			input.timecodes.FrameAtTime(line_end, agi::vfr::Time::END),
			0, input.video_frame_count - 1);

		FrameInterval domain;
		domain.first = std::max(lf, input.direction_domain.first);
		domain.last = std::min(ll, input.direction_domain.last);
		if (domain.first > domain.last) continue;

		int const dom_start_ms = std::max(
			line_start,
			input.timecodes.TimeAtFrame(domain.first, agi::vfr::Time::START));
		int const dom_end_ms = std::min(
			line_end,
			input.timecodes.TimeAtFrame(domain.last, agi::vfr::Time::END));
		if (dom_end_ms <= dom_start_ms) continue;

		double origin_x = 0.0, origin_y = 0.0;
		if (!ResolveDialogueOrigin(file, *line, input.seed_time_ms,
		                           input.script_width, input.script_height,
		                           origin_x, origin_y)) {
			plan.status = ApplyPlanStatus::InvalidInput;
			plan.message = "line has an invalid explicit \\move window";
			return plan;
		}

		// Base for similarity tag emission: \frz/\fscx/\fscy REPLACE the
		// style values, so the styled look must be composed in. Inline tags
		// override the style and become the effective base, so user styling
		// survives the apply.
		double style_scale_x = 100.0, style_scale_y = 100.0, style_angle = 0.0;
		double base_angle = 0.0, base_scale_x = 100.0, base_scale_y = 100.0;
		bool inline_frz = false, inline_scale = false;
		double style_outline_w = 0.0, style_shadow_w = 0.0;
		PositionInfo pose_info;
		if (input.model == TrackModel::Similarity) {
			pose_info = ExtractPositionInfo(*line);
			inline_frz = pose_info.has_frz;
			inline_scale = pose_info.has_fscx || pose_info.has_fscy;
			if (AssStyle const* st =
			        const_cast<AssFile&>(file).GetStyle(line->Style)) {
				style_scale_x = st->scalex;
				style_scale_y = st->scaley;
				style_angle = st->angle;
				style_outline_w = st->outline_w;
				style_shadow_w = st->shadow_w;
			}
			base_angle = pose_info.has_frz ? pose_info.frz : style_angle;
			base_scale_x = pose_info.has_fscx ? pose_info.fscx : style_scale_x;
			base_scale_y = pose_info.has_fscy ? pose_info.fscy : style_scale_y;
		}

		// Resolves one sample into the line anchor's script-space position
		// (and, for similarity, the emitted transform values). Translation
		// moves the anchor by the trajectory delta; similarity carries the
		// anchor with the full pose — its offset from the ROI seed center is
		// rotated and scaled about the current center, so a subtitle placed
		// off-center on the object stays glued to it as the object turns.
		auto resolve_point = [&](TrackSample const& s, int frame,
		                         bool held) {
			ResolvedPoint p;
			p.frame = frame;
			p.held = held;
			if (input.model == TrackModel::Translation) {
				p.x = origin_x
				    + (s.center_x - input.origin_center_x) * scale_x;
				p.y = origin_y
				    + (s.center_y - input.origin_center_y) * scale_y;
				return p;
			}
			double const m00 = s.transform.matrix[0];
			double const m01 = s.transform.matrix[1];
			double const m10 = s.transform.matrix[3];
			double const m11 = s.transform.matrix[4];
			double const ox = origin_x / scale_x - input.origin_center_x;
			double const oy = origin_y / scale_y - input.origin_center_y;
			p.x = (s.center_x + m00 * ox + m01 * oy) * scale_x;
			p.y = (s.center_y + m10 * ox + m11 * oy) * scale_y;
			double const theta_cw = std::atan2(m10, m00);
			double const scale = std::hypot(m00, m10);
			// \frz and the style Angle are counterclockwise-positive degrees;
			// theta is clockwise-positive in the Y-down image space, so they
			// compose with opposite signs.
			p.rot_deg = base_angle - theta_cw * 180.0 / std::numbers::pi;
			p.scale_pct_x = base_scale_x * scale;
			p.scale_pct_y = base_scale_y * scale;
			// Local stretch the transform applies along the script axes at
			// the anchor: |M * e_x| and |M * e_y| of the seed-relative linear
			// part. Uniform (== scale) for a similarity pose; carried per
			// axis so border/shadow/blur growth composes with anisotropic
			// bases.
			// Adapted from croni1012/Aegisub src/typesetting_motion.cpp
			// (ISC license) -- growth.
			p.growth_x = std::hypot(m00, m10);
			p.growth_y = std::hypot(m01, m11);
			return p;
		};

		// Resolve every frame in the domain; interior Failed gaps hold the
		// previous Ok pose only when bounded by Ok on both sides.
		std::vector<ResolvedPoint> points;
		std::vector<UncoveredRange> uncovered;
		for (int f = domain.first; f <= domain.last; ++f) {
			if (auto const* ok = FindOkAt(input.samples, f)) {
				points.push_back(resolve_point(*ok, f, false));
				continue;
			}
			auto const* s = FindSample(input.samples, f);
			if (s && s->status == TrackStatus::Failed) {
				auto const* before = FindNearestOk(
					input.samples, f - 1, -1, input.decode_interval);
				auto const* after = FindNearestOk(
					input.samples, f + 1, +1, input.decode_interval);
				if (before && after) {
					points.push_back(resolve_point(*before, f, true));
					continue;
				}
			}
			if (!uncovered.empty()
			    && uncovered.back().last == f - 1)
				uncovered.back().last = f;
			else
				uncovered.push_back(UncoveredRange{f, f});
		}

		if (!uncovered.empty()) {
			any_uncovered = true;
			uncovered_lines.push_back(LineUncovered{line, std::move(uncovered)});
			continue;
		}
		if (points.empty()) continue;

		// Split points into forced runs at hold boundaries (held <-> tracked
		// transitions): segments must never interpolate across a gap hold.
		std::vector<std::pair<size_t, size_t>> runs;
		size_t run_start = 0;
		for (size_t i = 1; i < points.size(); ++i) {
			bool const boundary =
			    points[i].held != points[i - 1].held;
			if (boundary) {
				runs.emplace_back(run_start, i - 1);
				run_start = i;
			}
		}
		runs.emplace_back(run_start, points.size() - 1);

		// Optional local-linear smoothing inside each run, before any
		// simplification sees the points. Position-only by construction —
		// smoothing a similarity pose would desynchronize position from
		// rotation/scale, so it stays a Translation feature.
		if (input.model == TrackModel::Translation)
			for (auto const& run : runs)
				SmoothRun(points, run.first, run.second,
				          input.options.smooth_frames);

		// Optional stabilization chain (see StabilizeRun): similarity pose
		// channels always, positions in Exact mode. Never touches held runs
		// (constant by construction) and never crosses hold boundaries; the
		// Compact translation fit is deliberately left to its own machinery.
		if (input.options.stabilization.enable) {
			auto const& stab = input.options.stabilization;
			int const seed_frame = std::clamp(
				input.timecodes.FrameAtTime(input.seed_time_ms),
				domain.first, domain.last);
			for (auto const& run : runs) {
				if (points[run.first].held)
					continue;
				size_t const n = run.second - run.first + 1;
				if (n < 2)
					continue;
				int seed_index = -1;
				for (size_t i = run.first; i <= run.second; ++i)
					if (points[i].frame == seed_frame)
						seed_index = int(i - run.first);

				// Unwrap-then-stabilize for the angle channel; the emitted
				// \frz stays rendering-equivalent modulo 360 deg.
				auto stabilize_channel = [&](double ResolvedPoint::*field,
											 double floor, bool ratio) {
					std::vector<double> v = ChannelOf(points, run, field);
					if (field == &ResolvedPoint::rot_deg)
						v = UnwrapDegrees(v);
					StabilizeRun(v, seed_index, floor, ratio);
					for (size_t i = run.first; i <= run.second; ++i)
						points[i].*field = v[i - run.first];
				};

				if (input.model == TrackModel::Similarity) {
					stabilize_channel(&ResolvedPoint::rot_deg,
									  stab.angle_floor_deg, false);
					stabilize_channel(&ResolvedPoint::scale_pct_x,
									  stab.scale_floor, true);
					stabilize_channel(&ResolvedPoint::scale_pct_y,
									  stab.scale_floor, true);
				}
				if (input.options.mode == ApplyMode::Exact) {
					stabilize_channel(&ResolvedPoint::x,
									  stab.position_floor_storage_px * scale_x,
									  false);
					stabilize_channel(&ResolvedPoint::y,
									  stab.position_floor_storage_px * scale_y,
									  false);
				}
			}
		}

		std::vector<FitPiece> pieces;
		if (input.options.mode == ApplyMode::Exact) {
			// Merge consecutive frames whose rounded values coincide, but
			// never across a hold boundary: the forced runs are hard cut
			// points for fitting, so nothing interpolates across a gap hold.
			// (A post-pass below still folds adjacent parts whose emitted
			// text is identical, which renders the same at every frame
			// time.)
			// Similarity groups additionally require the rounded pose
			// (\frz/\fscx/\fscy) to match.
			auto rounded2 = [](double v) {
				return std::round(v * 100.0) / 100.0;
			};
			auto same_rounded = [&](size_t a, size_t b) {
				bool const same_pos =
				    std::round(points[a].x) == std::round(points[b].x)
				    && std::round(points[a].y) == std::round(points[b].y);
				if (!same_pos
				    || input.model != TrackModel::Similarity)
					return same_pos;
				return rounded2(points[a].rot_deg)
				         == rounded2(points[b].rot_deg)
				    && rounded2(points[a].scale_pct_x)
				         == rounded2(points[b].scale_pct_x)
				    && rounded2(points[a].scale_pct_y)
				         == rounded2(points[b].scale_pct_y);
			};
			for (auto const& run : runs) {
				size_t group_start = run.first;
				for (size_t i = run.first + 1; i <= run.second; ++i) {
					if (!same_rounded(i, i - 1)) {
						FitPiece p;
						p.i0 = group_start;
						p.i1 = i - 1;
						p.x0 = p.x1 = points[group_start].x;
						p.y0 = p.y1 = points[group_start].y;
						pieces.push_back(p);
						group_start = i;
					}
				}
				FitPiece p;
				p.i0 = group_start;
				p.i1 = run.second;
				p.x0 = p.x1 = points[group_start].x;
				p.y0 = p.y1 = points[group_start].y;
				pieces.push_back(p);
			}
		} else {
			// Continuous piecewise-linear least-squares fit per forced run:
			// each segment is uniform-velocity, adjacent segments share their
			// boundary position exactly (no artificial velocity steps at
			// simplification vertices), and knots are added until the
			// storage-space deviation is within the threshold.
			for (auto const& run : runs) {
				auto run_pieces = FitRunPieces(
				    points, run, input.options.compact_epsilon,
				    inv_scale_x, inv_scale_y);
				pieces.insert(pieces.end(),
				              std::make_move_iterator(run_pieces.begin()),
				              std::make_move_iterator(run_pieces.end()));
			}
		}

		// Assemble parts: optional preserved prefix, covered pieces, optional
		// preserved suffix.
		PlannedLine pl;
		pl.source = line;

		int cursor = dom_start_ms;
		if (dom_start_ms > line_start) {
			PlannedLinePart prefix;
			prefix.start_ms = line_start;
			prefix.end_ms = dom_start_ms;
			prefix.text = line->Text;
			prefix.covered = false;
			pl.parts.push_back(std::move(prefix));
		}

		for (size_t p = 0; p < pieces.size(); ++p) {
			auto const& piece = pieces[p];
			PlannedLinePart part;
			part.start_ms = p == 0 ? dom_start_ms
			    : input.timecodes.TimeAtFrame(points[piece.i0].frame,
			                                  agi::vfr::Time::START);
			part.end_ms = p + 1 == pieces.size()
			    ? dom_end_ms
			    : input.timecodes.TimeAtFrame(points[piece.i1].frame + 1,
			                                  agi::vfr::Time::START);
			part.start_ms = std::clamp(part.start_ms, cursor, dom_end_ms);
			part.end_ms = std::clamp(part.end_ms, part.start_ms, dom_end_ms);
			cursor = part.end_ms;

			if (input.model == TrackModel::Similarity) {
				// Exact parts are static by construction: one constant pose
				// per part. Transform tags are emitted only when they carry
				// information — identity tracked pose with no inline base
				// renders correctly from the style alone, so a pure
				// translation yields plain \pos parts (the inline tags were
				// stripped, so an inline base is always re-emitted).
				ResolvedPoint const& pt = points[piece.i0];
				auto rounded2 = [](double v) {
					return std::round(v * 100.0) / 100.0;
				};
				bool const emit_frz =
				    inline_frz
				    || rounded2(pt.rot_deg) != rounded2(style_angle);
				bool const emit_scale =
				    inline_scale
				    || rounded2(pt.scale_pct_x) != rounded2(style_scale_x)
				    || rounded2(pt.scale_pct_y) != rounded2(style_scale_y);
				int const dec_s = input.options.position_decimals;
				std::string tag =
					"\\pos(" + FormatCoord(pt.x, dec_s) + "," + FormatCoord(pt.y, dec_s) + ")";
				if (emit_frz)
					tag += "\\frz(" + FormatCoord(pt.rot_deg, 2) + ")";
				if (emit_scale)
					tag += "\\fscx(" + FormatCoord(pt.scale_pct_x, 2) + ")\\fscy(" + FormatCoord(pt.scale_pct_y, 2) + ")";

				// Border/shadow/blur growth compensation: scale the composed
				// base (style + inline override) with the transform's local
				// stretch so outline width, shadow distance and blur radius
				// follow the object as it zooms. Per-axis tags when the user
				// styled the axes separately, the geometric mean otherwise.
				// A family is emitted unless the rounded compensated value
				// equals both the composed base and the style default:
				// identity transforms and zero bases emit nothing (the
				// pre-feature output is unchanged), while a value that came
				// from an inline override is always re-emitted compensated,
				// so a leftover override can never swallow the compensation.
				// Adapted from croni1012/Aegisub src/typesetting_motion.cpp
				// (ISC license) --
				// growth compensation.
				std::vector<std::string_view> growth_drops;
				double const growth_geo = std::sqrt(
					std::max(0.0, pt.growth_x * pt.growth_y));
				auto scaled_differs = [](double base, double growth,
										 double style_default) {
					return FormatCoord(base * growth, 2) != FormatCoord(base, 2) || FormatCoord(base, 2) != FormatCoord(style_default, 2);
				};
				if (input.options.scale_border) {
					double const base =
						pose_info.has_bord ? pose_info.bord : style_outline_w;
					if (pose_info.has_xbord || pose_info.has_ybord) {
						double const bx =
							pose_info.has_xbord ? pose_info.xbord : base;
						double const by =
							pose_info.has_ybord ? pose_info.ybord : base;
						bool const emit_x =
							scaled_differs(bx, pt.growth_x, style_outline_w);
						bool const emit_y =
							scaled_differs(by, pt.growth_y, style_outline_w);
						if (emit_x || emit_y) {
							if (emit_x) {
								tag += "\\xbord(" + FormatCoord(bx * pt.growth_x, 2) + ")";
								growth_drops.emplace_back("\\xbord");
							}
							if (emit_y) {
								tag += "\\ybord(" + FormatCoord(by * pt.growth_y, 2) + ")";
								growth_drops.emplace_back("\\ybord");
							}
							// A uniform inline \bord would override the
							// per-axis values; it is absorbed into the base.
							growth_drops.emplace_back("\\bord");
						}
					}
					else if (scaled_differs(base, growth_geo,
											style_outline_w)) {
						tag += "\\bord(" + FormatCoord(base * growth_geo, 2) + ")";
						growth_drops.emplace_back("\\bord");
					}
				}
				if (input.options.scale_shadow) {
					double const base =
						pose_info.has_shad ? pose_info.shad : style_shadow_w;
					if (pose_info.has_xshad || pose_info.has_yshad) {
						double const sx =
							pose_info.has_xshad ? pose_info.xshad : base;
						double const sy =
							pose_info.has_yshad ? pose_info.yshad : base;
						bool const emit_x =
							scaled_differs(sx, pt.growth_x, style_shadow_w);
						bool const emit_y =
							scaled_differs(sy, pt.growth_y, style_shadow_w);
						if (emit_x || emit_y) {
							if (emit_x) {
								tag += "\\xshad(" + FormatCoord(sx * pt.growth_x, 2) + ")";
								growth_drops.emplace_back("\\xshad");
							}
							if (emit_y) {
								tag += "\\yshad(" + FormatCoord(sy * pt.growth_y, 2) + ")";
								growth_drops.emplace_back("\\yshad");
							}
							growth_drops.emplace_back("\\shad");
						}
					}
					else if (scaled_differs(base, growth_geo,
											style_shadow_w)) {
						tag += "\\shad(" + FormatCoord(base * growth_geo, 2) + ")";
						growth_drops.emplace_back("\\shad");
					}
				}
				if (input.options.scale_blur) {
					// \blur has no style counterpart; the base is the inline
					// value or 0, and 0 never emits.
					double const base = pose_info.has_blur ? pose_info.blur : 0.0;
					if (scaled_differs(base, growth_geo, 0.0)) {
						tag += "\\blur(" + FormatCoord(base * growth_geo, 2) + ")";
						growth_drops.emplace_back("\\blur");
					}
				}

				part.text = ReplaceTagsDropping(line->Text, tag, true,
												growth_drops);
				part.x0 = part.x1 = pt.x;
				part.y0 = part.y1 = pt.y;
				part.covered = true;
				pl.parts.push_back(std::move(part));
				continue;
			}

			// The tag endpoints are the fitted spline evaluated at the part's
			// own time boundaries. The previous part owns the shared knot's
			// frame (rendering its position at that frame's timecode), so
			// this part starts one frame boundary later and must begin from
			// the spline value THERE — starting from the knot value would lag
			// the trajectory by a full frame at every part boundary.
			int const ta = input.timecodes.TimeAtFrame(points[piece.i0].frame);
			int const tb = input.timecodes.TimeAtFrame(points[piece.i1].frame);
			auto spline_at = [&](int t_ms, double ka, double kb) {
				double const u = tb > ta
				    ? std::clamp(double(t_ms - ta) / double(tb - ta), 0.0, 1.0)
				    : 0.0;
				return ka + (kb - ka) * u;
			};
			double const ex0 = spline_at(part.start_ms, piece.x0, piece.x1);
			double const ey0 = spline_at(part.start_ms, piece.y0, piece.y1);
			double const ex1 = piece.x1; // t2 below anchors on tb exactly
			double const ey1 = piece.y1;

			int const dec = input.options.position_decimals;
			std::string const x0s = FormatCoord(ex0, dec);
			std::string const y0s = FormatCoord(ey0, dec);
			std::string tag;
			if (input.options.mode == ApplyMode::Exact
			    || (std::round(ex0) == std::round(ex1)
			        && std::round(ey0) == std::round(ey1))) {
				tag = "\\pos(" + x0s + "," + y0s + ")";
			} else {
				// The endpoint anchors land on the frames the endpoints were
				// sampled at (a frame renders at its EXACT timecode), with an
				// explicit window: renderers -- and ResolveDialogueOrigin
				// above -- read a t1 or t2 of <= 0 as "no explicit window",
				// so both stay strictly positive and ordered. Nudging t1 from
				// 0 to 1 costs a sub-pixel of accuracy; being misread as a
				// 4-arg \move costs a whole frame.
				int const dur = part.end_ms - part.start_ms;
				int const t1 = std::clamp(
				    ta - part.start_ms, 1, std::max(1, dur - 1));
				int const t2 = std::clamp(
				    tb - part.start_ms, t1 + 1, std::max(t1 + 1, dur));
				tag = "\\move(" + x0s + "," + y0s + ","
				    + FormatCoord(ex1, dec) + ","
				    + FormatCoord(ey1, dec) + ","
				    + std::to_string(t1) + "," + std::to_string(t2) + ")";
			}
			part.text = ReplacePositionTag(line->Text, tag);
			part.covered = true;
			part.x0 = ex0;
			part.y0 = ey0;
			part.x1 = ex1;
			part.y1 = ey1;
			pl.parts.push_back(std::move(part));
		}

		// Output-side merge for Exact mode: adjacent covered parts whose
		// emitted text is byte-identical become one part spanning both.
		// Hold-separated runs stay hard cut points for FITTING (a piece
		// must never interpolate across a gap hold), but the static parts
		// they produce can still coincide — most often a pose held through
		// an interior Failed gap that matches the tracked pose either side
		// — and a duplicate event there changes nothing a renderer shows:
		// both parts render the same constant tags, so the merged part
		// renders identically at every frame time. The previous part keeps
		// owning the shared knot's frame; the merged part just extends to
		// the second part's end. Time adjacency is the exact tiling the
		// part loop above already guarantees.
		// Adapted from croni1012/Aegisub src/typesetting_motion.cpp
		// (ISC license) -- extending the previous output event when the
		// next frame's generated line is identical.
		if (input.options.mode == ApplyMode::Exact) {
			size_t write = 1;
			for (size_t read = 1; read < pl.parts.size(); ++read) {
				PlannedLinePart& prev = pl.parts[write - 1];
				if (prev.covered && pl.parts[read].covered && prev.end_ms == pl.parts[read].start_ms && prev.text == pl.parts[read].text) {
					prev.end_ms = pl.parts[read].end_ms;
					prev.x1 = pl.parts[read].x1;
					prev.y1 = pl.parts[read].y1;
					continue;
				}
				if (write != read)
					pl.parts[write] = std::move(pl.parts[read]);
				++write;
			}
			pl.parts.resize(write);
		}

		// Events are the covered parts that survive the merge.
		for (auto const& part : pl.parts)
			if (part.covered)
				++event_count;

		if (cursor < dom_end_ms) {
			// Defensive: rounding clamps should make this unreachable, but a
			// trailing filler keeps the timeline seamless.
			PlannedLinePart tail;
			tail.start_ms = cursor;
			tail.end_ms = dom_end_ms;
			tail.text = line->Text;
			tail.covered = false;
			pl.parts.push_back(std::move(tail));
		}
		if (dom_end_ms < line_end) {
			PlannedLinePart suffix;
			suffix.start_ms = dom_end_ms;
			suffix.end_ms = line_end;
			suffix.text = line->Text;
			suffix.covered = false;
			pl.parts.push_back(std::move(suffix));
		}

		// Per-part fade slicing (options.apply_fad): the dialog turns each
		// planned part into a SEPARATE ASS event, and an event's fade
		// windows are relative to its own duration -- so a \fad composed
		// against the whole line span would land its fade-out inside the
		// first part's short window while the later parts pop back in fully
		// opaque. Instead the interval's ramps, clamped into the LINE's own
		// [Start, End] (never moved), define one global alpha curve, and
		// every covered part receives the \fade that reproduces that curve
		// over the part's local time [0, part_end - part_start].
		if (fade_active && !pl.parts.empty()) {
			// Line-level durations: the global anchors clamped into the
			// line's span and rounded with RoundAssFadeTimingToCentisecond's
			// conventions (which also enforce fade_in + fade_out <= the
			// line duration, so the two ramps can never overlap).
			auto const line_timing =
				align_video_fade::RoundAssFadeTimingToCentiseconds(
					align_video_fade::AssFadeTiming{
						line_start,
						line_end,
						fade_anchors.has_in
							? fade_anchors.in_full_ms - line_start
							: 0,
						fade_anchors.has_out
							? line_end - fade_anchors.out_full_ms
							: 0});
			int const fade_in_ms = line_timing.fade_in_ms;
			int const fade_out_ms = line_timing.fade_out_ms;
			int const ramp_in_end = line_start + fade_in_ms;
			int const ramp_out_start = line_end - fade_out_ms;

			// Global curve alpha (ASS units: 0 opaque .. 255 transparent) at
			// absolute time t: 0 (or the curve's mid-ramp value) while the
			// fade-in ramp climbs to full visibility, fully opaque through
			// the plateau, then down to fully transparent at line End.
			auto alpha_at = [&](int t) {
				double visibility = 1.0;
				if (fade_in_ms > 0 && t < ramp_in_end)
					visibility = double(t - line_start) / fade_in_ms;
				else if (fade_out_ms > 0 && t > ramp_out_start)
					visibility = double(line_end - t) / fade_out_ms;
				return int(std::lround((1.0 - visibility) * 255.0));
			};

			for (auto& part : pl.parts) {
				// Stale fade representations (all \fad/\fade plus the
				// leading overall-alpha \t form ApplyAssFade would claim)
				// are stripped from EVERY part: covered parts receive their
				// own slice below, while uncovered prefix/suffix parts
				// render outside the tracked domain and never get a new
				// one.
				part.text = align_video_fade::StripClaimableFade(part.text);
				if (!part.covered)
					continue;

				int const dur = part.end_ms - part.start_ms;
				if (dur <= 0)
					continue;
				// Ramp breakpoints clamped into the part's local window and
				// rounded with the same centisecond conventions (the
				// normalization inside clamps them into [0, dur] and keeps
				// t2 <= t3). t1 is always 0 and t4 always the duration: the
				// ramps are clamped into the line's span and the covered
				// parts tile a subset of it.
				auto const part_timing =
					align_video_fade::RoundAssFadeTimingToCentiseconds(
						align_video_fade::AssFadeTiming{
							part.start_ms,
							part.end_ms,
							ramp_in_end - part.start_ms,
							part.end_ms - ramp_out_start});
				int const dur_cs =
					part_timing.end_ms - part_timing.start_ms;
				int const t2 = part_timing.fade_in_ms;
				int const t3 = dur_cs - part_timing.fade_out_ms;

				int a1 = alpha_at(part.start_ms);
				int a3 = alpha_at(part.end_ms);
				int a2 = 0; // the fully-visible plateau level
				if (t2 >= dur_cs)
					a2 = a3; // window entirely inside the fade-in ramp: the
							 // rendered start->plateau ramp must end at the
							 // curve's own value at the part end
				else if (t3 <= 0)
					a2 = a1; // window entirely inside the fade-out ramp: the
							 // rendered plateau->end ramp must start at the
							 // curve's own value at the part start
				if (a1 == 0 && a2 == 0 && a3 == 0)
					continue; // plateau-only part: nothing to reproduce

				// Endpoints no renderer can observe (a1 only renders before
				// t2, a3 only after t3) are normalized to the canonical
				// fully-transparent 255, so a window covering the line's
				// whole curve collapses to the simple \fad(in,out) form.
				if (t2 <= 0)
					a1 = 255;
				if (t3 >= dur_cs)
					a3 = 255;

				std::string tag;
				if (a1 == 255 && a2 == 0 && a3 == 255) {
					tag = "\\fad(" + std::to_string(t2) + "," + std::to_string(dur_cs - t3) + ")";
				}
				else {
					tag = "\\fade(" + std::to_string(a1) + "," + std::to_string(a2) + "," + std::to_string(a3) + ",0," + std::to_string(t2) + "," + std::to_string(t3) + "," + std::to_string(dur_cs) + ")";
				}
				part.text = InsertTagAtBlockStart(part.text, tag);
			}
		}

		planned_lines.push_back(std::move(pl));
	}

	if (any_uncovered) {
		plan.status = ApplyPlanStatus::IncompleteCoverage;
		plan.message = "trajectory does not cover every target line's apply "
		               "domain";
		plan.uncovered = std::move(uncovered_lines);
		return plan;
	}

	plan.status = event_count > 100 ? ApplyPlanStatus::NeedsConfirmation
	                                : ApplyPlanStatus::Ok;
	plan.event_count = event_count;
	plan.lines = std::move(planned_lines);
	return plan;
}

} // namespace aegisub::motion_track
