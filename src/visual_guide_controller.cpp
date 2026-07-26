#include "visual_guide_controller.h"

#include "ui_dispatch.h"

#include <algorithm>
#include <utility>

namespace {
auto FindGuide(std::vector<VisualGuide>& guides, std::string const& id) {
	return std::find_if(guides.begin(), guides.end(), [&id](VisualGuide const& guide) {
		return guide.id == id;
	});
}

auto FindGuide(std::vector<VisualGuide> const& guides, std::string const& id) {
	return std::find_if(guides.begin(), guides.end(), [&id](VisualGuide const& guide) {
		return guide.id == id;
	});
}
}

std::optional<std::string> VisualGuideController::Add(VisualGuide guide) {
	agi::ui::VerifyAccess();
	if (guides.size() >= MaximumGuideCount || !IsValidVisualGuideData(guide))
		return std::nullopt;

	guide.id = "guide-" + std::to_string(next_id++);
	auto id = guide.id;
	guides.push_back(std::move(guide));
	NotifyChanged();
	return id;
}

bool VisualGuideController::Update(std::string const& id, VisualGuide guide) {
	agi::ui::VerifyAccess();
	if (id.empty() || !IsValidVisualGuideData(guide))
		return false;

	auto existing = FindGuide(guides, id);
	if (existing == guides.end())
		return false;

	guide.id = id;
	if (*existing == guide)
		return false;

	*existing = std::move(guide);
	NotifyChanged();
	return true;
}

bool VisualGuideController::Remove(std::string const& id) {
	agi::ui::VerifyAccess();
	auto existing = FindGuide(guides, id);
	if (existing == guides.end())
		return false;

	guides.erase(existing);
	if (selected_id && *selected_id == id)
		selected_id.reset();
	NotifyChanged();
	return true;
}

bool VisualGuideController::Select(std::string const& id) {
	return Select(std::optional<std::string>(id));
}

bool VisualGuideController::Select(std::optional<std::string> id) {
	agi::ui::VerifyAccess();
	if (id && FindGuide(guides, *id) == guides.end())
		return false;
	if (selected_id == id)
		return false;

	selected_id = std::move(id);
	NotifyChanged();
	return true;
}

void VisualGuideController::Clear() {
	agi::ui::VerifyAccess();
	if (guides.empty() && !selected_id)
		return;

	guides.clear();
	selected_id.reset();
	NotifyChanged();
}

void VisualGuideController::ResetForVideoChange() {
	Clear();
}

VisualGuideSnapshot VisualGuideController::CaptureSnapshot() const {
	agi::ui::VerifyAccess();
	VisualGuideSnapshot snapshot{ generation, guides, selected_id, std::nullopt };
	if (!guides.empty())
		snapshot.last_measurement_id = guides.back().id;
	return snapshot;
}

VisualGuideSnapshotView VisualGuideController::CaptureView() const noexcept {
	agi::ui::VerifyAccess();
	return { generation, guides, selected_id };
}

void VisualGuideController::NotifyChanged() {
	++generation;
	if (generation == 0)
		++generation;
	changed();
}
