#include <main.h>

#include "../../src/discord_presence_client.h"
#include "../../src/discord_presence_model.h"

#include <libaegisub/fs.h>
#include <libaegisub/cajun/reader.h>

#ifdef _WIN32
#include <boost/asio/read.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/windows/overlapped_ptr.hpp>
#include <boost/asio/windows/stream_handle.hpp>
#include <boost/asio/write.hpp>
#include <array>
#include <atomic>
#include <sstream>
#include <stdexcept>
#include <thread>
#endif

namespace {
using namespace std::chrono_literals;
using discord_presence::Model;

Model::Clock::time_point Steady(int64_t seconds) {
	return Model::Clock::time_point(std::chrono::seconds(seconds));
}

Model::WallClock::time_point Wall(int64_t seconds) {
	return Model::WallClock::time_point(std::chrono::seconds(seconds));
}
}

TEST(discord_presence, application_uptime_updates_without_resetting_subtitle_timer) {
	Model model(Steady(100));
	model.UpdateDocument(1, "subtitles/episode_01.ass", true, Wall(1700000000));
	auto initial = model.GetActivity("Aegisub", Steady(100));
	ASSERT_TRUE(initial.has_value());
	EXPECT_EQ("episode_01.ass", initial->details);
	EXPECT_EQ("Aegisub running for 0h 0m", initial->state);
	EXPECT_EQ(1700000000, initial->started_at);
	EXPECT_EQ(initial, model.GetActivity("Aegisub", Steady(159)));
	auto minute = model.GetActivity("Aegisub", Steady(160));
	ASSERT_TRUE(minute.has_value());
	EXPECT_EQ("Aegisub running for 0h 1m", minute->state);
	EXPECT_EQ(initial->started_at, minute->started_at);
	auto hour = model.GetActivity("Aegisub", Steady(100 + 83 * 60));
	ASSERT_TRUE(hour.has_value());
	EXPECT_EQ("Aegisub running for 1h 23m", hour->state);
	EXPECT_EQ(initial->started_at, hour->started_at);
}

TEST(discord_presence, save_save_as_and_reload_preserve_the_session_start) {
	Model model(Steady(100));
	model.UpdateDocument(1, "subtitles/original.ass", true, Wall(1000));
	for (auto const *filename : {"subtitles/original.ass", "subtitles/renamed.ass", "subtitles/renamed.ass"}) {
		model.UpdateDocument(1, filename, false, Wall(1500));
		auto activity = model.GetActivity("Aegisub", Steady(200));
		ASSERT_TRUE(activity.has_value());
		EXPECT_EQ(1000, activity->started_at);
		EXPECT_EQ(agi::fs::PathToString(std::filesystem::path(filename).filename()), activity->details);
	}
}

TEST(discord_presence, opening_or_creating_subtitles_resets_only_the_subtitle_timer) {
	Model model(Steady(0));
	model.UpdateDocument(1, "first.ass", true, Wall(1000));
	model.UpdateDocument(1, "second.ass", true, Wall(1200));
	auto activity = model.GetActivity("Aegisub", Steady(300));
	ASSERT_TRUE(activity.has_value());
	EXPECT_EQ("second.ass", activity->details);
	EXPECT_EQ(1200, activity->started_at);
	EXPECT_EQ("Aegisub running for 0h 5m", activity->state);
	model.UpdateDocument(1, {}, true, Wall(1300));
	activity = model.GetActivity("Aegisub", Steady(420));
	ASSERT_TRUE(activity.has_value());
	EXPECT_EQ("Untitled subtitles", activity->details);
	EXPECT_EQ(1300, activity->started_at);
	EXPECT_EQ("Aegisub running for 0h 7m", activity->state);
}

