#pragma once

#include "discord_presence_model.h"

#include <libaegisub/signal.h>

#include <map>
#include <memory>
#include <string>
#include <vector>
#include <wx/event.h>
#include <wx/timer.h>

class SubsController;
namespace discord_presence {
class Client;
}

class DiscordPresence final : public wxEvtHandler {
	discord_presence::Model model;
	std::unique_ptr<discord_presence::Client> client;
	wxTimer timer;
	std::map<SubsController *, std::vector<agi::signal::Connection>> documents;
	std::vector<agi::signal::Connection> option_connections;

	void Update();

	public:
	explicit DiscordPresence(discord_presence::Model::Clock::time_point application_started);
	~DiscordPresence() override;
	void Track(SubsController *controller);
	void Activate(SubsController *controller);
	void Remove(SubsController *controller);
	[[nodiscard]] std::string GetStatus() const;
};
