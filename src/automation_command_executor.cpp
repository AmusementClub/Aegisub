#include "automation_command_executor.h"

#include "command/command.h"
#include "compat.h"
#include "include/aegisub/context.h"
#include "status_sink.h"

#include <exception>

namespace aegisub::automation_command_executor {

json::Object Invoke(std::string const& command_id, agi::Context& context, bool allow_gui_only) {
	json::Object result;
	result["command"] = command_id;
	result["invoked"] = false;
	result["found"] = false;
	result["validated"] = false;
	result["allowed"] = false;

	try {
		auto *command = cmd::get_if(command_id);
		if (!command) {
			result["error"] = "command was not found";
			return result;
		}
		result["found"] = true;
		result["scope"] = command->AutomationScope() == cmd::CommandExecutionScope::HeadlessSafe
			? "headless_safe"
			: "gui_only";
		if (!allow_gui_only && command->AutomationScope() != cmd::CommandExecutionScope::HeadlessSafe) {
			result["error"] = "command is not allowed in headless mode";
			return result;
		}
		result["allowed"] = true;

		bool const tracks_active = (command->Type() & (cmd::COMMAND_TOGGLE | cmd::COMMAND_RADIO)) != 0;
		if (tracks_active)
			result["active_before"] = command->IsActive(&context);
		bool const valid = command->Validate(&context);
		result["validated"] = valid;
		if (!valid) {
			result["error"] = "command validation rejected the invocation";
			return result;
		}
		if (auto sink = context.GetStatusSink())
			sink->SetLastCommand(from_wx(command->StrDisplay(&context)));
		(*command)(&context);
		result["invoked"] = true;
		if (tracks_active)
			result["active_after"] = command->IsActive(&context);
	}
	catch (std::exception const& e) {
		result["error"] = e.what();
	}
	catch (...) {
		result["error"] = "unknown command execution failure";
	}
	return result;
}

}
