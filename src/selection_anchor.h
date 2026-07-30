#pragma once

#include <functional>
#include <optional>

namespace aegisub::selection_anchor {

struct Snapshot {
	int line_id = 0;
	int row = -1;
	bool available = false;
};

class Anchor {
	struct Target {
		int line_id;
		int last_known_row;
	};

	std::optional<Target> target;

public:
	using ResolveRow = std::function<std::optional<int>(int)>;

	void Set(int line_id, int row);
	void Clear();
	bool IsSet() const { return target.has_value(); }

	std::optional<Snapshot> Peek(ResolveRow const& resolve_row) const;
	std::optional<Snapshot> Refresh(ResolveRow const& resolve_row);
};

}
