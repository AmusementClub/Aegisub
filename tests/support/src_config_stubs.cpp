#include "../../src/options.h"

namespace Automation4 { class AutoloadScriptManager; }

namespace config {
	agi::Options *opt = nullptr;
	agi::MRUManager *mru = nullptr;
	agi::Path *path = nullptr;
	Automation4::AutoloadScriptManager *global_scripts = nullptr;
}
