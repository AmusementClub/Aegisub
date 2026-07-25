#include "font_family_selection_model.h"

#include <algorithm>
#include <unordered_set>
#include <utility>

bool FontFamilySelectionModel::SupportsVerticalWriting(
	FontFamilyId family_id,
	std::string_view face_name) const {
	if (family_id != 0 && vertical_capable_family_ids.contains(family_id))
		return true;
	auto const bare = FontFamilyCatalog::SplitVerticalPrefix(face_name).second;
	if (bare.empty())
		return false;
	if (vertical_capable_bare_names.contains(std::string(bare)))
		return true;
	// Catalog path may only have filled ids; try resolve bare once.
	if (catalog && !catalog->empty()) {
		auto const resolved = catalog->Resolve(bare);
		if (resolved.family &&
		    vertical_capable_family_ids.contains(*resolved.family))
			return true;
	}
	return false;
}

std::string FontFamilySelectionModel::PreferredName(std::string_view stored_name) const {
	if (!catalog || catalog->empty())
		return std::string(stored_name);
	return catalog->MapToPreferredWriteName(stored_name, prefer_localized);
}

FontFamilyRecord const* FontFamilySelectionModel::ResolveRecord(std::string_view name) const {
	if (!catalog || catalog->empty())
		return nullptr;
	auto const resolved = catalog->Resolve(name);
	return resolved.family ? catalog->Find(*resolved.family) : nullptr;
}

FontFamilyRecord const* FontFamilySelectionModel::ResolveChoice(FontFamilyId family_id) const {
	return family_id != 0 && catalog ? catalog->Find(family_id) : nullptr;
}

FontFamilySelectionModel BuildFontFamilySelectionModel(
	std::shared_ptr<FontFamilyCatalog const> catalog,
	bool prefer_localized,
	std::vector<std::string> fallback_names)
{
	FontFamilySelectionModel model;
	model.prefer_localized = prefer_localized;
	model.catalog = std::move(catalog);

	if (!model.catalog || model.catalog->empty()) {
		model.choices.reserve(fallback_names.size());
		for (auto& name : fallback_names)
			model.choices.push_back({std::move(name), 0});
		return model;
	}

	model.choices.reserve(model.catalog->size());
	for (auto const& record : model.catalog->records()) {
		model.choices.push_back({
			model.catalog->PreferredWriteName(record, model.prefer_localized),
			record.id});
	}
	std::sort(model.choices.begin(), model.choices.end(), [](auto const& left, auto const& right) {
		return left.label != right.label
			? left.label < right.label
			: left.family_id < right.family_id;
	});
	return model;
}

void FillVerticalCapability(
	FontFamilySelectionModel& model,
	std::vector<std::string> const& vertical_faces) {
	for (auto const& name : vertical_faces) {
		if (name.empty() || name.front() != '@')
			continue;
		auto bare = name.substr(1);
		if (bare.empty())
			continue;
		model.vertical_capable_bare_names.insert(bare);
		if (!model.catalog || model.catalog->empty())
			continue;
		auto const resolved = model.catalog->Resolve(name);
		if (resolved.family)
			model.vertical_capable_family_ids.insert(*resolved.family);
	}
}

void AppendVerticalFacenameChoices(
	FontFamilySelectionModel& model,
	std::vector<std::string> const& vertical_faces) {
	if (!model.catalog || model.catalog->empty() || vertical_faces.empty())
		return;

	std::unordered_set<std::string> seen;
	seen.reserve(model.choices.size() * 2);
	for (auto const& choice : model.choices)
		seen.insert(choice.label);

	for (auto const& name : vertical_faces) {
		if (name.empty() || name.front() != '@')
			continue;
		auto const resolved = model.catalog->Resolve(name);
		if (resolved.match != FontFamilyMatchKind::Exact &&
		    resolved.match != FontFamilyMatchKind::CaseInsensitiveExact)
			continue;
		if (!resolved.family)
			continue;
		auto const* record = model.catalog->Find(*resolved.family);
		if (!record)
			continue;
		auto const preferred = model.catalog->PreferredWriteName(
			*record, model.prefer_localized);
		if (preferred.empty())
			continue;
		auto label = FontFamilyCatalog::JoinVerticalPrefix(true, preferred);
		if (label.size() <= 1 || !seen.insert(label).second)
			continue;
		model.choices.push_back({std::move(label), *resolved.family});
	}

	std::sort(model.choices.begin(), model.choices.end(),
		[](FontFamilyChoice const& left, FontFamilyChoice const& right) {
			return left.label != right.label
				? left.label < right.label
				: left.family_id < right.family_id;
		});
}

bool UpdateVerticalWritingIntent(
	bool current_intent,
	std::string_view face_name,
	bool committing_list_selection) noexcept {
	if (committing_list_selection)
		return current_intent;
	return !FontFamilyCatalog::SplitVerticalPrefix(face_name).first.empty();
}
