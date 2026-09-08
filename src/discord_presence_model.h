#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace discord_presence {

struct Activity {
	std::string name;
	std::string details;
	std::string state;
	int64_t started_at = 0;
	std::string large_image;

	bool operator==(Activity const&) const = default;
};

std::optional<uint64_t> ParseApplicationId(std::string_view value);

class Model {
	public:
	using SessionId = uintptr_t;
	using Clock = std::chrono::steady_clock;
	using WallClock = std::chrono::system_clock;

	private:
	struct Session {
		SessionId id;
		std::string filename;
		int64_t started_at;
	};

	Clock::time_point application_started;
	// The last entry is the most recently selected project.
	std::vector<Session> sessions;

	public:
	explicit Model(Clock::time_point application_started);
	void UpdateDocument(SessionId id, std::filesystem::path const& filename,
						bool new_session, WallClock::time_point now);
	void Activate(SessionId id);
	void Remove(SessionId id);
	std::optional<Activity> GetActivity(std::string_view application_name,
										Clock::time_point now) const;
};

}
