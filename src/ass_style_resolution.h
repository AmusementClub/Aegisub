// Copyright (c) 2026

#pragma once

#include <string>
#include <vector>

class AssDialogue;
class AssFile;
class AssStyle;

namespace aegisub::ass_style_resolution {

enum class StyleUsageKind {
	EventStyle,
	ResetTag
};

struct StyleRecord {
	AssStyle *style = nullptr;
	int index = -1;
	std::string raw_name;
	std::string definition_key;
	std::string canonical_name;
	int group_winner = -1;
	bool has_compatibility_prefix = false;
	bool has_default_case_confusion = false;
	bool shadowed = false;
	bool same_as_winner = false;
};

struct StyleUsage {
	StyleUsageKind kind = StyleUsageKind::EventStyle;
	AssDialogue *line = nullptr;
	std::string raw_name;
	int resolved_style = -1;
	int line_number = -1;
	bool resolved = false;
	bool strict_lookup = false;
};

struct StyleResolutionGraph {
	std::vector<StyleRecord> styles;
	std::vector<StyleUsage> usages;
};

struct StyleRename {
	AssStyle *style = nullptr;
	std::string new_name;
};

struct EventStyleUpdate {
	AssDialogue *line = nullptr;
	std::string old_name;
	std::string new_name;
	std::string resolved_style_name;
	int line_number = -1;
};

struct StyleCleanPlan {
	std::vector<StyleRename> renames;
	std::vector<AssStyle *> deletions;
	std::vector<EventStyleUpdate> event_updates;
	std::vector<std::string> notes;

	bool empty() const {
		return renames.empty() && deletions.empty() && event_updates.empty();
	}
};

AssStyle *ResolveEventStyle(AssFile& file, std::string const& name);
AssStyle const *ResolveEventStyle(AssFile const& file, std::string const& name);
AssStyle *ResolveResetStyle(AssFile& file, std::string const& name);
AssStyle const *ResolveResetStyle(AssFile const& file, std::string const& name);

StyleResolutionGraph BuildStyleResolutionGraph(AssFile& file);
StyleCleanPlan BuildStyleCleanPlan(StyleResolutionGraph const& graph);
void ApplyStyleCleanPlan(StyleCleanPlan const& plan);

std::string BuildStyleIssueTooltip(StyleResolutionGraph const& graph, int style_index);
std::string BuildStyleCleanPreview(StyleCleanPlan const& plan);

}
