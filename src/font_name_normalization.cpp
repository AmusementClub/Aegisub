#include "font_name_normalization.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "ass_style.h"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace {

std::string AsciiLower(std::string_view value) {
	std::string result(value);
	std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
		return static_cast<char>(std::tolower(ch));
	});
	return result;
}

std::string PreferredName(
	FontFamilyRecord const& record,
	std::string_view current_name,
	FontNameNormalizationTarget target)
{
	bool const vertical = !FontFamilyCatalog::SplitVerticalPrefix(current_name).first.empty();
	auto const& preferred = target == FontNameNormalizationTarget::Localized
		? record.localized_family_name
		: record.english_win32_family_name;
	auto bare = FontFamilyCatalog::SplitVerticalPrefix(preferred).second;
	return FontFamilyCatalog::JoinVerticalPrefix(vertical, bare);
}

void AnalyzeName(
	FontNameNormalizationPlan& plan,
	FontFamilyCatalog const& catalog,
	FontNameSourceLocation source,
	std::string const& current_name)
{
	if (current_name.empty())
		return;
	++plan.scanned_name_count;

	FontNameNormalizationChange change;
	change.source = std::move(source);
	change.current_name = current_name;

	if (catalog.empty()) {
		change.reason_code = "font_family_catalog_unavailable";
		plan.changes.push_back(std::move(change));
		return;
	}

	auto const resolved = catalog.Resolve(current_name);
	change.match_kind = resolved.match;
	if (resolved.match == FontFamilyMatchKind::Ambiguous) {
		change.reason_code = "ambiguous_family_alias";
		plan.changes.push_back(std::move(change));
		return;
	}
	if ((resolved.match != FontFamilyMatchKind::Exact &&
	     resolved.match != FontFamilyMatchKind::CaseInsensitiveExact) ||
	    !resolved.family) {
		change.reason_code = "unrecognized_family_name";
		plan.changes.push_back(std::move(change));
		return;
	}

	auto const *record = catalog.Find(*resolved.family);
	if (!record) {
		change.reason_code = "unrecognized_family_name";
		plan.changes.push_back(std::move(change));
		return;
	}

	if (plan.target == FontNameNormalizationTarget::EnglishWin32 &&
	    record->english_win32_family_name.empty()) {
		change.reason_code = "english_win32_name_unavailable";
		plan.changes.push_back(std::move(change));
		return;
	}

	change.recommended_name = PreferredName(*record, current_name, plan.target);
	if (change.recommended_name.empty()) {
		change.reason_code = "preferred_family_name_unavailable";
		plan.changes.push_back(std::move(change));
		return;
	}

	constexpr std::size_t kGdiFaceNameMaxUnits = 31;
	if (FontFamilyCatalog::Utf16CodeUnitLength(change.recommended_name) > kGdiFaceNameMaxUnits) {
		change.reason_code = "gdi_family_name_too_long";
		plan.changes.push_back(std::move(change));
		return;
	}

	if (change.current_name == change.recommended_name)
		return;

	change.safe_to_apply = true;
	if (AsciiLower(change.current_name) == AsciiLower(change.recommended_name))
		change.reason_code = "noncanonical_family_name_case";
	else if (plan.target == FontNameNormalizationTarget::EnglishWin32 &&
	         AsciiLower(change.current_name) == AsciiLower(
			 PreferredName(*record, current_name, FontNameNormalizationTarget::Localized)))
		change.reason_code = "localized_win32_family_alias";
	else if (plan.target == FontNameNormalizationTarget::Localized &&
	         AsciiLower(change.current_name) == AsciiLower(
			 PreferredName(*record, current_name, FontNameNormalizationTarget::EnglishWin32)))
		change.reason_code = "english_win32_family_alias";
	else
		change.reason_code = "win32_family_alias";
	plan.changes.push_back(std::move(change));
}

void AnalyzeOverrideBlock(
	FontNameNormalizationPlan& plan,
	FontFamilyCatalog const& catalog,
	AssDialogue const& event,
	std::size_t event_index,
	AssDialogueBlockOverride const& block,
	std::size_t& override_index)
{
	for (auto const& tag : block.Tags) {
		if (tag.Name == "\\fn") {
			auto const current_override_index = override_index++;
			if (!tag.Params.empty()) {
				auto const& parameter = tag.Params[0];
				if (!parameter.omitted && !parameter.empty) {
					auto const current_name = parameter.Get<std::string>();
					if (!current_name.empty()) {
						FontNameSourceLocation source;
						source.kind = FontNameSourceKind::Override;
						source.style = event.Style;
						source.line = event.Row >= 0 ? event.Row + 1 : static_cast<int>(event_index + 1);
						source.entry_index = event_index;
						source.override_index = current_override_index;
						source.comment = event.Comment;
						AnalyzeName(plan, catalog, std::move(source), current_name);
					}
				}
			}
		}

		for (auto const& parameter : tag.Params) {
			if (parameter.omitted || parameter.empty ||
			    parameter.GetType() != VariableDataType::BLOCK)
				continue;
			auto const *nested = parameter.Get<AssDialogueBlockOverride *>();
			if (nested)
				AnalyzeOverrideBlock(plan, catalog, event, event_index, *nested, override_index);
		}
	}
}

AssStyle *StyleAt(AssFile& file, std::size_t index) {
	if (index >= file.Styles.size())
		return nullptr;
	auto it = file.Styles.begin();
	std::advance(it, static_cast<std::ptrdiff_t>(index));
	return &*it;
}

