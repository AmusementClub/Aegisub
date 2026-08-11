#include "align_video_fade.h"

#include "ass_compat.h"
#include "ass_dialogue.h"

#include <libaegisub/vfr.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace aegisub::align_video_fade {
namespace {

double Median(std::span<double const> values) {
	if (values.empty())
		return 0.0;

	std::array<double, 96> local{};
	std::vector<double> dynamic;
	std::span<double> sorted;
	if (values.size() <= local.size()) {
		std::copy(values.begin(), values.end(), local.begin());
		sorted = std::span<double>(local).first(values.size());
	}
	else {
		dynamic.assign(values.begin(), values.end());
		sorted = dynamic;
	}
	auto const middle = sorted.begin() + sorted.size() / 2;
	std::nth_element(sorted.begin(), middle, sorted.end());
	double result = *middle;
	if (sorted.size() % 2 == 0) {
		auto const lower = std::max_element(sorted.begin(), middle);
		result = (*lower + result) * 0.5;
	}
	return result;
}

double Huber(double residual, double delta) {
	double const magnitude = std::abs(residual);
	return magnitude <= delta
		? 0.5 * residual * residual
		: delta * (magnitude - 0.5 * delta);
}

double ModelLoss(
	std::span<double const> values,
	int start,
	int end,
	double gamma,
	double delta) {
	double loss = 0.0;
	for (int i = 0; i < static_cast<int>(values.size()); ++i) {
		double predicted = 0.0;
		if (i >= end)
			predicted = 1.0;
		else if (i > start) {
			double const position = static_cast<double>(i - start) / (end - start);
			predicted = std::pow(position, gamma);
		}
		loss += Huber(values[i] - predicted, delta);
	}
	return loss / values.size();
}

double HardCutLoss(std::span<double const> values, int split, double delta) {
	double loss = 0.0;
	for (int i = 0; i < static_cast<int>(values.size()); ++i)
		loss += Huber(values[i] - (i >= split ? 1.0 : 0.0), delta);
	return loss / values.size();
}

bool IsFadeTag(AssOverrideTag const& tag) {
	return tag.Name == "\\fad" || tag.Name == "\\fade";
}

bool IsOverallAlphaTag(AssOverrideTag const& tag) {
	return tag.Name == "\\alpha";
}

bool TransformContainsOverallAlpha(AssOverrideTag const& tag) {
	if (tag.Name != "\\t" || tag.Params.size() <= 3 || tag.Params[3].omitted || tag.Params[3].empty)
		return false;

	auto const* nested = tag.Params[3].Get<AssDialogueBlockOverride*>();
	for (auto const& nested_tag : nested->Tags) {
		if (IsOverallAlphaTag(nested_tag) || TransformContainsOverallAlpha(nested_tag))
			return true;
	}
	return false;
}

void GatherOverallAlphaValues(AssDialogueBlockOverride const& block, std::vector<int>& values) {
	for (auto const& tag : block.Tags) {
		if (IsOverallAlphaTag(tag) && !tag.Params.empty() && !tag.Params[0].omitted && !tag.Params[0].empty)
			values.push_back(tag.Params[0].Get<int>());
		if (tag.Name == "\\t" && tag.Params.size() > 3 && !tag.Params[3].omitted && !tag.Params[3].empty)
			GatherOverallAlphaValues(*tag.Params[3].Get<AssDialogueBlockOverride*>(), values);
	}
}

bool RemoveOverallAlphaFromTransform(AssOverrideTag& tag) {
	if (tag.Name != "\\t" || tag.Params.size() <= 3 || tag.Params[3].omitted || tag.Params[3].empty)
		return true;

	auto* nested = tag.Params[3].Get<AssDialogueBlockOverride*>();
	for (auto it = nested->Tags.begin(); it != nested->Tags.end();) {
		if (IsOverallAlphaTag(*it)) {
			it = nested->Tags.erase(it);
			continue;
		}
		if (it->Name == "\\t" && !RemoveOverallAlphaFromTransform(*it)) {
			it = nested->Tags.erase(it);
			continue;
		}
		++it;
	}
	return !nested->Tags.empty();
}

AssDialogueBlockOverride* InitialOverride(
	std::vector<std::unique_ptr<AssDialogueBlock>>& blocks) {
	if (!blocks.empty() && blocks.front()->GetType() == AssBlockType::OVERRIDE)
		return static_cast<AssDialogueBlockOverride*>(blocks.front().get());

	auto block = std::make_unique<AssDialogueBlockOverride>();
	auto* result = block.get();
	blocks.insert(blocks.begin(), std::move(block));
	return result;
}

std::string AlphaTag(int alpha) {
	return "\\alpha" + AssCompat::FormatOverrideAlpha(alpha);
}

std::string TransformAlphaTag(int start, int end, int alpha) {
	return "\\t(" + std::to_string(start) + "," + std::to_string(end)
		+ "," + AlphaTag(alpha) + ")";
}

} // namespace

