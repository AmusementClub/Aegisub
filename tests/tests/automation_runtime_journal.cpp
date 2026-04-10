#include <main.h>

#include "../../src/automation/automation_invocation.h"
#include "../../src/automation/automation_runtime_journal.h"

namespace {

Automation4::AutomationRuntimeStateSnapshot MakeRuntimeSnapshot(std::string feature_name, int active_row) {
	Automation4::AutomationRuntimeStateSnapshot snapshot;
	snapshot.invocation = Automation4::MakeMacroRunInvocation(std::move(feature_name));
	snapshot.context_snapshot.has_project_context = true;
	snapshot.context_snapshot.selection.active_row = active_row;
	snapshot.context_snapshot.project.active_row = active_row;
	return snapshot;
}

TEST(AutomationRuntimeJournal, retains_recent_records_with_bounded_capacity) {
	Automation4::AutomationRuntimeJournal journal(2);

	journal.OnRuntimeStateSnapshot(MakeRuntimeSnapshot("one", 1));
	journal.OnRuntimeStateSnapshot(MakeRuntimeSnapshot("two", 2));
	journal.OnRuntimeStateSnapshot(MakeRuntimeSnapshot("three", 3));

	EXPECT_EQ(3u, journal.RecordCount());
	EXPECT_EQ(2u, journal.RetainedRecordCount());
	EXPECT_EQ(1u, journal.DroppedRecordCount());

	auto records = journal.GetRecords();
	ASSERT_EQ(2u, records.size());
	EXPECT_EQ(2u, records[0].sequence);
	EXPECT_EQ("two", records[0].snapshot.invocation.feature_name);
	EXPECT_EQ(3u, records[1].sequence);
	EXPECT_EQ("three", records[1].snapshot.invocation.feature_name);
}

TEST(AutomationRuntimeJournal, last_snapshot_tracks_latest_record_after_eviction) {
	Automation4::AutomationRuntimeJournal journal(1);

	auto first = MakeRuntimeSnapshot("first", 4);
	auto second = MakeRuntimeSnapshot("second", 8);
	second.template_debug = Automation4::AutomationTemplateDebugState{};
	second.template_debug->kind = "expression-run";

	journal.OnRuntimeStateSnapshot(first);
	journal.OnRuntimeStateSnapshot(second);

	auto last = journal.LastSnapshot();
	ASSERT_TRUE(last.has_value());
	EXPECT_EQ("second", last->invocation.feature_name);
	EXPECT_EQ(8, last->context_snapshot.selection.active_row);
	EXPECT_TRUE(last->template_debug.has_value());
	EXPECT_EQ(1u, journal.TemplateEventCount());
}

}
