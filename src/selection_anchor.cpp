#include "selection_anchor.h"

namespace aegisub::selection_anchor {

void Anchor::Set(int line_id, int row) {
	if (line_id <= 0) {
		Clear();
		return;
	}

	target = Target{line_id, row};
}

void Anchor::Clear() {
	target.reset();
}

std::optional<Snapshot> Anchor::Peek(ResolveRow const& resolve_row) const {
	if (!target)
		return std::nullopt;

	auto row = resolve_row(target->line_id);
	return Snapshot{target->line_id, row.value_or(target->last_known_row), row.has_value()};
}

std::optional<Snapshot> Anchor::Refresh(ResolveRow const& resolve_row) {
	auto snapshot = Peek(resolve_row);
	if (snapshot && snapshot->available)
		target->last_known_row = snapshot->row;
	return snapshot;
}

}