CurveFit FitVisibilityCurve(std::span<double const> samples, int plateau_samples) {
	CurveFit result;
	int const count = static_cast<int>(samples.size());
	if (plateau_samples < 2 || count < plateau_samples + 3)
		return result;

	int const outside_count = count - plateau_samples;
	int const baseline_count = std::min(3, outside_count);
	std::vector<double> outside_samples(
		samples.begin(), samples.begin() + outside_count);
	double off = *std::min_element(outside_samples.begin(), outside_samples.end());
	// When the scan reaches a real transparent plateau, use a small median to
	// suppress isolated readback noise. For short fades the first samples can
	// already be on the ramp, so retain the lower envelope instead.
	if (outside_count >= 5) {
		std::vector<double> leading(outside_samples.begin(), outside_samples.begin() + 5);
		std::sort(leading.begin(), leading.end());
		if (leading.back() - leading.front() <= 0.12)
			off = Median(std::span<double const>(leading));
	}
	double const on = Median(samples.last(plateau_samples));
	double const scale = on - off;
	if (scale <= 0.08 || off > 0.35)
		return result;

	std::vector<double> normalized;
	normalized.reserve(samples.size());
	for (double value : samples)
		normalized.push_back(std::clamp((value - off) / scale, -0.5, 1.5));

	std::vector<double> baseline_deviation;
	baseline_deviation.reserve(baseline_count);
	for (int i = 0; i < baseline_count; ++i)
		baseline_deviation.push_back(std::abs(normalized[i]));
	double const noise = 1.4826 * Median(baseline_deviation);
	if (noise > 0.50)
		return result;

	double const delta = std::clamp(3.0 * noise, 0.10, 0.25);
	double hard_loss = 1e30;
	for (int split = 1; split <= outside_count; ++split)
		hard_loss = std::min(hard_loss, HardCutLoss(normalized, split, delta));

	constexpr double gammas[] = { 0.5, 0.75, 1.0, 1.4, 2.0 };
	double best_loss = 1e30;
	int best_start = -1;
	int best_end = -1;
	for (int end = 2; end <= outside_count; ++end) {
		for (int start = 0; start + 2 <= end; ++start) {
			for (double gamma : gammas) {
				double const loss = ModelLoss(normalized, start, end, gamma, delta)
					+ 0.0005 * (end - start);
				if (loss < best_loss) {
					best_loss = loss;
					best_start = start;
					best_end = end;
				}
			}
		}
	}

	double const improvement = hard_loss - best_loss;
	double const relative_improvement = improvement / std::max(hard_loss, 1e-9);
	if (best_start < 0 || best_end - best_start < 2
		|| improvement < std::max(0.004, noise * noise * 0.2)
		|| relative_improvement < 0.20)
		return result;

	// The fitted zero point is deliberately tolerant of compression noise and
	// can sit inside the transparent lead-in. Report the first persistently
	// visible sample instead, which is the frame the aligned line should begin
	// on. The final plateau is supplied by the caller only after it has been
	// explicitly confirmed, so its first sample is the fade completion point.
	double const visible_threshold = std::clamp(3.0 * noise, 0.01, 0.10);
	int first_visible = -1;
	for (int i = 0; i < outside_count; ++i) {
		if (normalized[i] < visible_threshold)
			continue;
		bool const persists = (i + 1 < count && normalized[i + 1] >= visible_threshold)
			|| (i + 2 < count && normalized[i + 2] >= visible_threshold);
		if (persists) {
			first_visible = i;
			break;
		}
	}
	if (first_visible < 0 || outside_count - first_visible < 2)
		return result;

	result.detected = true;
	result.outer_index = first_visible;
	result.inner_index = outside_count;
	result.confidence = std::clamp(
		relative_improvement * (1.0 - std::min(noise, 1.0)),
		0.0,
		1.0);
	return result;
}