AssDialogue *EventAt(AssFile& file, std::size_t index) {
	if (index >= file.Events.size())
		return nullptr;
	auto it = file.Events.begin();
	std::advance(it, static_cast<std::ptrdiff_t>(index));
	return &*it;
}

AssOverrideParameter *FindOverrideFontParameter(
	AssDialogueBlockOverride& block,
	std::size_t target_index,
	std::size_t& current_index)
{
	for (auto& tag : block.Tags) {
		if (tag.Name == "\\fn") {
			auto const this_index = current_index++;
			if (this_index == target_index)
				return tag.Params.empty() ? nullptr : &tag.Params[0];
		}

		for (auto& parameter : tag.Params) {
			if (parameter.omitted || parameter.empty ||
			    parameter.GetType() != VariableDataType::BLOCK)
				continue;
			auto *nested = parameter.Get<AssDialogueBlockOverride *>();
			if (!nested)
				continue;
			if (auto *found = FindOverrideFontParameter(*nested, target_index, current_index))
				return found;
		}
	}
	return nullptr;
}

AssOverrideParameter *FindOverrideFontParameter(
	std::vector<std::unique_ptr<AssDialogueBlock>>& blocks,
	std::size_t target_index)
{
	std::size_t current_index = 0;
	for (auto& block : blocks) {
		if (block->GetType() != AssBlockType::OVERRIDE)
			continue;
		if (auto *found = FindOverrideFontParameter(
			static_cast<AssDialogueBlockOverride&>(*block), target_index, current_index))
			return found;
	}
	return nullptr;
}

} // namespace

FontNameNormalizationPlan BuildFontNameNormalizationPlan(
	AssFile const& file,
	FontFamilyCatalog const& catalog,
	FontNameNormalizationTarget target,
	std::span<int const> style_source_lines)
{
	FontNameNormalizationPlan plan;
	plan.target = target;
	plan.catalog_available = !catalog.empty();

	std::size_t style_index = 0;
	for (auto const& style : file.Styles) {
		FontNameSourceLocation source;
		source.kind = FontNameSourceKind::Style;
		source.style = style.name;
		source.entry_index = style_index;
		if (style_index < style_source_lines.size())
			source.line = style_source_lines[style_index];
		AnalyzeName(plan, catalog, std::move(source), style.font);
		++style_index;
	}

	std::size_t event_index = 0;
	for (auto const& event : file.Events) {
		std::size_t override_index = 0;
		for (auto const& block : event.ParseTags()) {
			if (block->GetType() != AssBlockType::OVERRIDE)
				continue;
			AnalyzeOverrideBlock(
				plan,
				catalog,
				event,
				event_index,
				static_cast<AssDialogueBlockOverride const&>(*block),
				override_index);
		}
		++event_index;
	}

	return plan;
}

FontNameNormalizationApplyResult ApplyFontNameNormalizationChanges(
	AssFile& file,
	FontNameNormalizationPlan const& plan,
	std::span<std::size_t const> selected_change_indices)
{
	FontNameNormalizationApplyResult result;
	std::unordered_set<std::size_t> seen;
	struct PendingEventChange {
		AssDialogue *event = nullptr;
		std::vector<std::unique_ptr<AssDialogueBlock>> blocks;
	};
	std::unordered_map<std::size_t, PendingEventChange> pending_events;
	seen.reserve(selected_change_indices.size());
	pending_events.reserve(selected_change_indices.size());

	for (auto index : selected_change_indices) {
		if (index >= plan.changes.size()) {
			result.error = "normalization selection is out of range";
			return result;
		}
		if (!seen.insert(index).second) {
			result.error = "normalization selection contains a duplicate change";
			return result;
		}

		auto const& change = plan.changes[index];
		if (!change.safe_to_apply || change.recommended_name.empty()) {
			result.error = "normalization selection contains an unsafe change";
			return result;
		}

		if (change.source.kind == FontNameSourceKind::Style) {
			auto *style = StyleAt(file, change.source.entry_index);
			if (!style || style->name != change.source.style || style->font != change.current_name) {
				result.error = "style font changed after the normalization plan was built";
				return result;
			}
		}
		else {
			auto *event = EventAt(file, change.source.entry_index);
			if (!event) {
				result.error = "dialogue font override changed after the normalization plan was built";
				return result;
			}

			auto [pending_it, inserted] = pending_events.try_emplace(change.source.entry_index);
			auto& pending = pending_it->second;
			if (inserted) {
				pending.event = event;
				pending.blocks = event->ParseTags();
			}

			auto *parameter = FindOverrideFontParameter(
				pending.blocks, change.source.override_index);
			if (!parameter || parameter->omitted || parameter->empty ||
			    parameter->Get<std::string>() != change.current_name) {
				result.error = "dialogue font override changed after the normalization plan was built";
				return result;
			}
			parameter->Set<std::string>(change.recommended_name);
		}
	}

	for (auto index : selected_change_indices) {
		auto const& change = plan.changes[index];
		if (change.source.kind == FontNameSourceKind::Style) {
			auto *style = StyleAt(file, change.source.entry_index);
			style->font = change.recommended_name;
			style->UpdateData();
			result.styles_changed = true;
		}
		++result.applied_change_count;
	}
	for (auto& [index, pending] : pending_events) {
		(void)index;
		pending.event->UpdateText(pending.blocks);
		result.dialogue_text_changed = true;
	}

	result.success = true;
	return result;
}
