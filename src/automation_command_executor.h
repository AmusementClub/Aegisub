#pragma once

#include <libaegisub/cajun/elements.h>

#include <string>

namespace agi { struct Context; }

namespace aegisub::automation_command_executor {

json::Object Invoke(
	std::string const& command_id,
	agi::Context& context,
	bool allow_gui_only);

}