PlateauReference BuildPlateauReference(std::span<double const> platform_samples) {
	PlateauReference result;
	if (platform_samples.size() < 2)
		return result;
	double const platform_level = Median(platform_samples);
	if (platform_level <= 0.08)
		return result;

	std::vector<double> deviations;
	deviations.reserve(platform_samples.size());
	for (double sample : platform_samples)
		deviations.push_back(std::abs(sample - platform_level));
	double const robust_noise = 1.4826 * Median(deviations);
	auto const [platform_minimum, platform_maximum] = std::minmax_element(
		platform_samples.begin(),
		platform_samples.end());
	double const platform_spread = *platform_maximum - *platform_minimum;
	result.valid = true;
	result.level = platform_level;
	result.level_band = std::max(
		std::abs(platform_level) * 0.005,
		3.0 * robust_noise);
	result.spread_limit = std::max(
		std::abs(platform_level) * 0.01,
		platform_spread * 1.5);
	return result;
}

int FindConfirmedPlateauStart(
	std::span<double const> samples,
	PlateauReference const& platform,
	int confirmation_samples) {
	if (!platform.valid
		|| confirmation_samples < 2
		|| static_cast<int>(samples.size()) < confirmation_samples)
		return -1;

	for (int start = 0;
		start + confirmation_samples <= static_cast<int>(samples.size());
		++start) {
		auto const window = samples.subspan(start, confirmation_samples);
		auto const [minimum, maximum] = std::minmax_element(window.begin(), window.end());
		double const level = Median(window);
		if (*minimum >= platform.level - platform.level_band
			&& level <= platform.level + platform.level_band
			&& *maximum - *minimum <= platform.spread_limit)
			return start;
	}
	return -1;
}

int FindConfirmedPlateauStart(
	std::span<double const> samples,
	std::span<double const> platform_samples,
	int confirmation_samples) {
	if (static_cast<int>(platform_samples.size()) < confirmation_samples)
		return -1;
	return FindConfirmedPlateauStart(
		samples,
		BuildPlateauReference(platform_samples),
		confirmation_samples);
}

AssFadeTiming BuildAssFadeTiming(
	agi::vfr::Framerate const& timecodes,
	int first_visible_frame,
	int last_visible_frame,
	int fade_in_full_frame,
	int fade_out_full_frame,
	bool fade_in_detected,
	bool fade_out_detected) {
	AssFadeTiming result;
	result.start_ms = timecodes.TimeAtFrame(first_visible_frame, agi::vfr::START);
	result.end_ms = timecodes.TimeAtFrame(last_visible_frame, agi::vfr::END);
	int const duration_ms = std::max(result.end_ms - result.start_ms, 0);
	if (fade_in_detected) {
		int const full_sample_ms = timecodes.TimeAtFrame(fade_in_full_frame, agi::vfr::EXACT);
		result.fade_in_ms = std::clamp(full_sample_ms - result.start_ms, 0, duration_ms);
	}
	if (fade_out_detected) {
		int const full_sample_ms = timecodes.TimeAtFrame(fade_out_full_frame, agi::vfr::EXACT);
		result.fade_out_ms = std::clamp(
			result.end_ms - full_sample_ms,
			0,
			duration_ms - result.fade_in_ms);
	}
	return result;
}