TEST(discord_presence, switching_and_closing_windows_preserves_each_session) {
	Model model(Steady(0));
	EXPECT_FALSE(model.GetActivity("Aegisub", Steady(0)).has_value());
	model.UpdateDocument(1, "first.ass", true, Wall(1000));
	model.UpdateDocument(2, "second.ass", true, Wall(1100));
	model.Activate(1);
	auto activity = model.GetActivity("Aegisub", Steady(200));
	ASSERT_TRUE(activity.has_value());
	EXPECT_EQ("first.ass", activity->details);
	EXPECT_EQ(1000, activity->started_at);
	model.UpdateDocument(2, "second-renamed.ass", false, Wall(1200));
	EXPECT_EQ(activity, model.GetActivity("Aegisub", Steady(200)));
	model.Remove(1);
	activity = model.GetActivity("Aegisub", Steady(200));
	ASSERT_TRUE(activity.has_value());
	EXPECT_EQ("second-renamed.ass", activity->details);
	EXPECT_EQ(1100, activity->started_at);
	model.Remove(2);
	EXPECT_FALSE(model.GetActivity("Aegisub", Steady(200)).has_value());
}

TEST(discord_presence, custom_name_and_utf8_limits_preserve_timestamps) {
	Model model(Steady(0));
	std::string const cjk = "\xe5\xad\x97";
	auto long_name = std::string(127, 'a') + cjk + ".ass";
	model.UpdateDocument(1, agi::fs::PathFromString("subtitles/" + long_name), true, Wall(1000));
	auto activity = model.GetActivity("Subtitle Studio", Steady(0));
	ASSERT_TRUE(activity.has_value());
	EXPECT_EQ("Subtitle Studio", activity->name);
	EXPECT_EQ(std::string(127, 'a'), activity->details);
	EXPECT_EQ(1000, activity->started_at);
	activity = model.GetActivity("", Steady(0));
	ASSERT_TRUE(activity.has_value());
	EXPECT_EQ("Aegisub", activity->name);
	activity = model.GetActivity(std::string(126, 'b') + cjk, Steady(0));
	ASSERT_TRUE(activity.has_value());
	EXPECT_EQ(std::string(126, 'b'), activity->name);
	EXPECT_EQ(1000, activity->started_at);
}

TEST(discord_presence, application_ids_reject_empty_zero_partial_and_overflow_values) {
	for (auto const *value : {"", "0", "-1", "+123", "123x", " 123", "18446744073709551616"})
		EXPECT_FALSE(discord_presence::ParseApplicationId(value).has_value()) << value;
	EXPECT_EQ(123456789012345678ULL, discord_presence::ParseApplicationId("123456789012345678"));
}

#ifdef _WIN32
namespace {

class DiscordPipe {
	boost::asio::io_context io;
	boost::asio::windows::stream_handle pipe{io};
	boost::asio::steady_timer timeout{io};
	std::string endpoint;

	template <typename Start>
	void Run(Start start) {
		io.restart();
		bool expired = false;
		boost::system::error_code result;
		timeout.expires_after(8s);
		timeout.async_wait([&](boost::system::error_code error) {
			if (!error) {
				expired = true;
				pipe.cancel();
			}
		});
		start([&](boost::system::error_code error, size_t) {
			result = error;
			timeout.cancel();
		});
		io.run();
		if (expired)
			throw std::runtime_error("Discord IPC test operation timed out");
		if (result)
			throw std::runtime_error("Discord IPC test operation failed: " + result.message());
	}

	void ReadBytes(void *bytes, size_t size) {
		Run([&](auto done) { boost::asio::async_read(pipe, boost::asio::buffer(bytes, size), std::move(done)); });
	}

	public:
	DiscordPipe() {
		static std::atomic<unsigned> next{0};
		endpoint = R"(\\.\pipe\aegisub-discord-test-)" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(++next);
		Open();
	}

	[[nodiscard]] std::string const& Endpoint() const { return endpoint; }

	void Open() {
		auto handle = CreateNamedPipeA(endpoint.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
									   PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 4096, 4096, 0, nullptr);
		if (handle == INVALID_HANDLE_VALUE)
			throw std::runtime_error("Could not create isolated Discord test pipe");
		pipe.assign(handle);
	}

	void Close() { pipe.close(); }

