#include <main.h>

#include "../../src/ui_services.h"

TEST(ui_services, null_interaction_sink_uses_safe_defaults) {
	agi::NullInteractionSink sink;

	EXPECT_EQ(agi::InteractionResult::Ok, sink.Request({"title", "message", agi::InteractionButtons::Ok}));
	EXPECT_EQ(agi::InteractionResult::Cancel, sink.Request({"title", "message", agi::InteractionButtons::OkCancel}));
	EXPECT_EQ(agi::InteractionResult::No, sink.Request({"title", "message", agi::InteractionButtons::YesNo}));
	EXPECT_EQ(agi::InteractionResult::Cancel, sink.Request({"title", "message", agi::InteractionButtons::YesNoCancel}));
}

TEST(ui_services, inline_background_runner_executes_task) {
	agi::InlineBackgroundRunnerFactory factory;
	auto runner = factory.Create("title", "message");

	bool ran = false;
	runner->Run([&](agi::ProgressSink *sink) {
		ran = true;
		sink->SetTitle("title");
		sink->SetMessage("message");
		sink->SetProgress(1, 4);
		sink->SetIndeterminate();
		sink->Log("log");
		EXPECT_FALSE(sink->IsCancelled());
	});

	EXPECT_TRUE(ran);
}
