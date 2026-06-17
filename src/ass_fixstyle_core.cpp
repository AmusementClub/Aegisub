#include "ass_fixstyle_core.h"

#include "ass_dialogue.h"
#include "ass_file.h"

namespace aegisub::ass_fixstyle {

void ReplaceMissingStylesWithDefault(AssFile *subs) {
	for (auto& diag : subs->Events) {
		if (!subs->GetStyle(diag.Style))
			diag.Style = "Default";
	}
}

}

