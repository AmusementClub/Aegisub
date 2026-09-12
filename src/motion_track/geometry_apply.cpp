#include "geometry_apply.h"

#include "geometry_tags.h"
#include "../ass_dialogue.h"
#include "../ass_file.h"
#include "../ass_tag_scanner.h"
#include "../perspective_ass_bounds.h"
#include "../perspective_ass_state.h"
#include "../perspective_apply_plan.h"
#include <libaegisub/ass/time.h>

#include <algorithm>
#include <cmath>
#include <string_view>
#include <utility>

namespace aegisub::motion_track {
namespace {
using namespace perspective;

MotionTrackApplyPlan Failure(std::string message) {
	MotionTrackApplyPlan result;
	result.status = ApplyPlanStatus::InvalidInput;
	result.message = std::move(message);
	return result;
}

void ScanGeometry(std::string_view text, bool& geometry, bool& clip) {
	auto scan_body = [&](auto const& self, std::string_view body, int depth) -> void {
		if (depth >= 32) {
			// Route excessive nesting through the geometry evaluator's
			// explicit failure path rather than bypassing its validation.
			clip = geometry = true;
			return;
		}
		ass_tag_scanner::ScanRawTags(body, [&](auto const& tag) {
			using ass_tag_scanner::NameHasPrefix;
			if (NameHasPrefix(tag.name, "clip") || NameHasPrefix(tag.name, "iclip"))
				clip = geometry = true;
			if (NameHasPrefix(tag.name, "org") || NameHasPrefix(tag.name, "fax") ||
				NameHasPrefix(tag.name, "fay") || NameHasPrefix(tag.name, "frx") || NameHasPrefix(tag.name, "fry"))
				geometry = true;
			if (NameHasPrefix(tag.name, "t") && tag.has_paren)
				self(self, tag.args, depth + 1);
		});
	};
	for (size_t at = 0; (at = text.find('{', at)) != std::string_view::npos;) {
		size_t const end = text.find('}', ++at);
		if (end == std::string_view::npos)
			break;
		scan_body(scan_body, text.substr(at, end - at), 0);
		at = end + 1;
	}
}

TrackTransform ScriptTransform(TrackSample const& sample, ApplyPlanInput const& input) {
	double const sx = static_cast<double>(input.script_width) / input.storage_width;
	double const sy = static_cast<double>(input.script_height) / input.storage_height;
	double const cx = input.origin_center_x, cy = input.origin_center_y;
	auto m = sample.transform.matrix;
	if (input.model == TrackModel::Translation)
		m = {1, 0, sample.center_x - cx, 0, 1, sample.center_y - cy, 0, 0, 1};
	else if (input.model == TrackModel::Similarity) {
		m[2] = sample.center_x - cx;
		m[5] = sample.center_y - cy;
	}
	Matrix3 const from_script({1 / sx, 0, -cx, 0, 1 / sy, -cy, 0, 0, 1});
	Matrix3 const to_script({sx, 0, sx * cx, 0, sy, sy * cy, 0, 0, 1});
	return {(to_script * Matrix3(m) * from_script).Values()};
}

struct GeometryFrame {
	int time = 0;
	bool held = false;
	Quad target;
	EvaluatedTransformState state;
	std::string text;
};

constexpr auto pose_channels = std::array{
	&EvaluatedTransformState::scale_x, &EvaluatedTransformState::scale_y,
	&EvaluatedTransformState::shear_x, &EvaluatedTransformState::shear_y,
	&EvaluatedTransformState::rotation_x, &EvaluatedTransformState::rotation_y,
	&EvaluatedTransformState::rotation_z};
constexpr auto pose_names = std::array{"\\fscx", "\\fscy", "\\fax", "\\fay", "\\frx", "\\fry", "\\frz"};

std::string AppendTags(std::string text, std::string const& tags) {
	if (!text.empty() && text.front() == '{') {
		auto const end = text.find('}');
		if (end != std::string::npos) {
			text.insert(end, tags);
			return text;
		}
	}
	return "{" + tags + "}" + text;
}

bool SameOrigin(EvaluatedTransformState const& a, EvaluatedTransformState const& b) {
	return a.origin.has_value() == b.origin.has_value() &&
		   (!a.origin || (a.origin->x == b.origin->x && a.origin->y == b.origin->y));
}

// Generate the actual ASS interpolation before checking its geometry. This
// includes decimal rounding and event-relative time windows in the error test.
std::string AnimatedText(GeometryFrame const& first, GeometryFrame const& last,
						 int event_start, int position_decimals) {
	int const t0 = std::max(0, first.time - event_start);
	int const t1 = std::max(t0 + 1, last.time - event_start);
	std::string text = first.text;
	if (first.state.position.x != last.state.position.x || first.state.position.y != last.state.position.y) {
		text = ReplacePositionTag(text, "\\move(" + FormatAssNumber(first.state.position.x, position_decimals) + "," +
											FormatAssNumber(first.state.position.y, position_decimals) + "," + FormatAssNumber(last.state.position.x, position_decimals) + "," +
											FormatAssNumber(last.state.position.y, position_decimals) + "," + std::to_string(t0) + "," + std::to_string(t1) + ")");
	}
	std::string animated;
	for (size_t i = 0; i < pose_channels.size(); ++i) {
		auto const channel = pose_channels[i];
		double end = last.state.*channel;
		if (i >= 4)
			end = first.state.*channel + std::remainder(end - first.state.*channel, 360.0);
		if (end != first.state.*channel)
			animated += std::string(pose_names[i]) + FormatAssNumber(end, 6);
	}
	if (!animated.empty())
		text = AppendTags(std::move(text), "\\t(" + std::to_string(t0) + "," + std::to_string(t1) + "," + animated + ")");
	return text;
}

} // namespace

bool NeedsGeometryApply(AssDialogue const& line, TrackModel model) {
	if (model == TrackModel::Affine || model == TrackModel::Homography)
		return true;
	bool geometry = false, clip = false;
	ScanGeometry(line.Text.get(), geometry, clip);
	return geometry;
}

MotionTrackApplyPlan BuildGeometryApplyPlan(AssFile const& file,
											AssDialogue *line, ApplyPlanInput const& input) {
	if (input.options.mode == ApplyMode::Compact && input.options.compact_epsilon <= 0)
		return Failure("full geometry Compact requires a positive pixel error budget");
	if (input.options.smooth_frames > 0 || input.options.stabilization.enable ||
		input.options.scale_border || input.options.scale_shadow || input.options.scale_blur)
		return Failure("disable smoothing, stabilization and border/shadow/blur scaling before applying full geometry tracking");
	// The position planner owns temporal coverage and per-event fade slicing.
	// Its frame-parts mode deliberately keeps distinct poses even when their
	// centers coincide (pure shear, zoom and perspective can all do that).
	ApplyPlanInput temporal = input;
	temporal.model = TrackModel::Translation;
	temporal.options.mode = ApplyMode::Exact;
	temporal.options.smooth_frames = 0;
	temporal.options.stabilization.enable = false;
	auto result = BuildPositionApplyPlan(file, {line}, temporal, true);
	if (!result.has_mutations())
		return result;

	double x = 0, y = 0;
	ResolveDialogueOrigin(file, *line, input.seed_time_ms, input.script_width, input.script_height, x, y);
	AssDialogue frozen(*line);
	frozen.Text = ReplacePositionTag(line->Text, "\\pos" + FormatAssPoint({.x = x, .y = y}, 6));
	// Selected lines can start after or end before the common tracking seed.
	// Geometry is static here; only the position anchor uses the original seed.
	int const capture_time = std::clamp(input.seed_time_ms,
										line->Start.GetMillisecond(), line->End.GetMillisecond() - 1);
	auto const evaluated = EvaluateEffectiveAssState({.file = &file, .line = &frozen, .play_resolution = {.width = static_cast<double>(input.script_width), .height = static_cast<double>(input.script_height)}, .capture_time_ms = capture_time});
	if (!evaluated)
		return Failure(DescribeAssStateError(evaluated.error));
	if (!evaluated.CanApply())
		return Failure(DescribeAssApplyBlocker(evaluated.apply_blocker));
	ForwardInput source;
	source.play_resolution = {.width = static_cast<double>(input.script_width), .height = static_cast<double>(input.script_height)};
	source.video_storage_resolution = Resolution{.width = static_cast<double>(input.storage_width), .height = static_cast<double>(input.storage_height)};
	int const layout_x = file.GetScriptInfoAsInt("LayoutResX");
	int const layout_y = file.GetScriptInfoAsInt("LayoutResY");
	if (layout_x > 0 && layout_y > 0)
		source.layout_resolution = Resolution{.width = static_cast<double>(layout_x), .height = static_cast<double>(layout_y)};
	auto const layout_aspect = ResolvePerspectiveLayoutAspect(source);
	if (!layout_aspect)
		return Failure("invalid script-to-layout coordinate mapping");
	auto const bounds = EvaluateAssBaseBounds({.line = &frozen, .state = &evaluated.value, .text_extents = input.text_extents, .layout_aspect = *layout_aspect});
	if (!bounds)
		return Failure(DescribeAssBoundsError(bounds.error));
	source.bounds = bounds.value;
	source.state = evaluated.value.transform;
	auto const base = ForwardQuad(source);
	if (!base)
		return Failure(DescribeForwardError(base.error));
	OutputCoordinateMapping const mapping{.scale_x = static_cast<double>(input.storage_width) / input.script_width,
										  .scale_y = static_cast<double>(input.storage_height) / input.script_height};
	bool geometry = false, clip = false;
	ScanGeometry(line->Text.get(), geometry, clip);
	std::vector<GeometryFrame> frames;
	auto& parts = result.lines.front().parts;
	frames.reserve(parts.size());
	PerspectiveTagRewriter const cost_writer(frozen.Text.get(), evaluated.value.transform,
											 evaluated.value.event_style_transform);
	double const position_factor = std::pow(10.0, input.options.position_decimals);
	double const output_error = input.options.mode == ApplyMode::Exact
									? 0.05
									: input.options.compact_epsilon;
	TrackSample const *previous = nullptr;
	for (auto& part : parts) {
		if (!part.covered) {
			frames.emplace_back();
			continue;
		}
		int const frame = input.timecodes.FrameAtTime(part.start_ms, agi::vfr::Time::START);
		auto const *sample = FindSample(input.samples, frame);
		bool const held = sample && sample->status == TrackStatus::Failed;
		if (held) {
			// A line may begin in a bounded held gap whose good predecessor
			// lies before the line. The temporal planner already validated it.
			if (!previous)
				for (auto const& s : input.samples) {
					if (s.frame >= frame)
						break;
					if (s.status == TrackStatus::Ok)
						previous = &s;
				}
			sample = previous;
		}
		if (!sample || sample->status != TrackStatus::Ok)
			return Failure("missing geometry sample at an applied frame");
		previous = sample;
		TrackTransform const matrix = ScriptTransform(*sample, input);
		Homography const transform{Matrix3(matrix.matrix)};
		Quad target;
		for (size_t i = 0; i < target.size(); ++i) {
			auto const point = transform.Map(base.quad[i]);
			if (!point)
				return Failure("tracked geometry crosses the perspective projection plane");
			target[i] = *point;
		}
		SolverInput solve;
		solve.source = source;
		solve.target = target;
		solve.output_mapping = mapping;
		solve.event_style = evaluated.value.event_style_transform;
		solve.locked_rotation_z = PerspectiveRotationLock(evaluated.value);
		solve.max_error = input.options.mode == ApplyMode::Exact
							  ? 0.05
							  : std::min(0.05, input.options.compact_epsilon * 0.25);
		solve.shape_tolerance = solve.max_error;
		// Check position precision during candidate selection and compression,
		// so an economical pose cannot crowd out another pose that survives
		// the final position rounding within the user's geometry error budget.
		solve.measure_tag_cost = [&](EvaluatedTransformState const& state,
									 SerializedTransformState const& serialized, PerspectiveScalePolicy scale_policy)
			-> std::optional<PerspectiveTagCost> {
			ForwardInput quantized = source;
			quantized.state = state;
			quantized.state.position = {.x = std::round(state.position.x * position_factor) / position_factor,
										.y = std::round(state.position.y * position_factor) / position_factor};
			auto const residual = MeasurePerspectiveResidual(quantized, target, mapping);
			if (!residual || residual.max_error > output_error)
				return std::nullopt;
			auto actual = serialized;
			actual.position = FormatAssPoint(quantized.state.position, input.options.position_decimals);
			auto const rewritten = cost_writer.Rewrite(quantized.state, actual, scale_policy,
													   PerspectiveRepresentationPolicy::Automatic);
			return rewritten ? std::optional(rewritten.cost) : std::nullopt;
		};
		auto const solved = SolvePerspectiveTags(solve);
		if (!solved)
			return Failure(std::string(DescribeSolverError(solved.error)) +
						   "; increase Position decimals or Compact error if the output precision is insufficient");
		if (solved.candidate->max_error > solve.max_error)
			return Failure("tracked geometry cannot be represented by ASS within the pixel error budget");
		// Keep the temporal planner's per-part fade tags, then compose all
		// absolute script geometry before replacing the glyph transform.
		auto const tags = TransformGeometryTags(part.text, matrix, solve.max_error / std::max(mapping.scale_x, mapping.scale_y));
		if (!tags.success)
			return Failure(tags.error);
		if (tags.has_animated_clip)
			return Failure("animated clips must be made static before applying motion tracking");
		std::string const positioned = ReplacePositionTag(tags.text,
														  "\\pos" + FormatAssPoint(source.state.position, 6));
		AssDialogue positioned_line(frozen);
		positioned_line.Text = positioned;
		auto const positioned_state = EvaluateEffectiveAssState({.file = &file, .line = &positioned_line, .play_resolution = source.play_resolution, .capture_time_ms = capture_time});
		if (!positioned_state)
			return Failure(DescribeAssStateError(positioned_state.error));
		auto const rewritten = RewritePerspectiveTags(positioned, positioned_state.value.transform,
													  evaluated.value.event_style_transform, *solved.candidate);
		if (!rewritten)
			return Failure(DescribeRewriteError(rewritten.error));
		AssDialogue staged(*line);
		staged.Start = static_cast<int>(agi::Time(part.start_ms));
		staged.End = static_cast<int>(agi::Time(part.end_ms));
		// The solver chooses precision for the pose channels independently.
		// Apply the user's position precision only to the final output, so it
		// cannot alter the reference geometry or be bypassed by a no-op rewrite.
		staged.Text = ReplacePositionTag(rewritten.text, "\\pos" +
															 FormatAssPoint(solved.candidate->state.position, input.options.position_decimals));
		auto const written_state = EvaluateEffectiveAssState({.file = &file, .line = &staged, .play_resolution = source.play_resolution, .capture_time_ms = input.timecodes.TimeAtFrame(frame)});
		if (!written_state)
			return Failure(DescribeAssStateError(written_state.error));
		auto const written_bounds = EvaluateAssBaseBounds({.line = &staged, .state = &written_state.value, .text_extents = input.text_extents, .layout_aspect = *layout_aspect});
		if (!written_bounds)
			return Failure(DescribeAssBoundsError(written_bounds.error));
		ForwardInput written = source;
		written.bounds = written_bounds.value;
		written.state = written_state.value.transform;
		auto const residual = MeasurePerspectiveResidual(written, target, mapping);
		if (!residual || residual.max_error > output_error)
			return Failure("written ASS geometry exceeds the pixel error budget; increase Position decimals or Compact error");
		part.text = staged.Text.get();
		part.x0 = part.x1 = written_state.value.transform.position.x;
		part.y0 = part.y1 = written_state.value.transform.position.y;
		frames.push_back({.time = input.timecodes.TimeAtFrame(frame), .held = held, .target = target, .state = written_state.value.transform, .text = part.text});
	}

	// Compact tests serialized ASS at every source frame. Projective tag
	// interpolation is nonlinear in screen space, so scalar-channel error
	// alone cannot certify a segment. Clip paths and origins have no general
	// ASS animation; those frames remain static and identical parts merge.
	if (input.options.mode == ApplyMode::Compact && !clip && !input.options.apply_fad) {
		struct Span {
			size_t first, last;
		};
		std::vector<Span> pending;
		for (size_t i = parts.size(); i > 0;) {
			size_t const last = --i;
			while (i > 0 && parts[last].covered && parts[i - 1].covered &&
				   frames[i - 1].held == frames[last].held)
				--i;
			pending.push_back({.first = i, .last = last});
		}
		std::vector<PlannedLinePart> compact;
		while (!pending.empty()) {
			auto const span = pending.back();
			pending.pop_back();
			auto part = parts[span.first];
			if (span.first == span.last || !part.covered) {
				compact.push_back(std::move(part));
				continue;
			}
			part.end_ms = parts[span.last].end_ms;
			bool origins_match = true;
			for (size_t i = span.first + 1; i <= span.last; ++i)
				origins_match = origins_match && SameOrigin(frames[span.first].state, frames[i].state);
			part.text = AnimatedText(frames[span.first], frames[span.last], static_cast<int>(agi::Time(part.start_ms)), input.options.position_decimals);
			AssDialogue candidate(*line);
			candidate.Start = static_cast<int>(agi::Time(part.start_ms));
			candidate.End = static_cast<int>(agi::Time(part.end_ms));
			candidate.Text = part.text;
			double worst_error = origins_match ? 0.0 : input.options.compact_epsilon + 1.0;
			size_t worst = (span.first + span.last) / 2;
			if (origins_match)
				for (size_t i = span.first; i <= span.last; ++i) {
					auto const state = EvaluateEffectiveAssState({.file = &file, .line = &candidate, .play_resolution = source.play_resolution, .capture_time_ms = frames[i].time});
					double error = input.options.compact_epsilon + 1.0;
					if (state) {
						ForwardInput check = source;
						check.state = state.value.transform;
						auto const candidate_bounds = EvaluateAssBaseBounds({.line = &candidate, .state = &state.value, .text_extents = input.text_extents, .layout_aspect = *layout_aspect});
						if (candidate_bounds) {
							check.bounds = candidate_bounds.value;
							auto const measured = MeasurePerspectiveResidual(check, frames[i].target, mapping);
							if (measured)
								error = measured.max_error;
						}
					}
					if (error > worst_error) {
						worst_error = error;
						worst = i;
					}
				}
			if (worst_error <= input.options.compact_epsilon) {
				part.x1 = frames[span.last].state.position.x;
				part.y1 = frames[span.last].state.position.y;
				compact.push_back(std::move(part));
			}
			else {
				worst = std::clamp(worst, span.first, span.last - 1);
				pending.push_back({.first = worst + 1, .last = span.last});
				pending.push_back({.first = span.first, .last = worst});
			}
		}
		parts = std::move(compact);
	}
	std::vector<PlannedLinePart> merged;
	for (auto& part : parts) {
		if (!merged.empty() && part.covered && merged.back().covered &&
			merged.back().end_ms == part.start_ms && merged.back().text == part.text &&
			part.text.find("\\t(") == std::string::npos && part.text.find("\\move(") == std::string::npos &&
			part.text.find("\\fad") == std::string::npos) {
			merged.back().end_ms = part.end_ms;
		}
		else
			merged.push_back(std::move(part));
	}
	parts = std::move(merged);
	result.needs_manual_review.clear();
	result.event_count = std::count_if(parts.begin(), parts.end(), [](auto const& part) { return part.covered; });
	result.status = result.event_count > 100 ? ApplyPlanStatus::NeedsConfirmation : ApplyPlanStatus::Ok;
	return result;
}

} // namespace aegisub::motion_track