	void Accept() {
		Run([&](auto done) {
			boost::asio::windows::overlapped_ptr operation(io, std::move(done));
			auto connected = ConnectNamedPipe(pipe.native_handle(), operation.get());
			auto error = connected ? ERROR_SUCCESS : GetLastError();
			if (error == ERROR_IO_PENDING)
				operation.release();
			else
				operation.complete(boost::system::error_code(
									   error == ERROR_PIPE_CONNECTED ? ERROR_SUCCESS : error, boost::system::system_category()),
								   0);
		});
	}

	std::pair<uint32_t, std::string> ReadPacket() {
		std::array<unsigned char, 8> header{};
		ReadBytes(header.data(), header.size());
		auto word = [&](size_t offset) {
			return static_cast<uint32_t>(header[offset]) | (static_cast<uint32_t>(header[offset + 1]) << 8) | (static_cast<uint32_t>(header[offset + 2]) << 16) | (static_cast<uint32_t>(header[offset + 3]) << 24);
		};
		auto length = word(4);
		if (length > 65536)
			throw std::runtime_error("Oversized packet from Discord client");
		std::string body(length, '\0');
		ReadBytes(body.data(), body.size());
		return {word(0), body};
	}

	void Write(std::string_view bytes) {
		Run([&](auto done) { boost::asio::async_write(pipe, boost::asio::buffer(bytes), std::move(done)); });
	}

	void Reply(uint32_t opcode, std::string const& body) {
		std::string header(8, '\0');
		for (size_t index = 0; index < 4; ++index) {
			header[index] = static_cast<char>((opcode >> (8 * index)) & 255);
			header[index + 4] = static_cast<char>((body.size() >> (8 * index)) & 255);
		}
		// Split the frame to exercise the transport's exact-length reads.
		Write(std::string_view(header).substr(0, 3));
		Write(std::string_view(header).substr(3));
		Write(body);
	}
};

json::UnknownElement ReadJson(DiscordPipe& server, uint32_t expected_opcode) {
	auto [opcode, body] = server.ReadPacket();
	EXPECT_EQ(expected_opcode, opcode);
	std::istringstream input(body);
	json::UnknownElement result;
	json::Reader::Read(result, input);
	return result;
}

void ExpectActivity(DiscordPipe& server, discord_presence::Activity const& expected) {
	auto packet = ReadJson(server, 1);
	auto const& command = static_cast<json::Object const&>(packet);
	EXPECT_EQ("SET_ACTIVITY", static_cast<json::String const&>(command.at("cmd")));
	auto const& args = static_cast<json::Object const&>(command.at("args"));
	EXPECT_EQ(GetCurrentProcessId(), static_cast<json::Integer const&>(args.at("pid")));
	auto const& activity = static_cast<json::Object const&>(args.at("activity"));
	EXPECT_EQ(expected.name, static_cast<json::String const&>(activity.at("name")));
	EXPECT_EQ(expected.details, static_cast<json::String const&>(activity.at("details")));
	EXPECT_EQ(expected.state, static_cast<json::String const&>(activity.at("state")));
	auto const& timestamps = static_cast<json::Object const&>(activity.at("timestamps"));
	EXPECT_EQ(expected.started_at, static_cast<json::Integer const&>(timestamps.at("start")));
	EXPECT_EQ(0, timestamps.count("end"));
	if (expected.large_image.empty()) {
		EXPECT_EQ(0, activity.count("assets"));
	}
	else {
		auto const& assets = static_cast<json::Object const&>(activity.at("assets"));
		EXPECT_EQ(expected.large_image, static_cast<json::String const&>(assets.at("large_image")));
	}
}

void Handshake(DiscordPipe& server) {
	server.Accept();
	auto packet = ReadJson(server, 0);
	auto const& handshake = static_cast<json::Object const&>(packet);
	EXPECT_EQ(1, static_cast<json::Integer const&>(handshake.at("v")));
	EXPECT_EQ("123456789012345678", static_cast<json::String const&>(handshake.at("client_id")));
	server.Reply(1, R"({"cmd":"DISPATCH","evt":"READY","data":{}})");
}

bool WaitForStatus(discord_presence::Client const& client, std::string const& expected) {
	auto const deadline = std::chrono::steady_clock::now() + 2s;
	while (std::chrono::steady_clock::now() < deadline) {
		if (client.GetStatus() == expected)
			return true;
		std::this_thread::sleep_for(10ms);
	}
	return false;
}

}

