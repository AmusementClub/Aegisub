#pragma once

#include <utility>

namespace aegisub { namespace subtitle_edit_box_input {
	struct MarginInput {
		int value;
		bool normalize_control;
	};

	template<typename ValueReader>
	MarginInput ResolveMarginInput(bool is_blank, ValueReader&& read_value) {
		if (is_blank)
			return {0, true};
		return {static_cast<int>(std::forward<ValueReader>(read_value)()), false};
	}
} }
