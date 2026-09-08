#pragma once

#include "discord_presence_model.h"

#include <memory>
#include <optional>
#include <string>

namespace discord_presence {

class Client {
	struct Impl;
	std::unique_ptr<Impl> impl;

	public:
	explicit Client(std::string const& endpoint = {});
	~Client();
	Client(Client const&) = delete;
	Client& operator=(Client const&) = delete;

	void Update(std::optional<uint64_t> application_id, std::optional<Activity> activity);
	[[nodiscard]] std::string GetStatus() const;
};

}
