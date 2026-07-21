#include "font_family_selection_model.h"

#include <algorithm>
#include <utility>

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
