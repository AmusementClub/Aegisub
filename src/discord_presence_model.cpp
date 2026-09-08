#include "discord_presence_model.h"

#include <libaegisub/fs.h>

#include <algorithm>
#include <charconv>

namespace discord_presence {
namespace {

std::string LimitText(std::string_view value) {
	constexpr size_t max_bytes = 128;
	auto length = std::min(value.size(), max_bytes);
	if (length < value.size()) {
		while (length > 0 && (static_cast<unsigned char>(value[length]) & 0xc0) == 0x80)
			--length;
	}
	return std::string(value.substr(0, length));
}

}

std::optional<uint64_t> ParseApplicationId(std::string_view value) {
	if (value.empty())
		return std::nullopt;
	uint64_t id = 0;
	auto const result = std::from_chars(value.data(), value.data() + value.size(), id);
	if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || id == 0)
		return std::nullopt;
	return id;
}

Model::Model(Clock::time_point application_started)
	: application_started(application_started) {
}

void Model::UpdateDocument(SessionId id, std::filesystem::path const& filename,
						   bool new_session, WallClock::time_point now) {
	auto name = filename.empty() ? std::string("Untitled subtitles")
								 : agi::fs::PathToString(filename.filename());
	name = LimitText(name);
	auto const started_at = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
	auto found = std::ranges::find(sessions, id, &Session::id);
	if (found == sessions.end()) {
		sessions.push_back({.id = id, .filename = std::move(name), .started_at = started_at});
		return;
	}
	found->filename = std::move(name);
	if (new_session)
		found->started_at = started_at;
}

void Model::Activate(SessionId id) {
	auto found = std::ranges::find(sessions, id, &Session::id);
	if (found != sessions.end())
		std::rotate(found, std::next(found), sessions.end());
}

void Model::Remove(SessionId id) {
	std::erase_if(sessions, [id](Session const& session) { return session.id == id; });
}

std::optional<Activity> Model::GetActivity(std::string_view application_name,
										   Clock::time_point now) const {
	if (sessions.empty())
		return std::nullopt;

	auto const minutes = std::max<int64_t>(0,
										   std::chrono::duration_cast<std::chrono::minutes>(now - application_started).count());
	auto const& session = sessions.back();
	return Activity{
		.name = LimitText(application_name.empty() ? "Aegisub" : application_name),
		.details = session.filename,
		.state = "Aegisub running for " + std::to_string(minutes / 60) + "h " + std::to_string(minutes % 60) + "m",
		.started_at = session.started_at};
}

}