TEST(discord_presence, ipc_publishes_custom_name_uptime_and_subtitle_timestamp_then_clears) {
	DiscordPipe server;
	discord_presence::Client client(server.Endpoint());
	Model model(Steady(0));
	model.UpdateDocument(1, "subtitles/episode.ass", true, Wall(1700000000));
	auto activity = model.GetActivity("Subtitle Studio", Steady(83 * 60));
	ASSERT_TRUE(activity.has_value());
	activity->large_image = "aegisub";
	client.Update(123456789012345678ULL, activity);
	Handshake(server);
	ExpectActivity(server, *activity);
	server.Reply(1, R"({"cmd":"SET_ACTIVITY","evt":null,"data":{}})");
	EXPECT_TRUE(WaitForStatus(client, "Connected"));
	server.Reply(3, "ping");
	auto pong = server.ReadPacket();
	EXPECT_EQ(4, pong.first);
	EXPECT_EQ("ping", pong.second);

	auto updated = model.GetActivity("Subtitle Studio", Steady(84 * 60));
	ASSERT_TRUE(updated.has_value());
	client.Update(123456789012345678ULL, updated);
	ExpectActivity(server, *updated);
	EXPECT_EQ(activity->started_at, updated->started_at);
	client.Update(std::nullopt, updated);
	auto clear = ReadJson(server, 1);
	auto const& args = static_cast<json::Object const&>(static_cast<json::Object const&>(clear).at("args"));
	EXPECT_NO_THROW(static_cast<void>(static_cast<json::Null const&>(args.at("activity"))));
	EXPECT_TRUE(WaitForStatus(client, "Disconnected"));
}

TEST(discord_presence, ipc_reconnects_with_the_latest_activity_and_original_session_start) {
	DiscordPipe server;
	discord_presence::Client client(server.Endpoint());
	discord_presence::Activity first{.name = "Aegisub", .details = "first.ass", .state = "Aegisub running for 0h 1m", .started_at = 1000};
	client.Update(123456789012345678ULL, first);
	Handshake(server);
	ExpectActivity(server, first);
	server.Close();
	ASSERT_TRUE(WaitForStatus(client, "Waiting for Discord"));
	server.Open();
	Handshake(server);
	ExpectActivity(server, first);
	server.Close();
	ASSERT_TRUE(WaitForStatus(client, "Waiting for Discord"));
	server.Open();
	auto latest = first;
	latest.name = "Subtitle Studio";
	latest.details = "renamed.ass";
	latest.state = "Aegisub running for 0h 2m";
	client.Update(123456789012345678ULL, latest);
	Handshake(server);
	ExpectActivity(server, latest);
}

TEST(discord_presence, ipc_rejects_oversized_frames_and_stops_with_a_pending_read) {
	DiscordPipe server;
	auto client = std::make_unique<discord_presence::Client>(server.Endpoint());
	discord_presence::Activity const activity{.name = "Aegisub", .details = "test.ass", .state = "0m", .started_at = 1000};
	client->Update(123456789012345678ULL, activity);
	Handshake(server);
	ExpectActivity(server, activity);
	server.Write(std::string_view("\x01\x00\x00\x00\xff\xff\xff\x7f", 8));
	EXPECT_TRUE(WaitForStatus(*client, "Invalid Discord response"));
	client.reset();
	server.Close();
	server.Open();
	client = std::make_unique<discord_presence::Client>(server.Endpoint());
	client->Update(123456789012345678ULL, activity);
	server.Accept();
	ReadJson(server, 0);
	server.Write(std::string_view("\x01\x00\x00\x00\x64\x00\x00\x00{", 9));
	auto const started = std::chrono::steady_clock::now();
	client.reset();
	EXPECT_LT(std::chrono::steady_clock::now() - started, 2s);
}
#endif