AssFadeUpdate ApplyAssFade(
	std::string const& text,
	int duration_ms,
	int fade_in_ms,
	int fade_out_ms) {
	duration_ms = std::max(duration_ms, 0);
	fade_in_ms = std::clamp(fade_in_ms, 0, duration_ms);
	fade_out_ms = std::clamp(fade_out_ms, 0, duration_ms - fade_in_ms);

	AssDialogue line;
	line.Text = text;
	auto blocks = line.ParseTags();

	bool has_fade = false;
	bool has_fad = false;
	bool alpha_animation_in_initial_block = false;
	bool alpha_animation_elsewhere = false;
	for (size_t block_index = 0; block_index < blocks.size(); ++block_index) {
		if (blocks[block_index]->GetType() != AssBlockType::OVERRIDE)
			continue;
		auto const* block = static_cast<AssDialogueBlockOverride*>(blocks[block_index].get());
		for (auto const& tag : block->Tags) {
			has_fade = has_fade || tag.Name == "\\fade";
			has_fad = has_fad || tag.Name == "\\fad";
			if (TransformContainsOverallAlpha(tag)) {
				if (block_index == 0)
					alpha_animation_in_initial_block = true;
				else
					alpha_animation_elsewhere = true;
			}
		}
	}

	AssFadeEncoding encoding = has_fade
		? AssFadeEncoding::Fade
		: has_fad
			? AssFadeEncoding::Fad
			: alpha_animation_in_initial_block && !alpha_animation_elsewhere
				? AssFadeEncoding::AlphaTransform
				: AssFadeEncoding::Fad;

	AssDialogueBlockOverride* alpha_animation_block = nullptr;
	std::vector<int> alpha_values;
	if (encoding == AssFadeEncoding::AlphaTransform
		&& !blocks.empty()
		&& blocks.front()->GetType() == AssBlockType::OVERRIDE) {
		alpha_animation_block = static_cast<AssDialogueBlockOverride*>(blocks.front().get());
		GatherOverallAlphaValues(*alpha_animation_block, alpha_values);
	}

	for (auto& block : blocks) {
		if (block->GetType() != AssBlockType::OVERRIDE)
			continue;
		auto* override_block = static_cast<AssDialogueBlockOverride*>(block.get());
		bool const normalize_alpha_animation = override_block == alpha_animation_block;

		for (auto it = override_block->Tags.begin(); it != override_block->Tags.end();) {
			if (IsFadeTag(*it)
				|| (normalize_alpha_animation && IsOverallAlphaTag(*it))) {
				it = override_block->Tags.erase(it);
				continue;
			}
			if (normalize_alpha_animation
				&& it->Name == "\\t"
				&& !RemoveOverallAlphaFromTransform(*it)) {
				it = override_block->Tags.erase(it);
				continue;
			}
			++it;
		}
	}

	blocks.erase(
		std::remove_if(blocks.begin(), blocks.end(), [](auto const& block) {
			return block->GetType() == AssBlockType::OVERRIDE
				&& static_cast<AssDialogueBlockOverride const*>(block.get())->Tags.empty();
		}),
		blocks.end());

	auto* initial = InitialOverride(blocks);
	std::vector<AssOverrideTag> inserted;
	if (encoding == AssFadeEncoding::Fade) {
		inserted.emplace_back(
			"\\fade(255,0,255,0," + std::to_string(fade_in_ms) + ","
			+ std::to_string(duration_ms - fade_out_ms) + ","
			+ std::to_string(duration_ms) + ")");
	}
	else if (encoding == AssFadeEncoding::AlphaTransform) {
		int const middle_alpha = alpha_values.empty()
			? 0
			: *std::min_element(alpha_values.begin(), alpha_values.end());
		inserted.emplace_back(AlphaTag(fade_in_ms > 0 ? 255 : middle_alpha));
		if (fade_in_ms > 0)
			inserted.emplace_back(TransformAlphaTag(0, fade_in_ms, middle_alpha));
		if (fade_out_ms > 0)
			inserted.emplace_back(TransformAlphaTag(
				duration_ms - fade_out_ms,
				duration_ms,
				255));
	}
	else {
		inserted.emplace_back(
			"\\fad(" + std::to_string(fade_in_ms) + ","
			+ std::to_string(fade_out_ms) + ")");
	}

	initial->Tags.insert(
		initial->Tags.begin(),
		std::make_move_iterator(inserted.begin()),
		std::make_move_iterator(inserted.end()));
	line.UpdateText(blocks);
	return { line.Text.get(), encoding };
}

} // namespace aegisub::align_video_fade
