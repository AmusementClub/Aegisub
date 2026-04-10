#include <main.h>

#include "../../src/automation/automation_debug_session.h"
#include "../../src/automation/automation_invocation.h"

namespace {

Automation4::AutomationDebugCapturedState MakeCapturedState(int line) {
	Automation4::AutomationDebugCapturedState captured;
	Automation4::AutomationDebugFrame frame;
	frame.level = 0;
	frame.kind = "lua";
	frame.function_name = "macro";
	frame.location = {
		"automation/tests/automation/debug-session-test.lua",
		"script",
		"debug-session-test.lua",
		line,
		1
	};
	captured.frames.push_back(std::move(frame));
	return captured;
}

TEST(AutomationDebugSession, retains_most_recent_pauses_when_capacity_is_reached) {
	Automation4::AutomationDebugLaunchRequest request;
	request.enabled = true;
	request.nonblocking = true;
	request.max_pauses = 2;
	request.breakpoints = {
		{"automation/tests/automation/debug-session-test.lua", 10, true},
		{"automation/tests/automation/debug-session-test.lua", 11, true},
		{"automation/tests/automation/debug-session-test.lua", 12, true},
	};

	Automation4::AutomationDebugSession session(request);
	session.SetTarget({
		"Lua",
		agi::fs::path("automation/tests/automation/debug-session-test.lua"),
		"Debug smoke"
	});
	session.BeginInvocation(Automation4::MakeMacroRunInvocation("Debug smoke"));

	for (int line : {10, 11, 12}) {
		EXPECT_TRUE(session.HandleHookPause(
			{
				"automation/tests/automation/debug-session-test.lua",
				"script",
				"debug-session-test.lua",
				line,
				1
			},
			1,
			[line] { return MakeCapturedState(line); }));
	}

	session.EndInvocation();

	EXPECT_EQ(3u, session.PauseCount());
	EXPECT_EQ(1u, session.DroppedPauseCount());

	auto pauses = session.GetPauses();
	ASSERT_EQ(2u, pauses.size());
	EXPECT_EQ(2u, pauses[0].sequence);
	EXPECT_EQ(11, pauses[0].location.line);
	EXPECT_EQ(3u, pauses[1].sequence);
	EXPECT_EQ(12, pauses[1].location.line);
}

}
