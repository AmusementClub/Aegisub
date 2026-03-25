#include <main.h>

#include "../../src/ui_services.h"

TEST(ui_services, null_interaction_sink_uses_safe_defaults) {
	agi::NullInteractionSink sink;

	EXPECT_EQ(agi::InteractionResult::Ok, sink.Request({"title", "message", agi::InteractionButtons::Ok}));
	EXPECT_EQ(agi::InteractionResult::Cancel, sink.Request({"title", "message", agi::InteractionButtons::OkCancel}));
	EXPECT_EQ(agi::InteractionResult::No, sink.Request({"title", "message", agi::InteractionButtons::YesNo}));
	EXPECT_EQ(agi::InteractionResult::Cancel, sink.Request({"title", "message", agi::InteractionButtons::YesNoCancel}));
}

TEST(ui_services, null_single_choice_interaction_sink_cancels) {
	agi::NullSingleChoiceInteractionSink sink;

	EXPECT_EQ(std::nullopt, sink.RequestSingleChoice({
		"title",
		"message",
		{"first", "second"},
		0
	}));
}

TEST(ui_services, null_file_dialog_service_cancels) {
	agi::NullFileDialogService sink;

	EXPECT_TRUE(sink.RequestOpenFile({
		"Open video file",
		"Path/Last/Video",
		"",
		"",
		"Video Files|*.mkv"
	}).empty());
	EXPECT_TRUE(sink.RequestOpenFiles({
		"Open video files",
		"Path/Last/Video",
		"",
		"",
		"Video Files|*.mkv"
	}).empty());
	EXPECT_TRUE(sink.RequestSaveFile({
		"Save video file",
		"Path/Last/Video",
		"clip.mkv",
		"mkv",
		"Video Files|*.mkv"
	}).empty());
	EXPECT_TRUE(sink.RequestSelectDirectory({
		"Select export directory",
		"C:/temp"
	}).empty());
}

TEST(ui_services, null_video_source_request_service_cancels) {
	agi::NullVideoSourceRequestService sink;

	EXPECT_TRUE(sink.RequestDummyVideoPath().empty());
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

TEST(ui_services, null_notification_sink_accepts_all_levels) {
	agi::NullNotificationSink sink;

	sink.ShowInfo("title", "message");
	sink.ShowWarning("title", "message");
	sink.ShowError("title", "message");
}
