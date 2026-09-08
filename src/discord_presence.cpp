#include "discord_presence.h"

#include "discord_presence_client.h"
#include "options.h"
#include "subs_controller.h"

#include <libaegisub/fs.h>

namespace {

discord_presence::Model::SessionId SessionId(SubsController *controller) {
	return reinterpret_cast<discord_presence::Model::SessionId>(controller);
}

}

DiscordPresence::DiscordPresence(discord_presence::Model::Clock::time_point application_started)
	: model(application_started), timer(this) {
	Bind(wxEVT_TIMER, [this](wxTimerEvent&) { Update(); });
	for (auto const *option : {"Discord/Enabled", "Discord/Application ID", "Discord/Application Name", "Discord/Large Image Key"})
		option_connections.emplace_back(OPT_SUB(option, [this] { Update(); }));
}

DiscordPresence::~DiscordPresence() {
	timer.Stop();
}

void DiscordPresence::Track(SubsController *controller) {
	if (documents.contains(controller))
		return;
	auto& connections = documents[controller];
	connections.emplace_back(controller->AddFileOpenListener(
		[this, controller](agi::fs::path const& filename, bool is_reload) {
			model.UpdateDocument(SessionId(controller), filename, !is_reload,
								 discord_presence::Model::WallClock::now());
			Update();
		}));
	connections.emplace_back(controller->AddFileSaveListener([this, controller] {
		CallAfter([this, controller] {
			if (!documents.contains(controller))
				return;
			model.UpdateDocument(SessionId(controller), controller->Filename(), false,
								 discord_presence::Model::WallClock::now());
			Update();
		});
	}));
	model.UpdateDocument(SessionId(controller),
						 controller->HasFile() ? controller->Filename() : agi::fs::path(), true,
						 discord_presence::Model::WallClock::now());
	Update();
}

void DiscordPresence::Activate(SubsController *controller) {
	model.Activate(SessionId(controller));
	Update();
}

void DiscordPresence::Remove(SubsController *controller) {
	documents.erase(controller);
	model.Remove(SessionId(controller));
	Update();
}

void DiscordPresence::Update() {
	auto const enabled = OPT_GET("Discord/Enabled")->GetBool();
	auto const application_id = enabled
									? discord_presence::ParseApplicationId(OPT_GET("Discord/Application ID")->GetString())
									: std::nullopt;
	if (application_id && !client)
		client = std::make_unique<discord_presence::Client>();
	if (!application_id) {
		timer.Stop();
		if (client) {
			client->Update(std::nullopt, std::nullopt);
			client.reset();
		}
		return;
	}
	if (!timer.IsRunning())
		timer.Start(1000);
	if (client) {
		auto activity = model.GetActivity(
			OPT_GET("Discord/Application Name")->GetString(), discord_presence::Model::Clock::now());
		if (activity)
			activity->large_image = OPT_GET("Discord/Large Image Key")->GetString();
		client->Update(application_id, std::move(activity));
	}
}

std::string DiscordPresence::GetStatus() const {
	if (!OPT_GET("Discord/Enabled")->GetBool())
		return "Disconnected";
	if (!discord_presence::ParseApplicationId(OPT_GET("Discord/Application ID")->GetString()))
		return "Enter a valid Discord Application ID";
	return client ? client->GetStatus() : "Waiting for a subtitle session";
}
