#include <main.h>

#include "../../src/automation/automation_debug_adapter.h"
#include "../../src/automation/automation_debug_service.h"
#include "../../src/automation/automation_debug_session.h"
#include "../../src/automation/automation_invocation.h"
#include "../../src/options.h"

#include <boost/asio/ip/tcp.hpp>

#include <libaegisub/cajun/elements.h>
#include <libaegisub/cajun/reader.h>
#include <libaegisub/cajun/writer.h>
#include <libaegisub/option.h>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
using namespace std::chrono_literals;

constexpr char kDebugSessionScriptPath[] = "automation/tests/automation/debug-session-test.lua";
constexpr char kDebugSessionScriptName[] = "debug-session-test.lua";
constexpr char kDebugSessionSubtitlePath[] = "automation/tests/automation/kara-templater-retime.ass";
constexpr char kDebugServiceOptionConfig[] = R"({
	"Automation" : {
		"Debug" : {
			"Listen Port" : 0,
			"Require Token" : true,
			"Token" : ""
		}
	}
})";
constexpr char kDebugServicePartialOptionConfig[] = R"({
	"Automation" : {
	}
})";

class ScopedDebugServiceOptions final {
	bool owns_options = false;
public:
	ScopedDebugServiceOptions()
	{
		if (!config::opt) {
			config::opt = new agi::Options("", kDebugServiceOptionConfig, agi::Options::FLUSH_SKIP);
			owns_options = true;
		}
	}

	~ScopedDebugServiceOptions()
	{
		if (!owns_options)
			return;
		delete config::opt;
		config::opt = nullptr;
	}

	ScopedDebugServiceOptions(ScopedDebugServiceOptions const&) = delete;
	ScopedDebugServiceOptions& operator=(ScopedDebugServiceOptions const&) = delete;
};

class ScopedReplaceDebugServiceOptions final {
	agi::Options *previous = nullptr;
public:
	template<size_t N>
	explicit ScopedReplaceDebugServiceOptions(char const (&option_config)[N])
	: previous(config::opt)
	{
		config::opt = new agi::Options("", option_config, agi::Options::FLUSH_SKIP);
	}

	~ScopedReplaceDebugServiceOptions()
	{
		delete config::opt;
		config::opt = previous;
	}

	ScopedReplaceDebugServiceOptions(ScopedReplaceDebugServiceOptions const&) = delete;
	ScopedReplaceDebugServiceOptions& operator=(ScopedReplaceDebugServiceOptions const&) = delete;
};

class ScopedIntOption final {
	std::string name;
	int original = 0;
public:
	explicit ScopedIntOption(char const* option_name)
	: name(option_name)
	, original(OPT_GET(option_name)->GetInt()) {
	}

	~ScopedIntOption() {
		OPT_SET(name)->SetInt(original);
	}
};

class ScopedBoolOption final {
	std::string name;
	bool original = false;
public:
	explicit ScopedBoolOption(char const* option_name)
	: name(option_name)
	, original(OPT_GET(option_name)->GetBool()) {
	}

	~ScopedBoolOption() {
		OPT_SET(name)->SetBool(original);
	}
};

class ScopedStringOption final {
	std::string name;
	std::string original;
public:
	explicit ScopedStringOption(char const* option_name)
	: name(option_name)
	, original(OPT_GET(option_name)->GetString()) {
	}

	~ScopedStringOption() {
		OPT_SET(name)->SetString(original);
	}
};

int FindFreeLoopbackPort() {
	boost::asio::io_context io;
	boost::asio::ip::tcp::acceptor acceptor(io);
	boost::system::error_code ec;
	acceptor.open(boost::asio::ip::tcp::v4(), ec);
	if (ec)
		return 0;
	acceptor.bind({boost::asio::ip::make_address("127.0.0.1"), 0}, ec);
	if (ec)
		return 0;
	acceptor.listen(boost::asio::socket_base::max_listen_connections, ec);
	if (ec)
		return 0;
	return static_cast<int>(acceptor.local_endpoint().port());
}

std::string SerializeJson(json::Object const& value) {
	std::ostringstream out;
	agi::JsonWriter::Write(value, out);
	return out.str();
}

json::Object ParseJson(std::string const& payload) {
	json::UnknownElement message;
	std::stringstream stream(payload);
	json::Reader::Read(message, stream);
	return std::move(static_cast<json::Object&>(message));
}

std::optional<json::Object const*> FindObject(json::Object const& object, std::string const& key) {
	auto it = object.find(key);
	if (it == object.end())
		return std::nullopt;
	try {
		return &static_cast<json::Object const&>(it->second);
	}
	catch (...) {
		return std::nullopt;
	}
}

std::optional<json::Array const*> FindArray(json::Object const& object, std::string const& key) {
	auto it = object.find(key);
	if (it == object.end())
		return std::nullopt;
	try {
		return &static_cast<json::Array const&>(it->second);
	}
	catch (...) {
		return std::nullopt;
	}
}

std::optional<std::string> FindString(json::Object const& object, std::string const& key) {
	auto it = object.find(key);
	if (it == object.end())
		return std::nullopt;
	try {
		return static_cast<json::String const&>(it->second);
	}
	catch (...) {
		return std::nullopt;
	}
}

std::optional<int> FindInt(json::Object const& object, std::string const& key) {
	auto it = object.find(key);
	if (it == object.end())
		return std::nullopt;
	try {
		return static_cast<int>(static_cast<json::Integer const&>(it->second));
	}
	catch (...) {
		try {
			return static_cast<int>(static_cast<json::Double const&>(it->second));
		}
		catch (...) {
			return std::nullopt;
		}
	}
}

std::optional<bool> FindBool(json::Object const& object, std::string const& key) {
	auto it = object.find(key);
	if (it == object.end())
		return std::nullopt;
	try {
		return static_cast<json::Boolean const&>(it->second);
	}
	catch (...) {
		return std::nullopt;
	}
}

class FakeDebugConnection final : public Automation4::AutomationDebugAdapterConnection {
	mutable std::mutex mutex;
	std::condition_variable cv;
	std::deque<std::string> incoming;
	std::vector<std::string> outgoing;
	bool closed = false;

public:
	bool ReadProtocolMessage(std::string& payload, std::string& error) override {
		std::unique_lock<std::mutex> lock(mutex);
		cv.wait(lock, [&] { return closed || !incoming.empty(); });
		if (incoming.empty()) {
			error = "connection closed";
			return false;
		}

		payload = std::move(incoming.front());
		incoming.pop_front();
		return true;
	}

	bool WriteProtocolMessage(std::string const& payload, std::string&) override {
		{
			std::lock_guard<std::mutex> lock(mutex);
			outgoing.push_back(payload);
		}
		cv.notify_all();
		return true;
	}

	void Close() override {
		{
			std::lock_guard<std::mutex> lock(mutex);
			closed = true;
		}
		cv.notify_all();
	}

	void PushRequest(json::Object request) {
		{
			std::lock_guard<std::mutex> lock(mutex);
			incoming.push_back(SerializeJson(request));
		}
		cv.notify_all();
	}

	bool WaitForMessage(
		size_t& cursor,
		std::function<bool(json::Object const&)> const& predicate,
		json::Object& result,
		std::chrono::milliseconds timeout = 2s) {
		auto const deadline = std::chrono::steady_clock::now() + timeout;
		size_t search_cursor = cursor;

		while (std::chrono::steady_clock::now() < deadline) {
			std::vector<std::string> snapshot;
			{
				std::unique_lock<std::mutex> lock(mutex);
				cv.wait_until(lock, deadline, [&] {
					return closed || outgoing.size() > search_cursor;
				});
				if (outgoing.size() > search_cursor)
					snapshot.assign(outgoing.begin() + search_cursor, outgoing.end());
			}

			if (snapshot.empty())
				continue;

			for (size_t i = 0; i < snapshot.size(); ++i) {
				auto message = ParseJson(snapshot[i]);
				if (predicate(message)) {
					cursor = search_cursor + i + 1;
					result = std::move(message);
					return true;
				}
			}

			search_cursor += snapshot.size();
		}

		return false;
	}
};

json::Object MakeRequest(int seq, std::string const& command, json::Object arguments = {}) {
	json::Object request;
	request["seq"] = static_cast<json::Integer>(seq);
	request["type"] = "request";
	request["command"] = command;
	request["arguments"] = std::move(arguments);
	return request;
}

bool WaitForResponse(
	FakeDebugConnection& connection,
	size_t& cursor,
	int request_seq,
	std::string const& command,
	json::Object& response,
	std::chrono::milliseconds timeout = 2s) {
	return connection.WaitForMessage(cursor, [&](json::Object const& message) {
		return FindString(message, "type") == std::optional<std::string>{"response"}
			&& FindInt(message, "request_seq") == std::optional<int>{request_seq}
			&& FindString(message, "command") == std::optional<std::string>{command};
	}, response, timeout);
}

bool WaitForEvent(
	FakeDebugConnection& connection,
	size_t& cursor,
	std::string const& event_name,
	json::Object& event,
	std::chrono::milliseconds timeout = 2s) {
	return connection.WaitForMessage(cursor, [&](json::Object const& message) {
		return FindString(message, "type") == std::optional<std::string>{"event"}
			&& FindString(message, "event") == std::optional<std::string>{event_name};
	}, event, timeout);
}

std::vector<std::string> ExtractScopeNames(json::Object const& scopes_response) {
	std::vector<std::string> names;
	auto body = FindObject(scopes_response, "body");
	if (!body)
		return names;

	auto scopes = FindArray(**body, "scopes");
	if (!scopes)
		return names;

	for (auto const& entry : **scopes) {
		try {
			auto const& scope = static_cast<json::Object const&>(entry);
			if (auto name = FindString(scope, "name"))
				names.push_back(*name);
		}
		catch (...) {
		}
	}
	return names;
}

std::optional<int> FindScopeReference(json::Object const& scopes_response, std::string const& scope_name) {
	auto body = FindObject(scopes_response, "body");
	if (!body)
		return std::nullopt;

	auto scopes = FindArray(**body, "scopes");
	if (!scopes)
		return std::nullopt;

	for (auto const& entry : **scopes) {
		try {
			auto const& scope = static_cast<json::Object const&>(entry);
			if (FindString(scope, "name") == std::optional<std::string>{scope_name})
				return FindInt(scope, "variablesReference");
		}
		catch (...) {
		}
	}

	return std::nullopt;
}

std::vector<std::string> ExtractVariableNames(json::Object const& variables_response) {
	std::vector<std::string> names;
	auto body = FindObject(variables_response, "body");
	if (!body)
		return names;

	auto variables = FindArray(**body, "variables");
	if (!variables)
		return names;

	for (auto const& entry : **variables) {
		try {
			auto const& variable = static_cast<json::Object const&>(entry);
			if (auto name = FindString(variable, "name"))
				names.push_back(*name);
		}
		catch (...) {
		}
	}
	return names;
}

std::optional<int> FindVariableReference(json::Object const& variables_response, std::string const& variable_name) {
	auto body = FindObject(variables_response, "body");
	if (!body)
		return std::nullopt;

	auto variables = FindArray(**body, "variables");
	if (!variables)
		return std::nullopt;

	for (auto const& entry : **variables) {
		try {
			auto const& variable = static_cast<json::Object const&>(entry);
			if (FindString(variable, "name") == std::optional<std::string>{variable_name})
				return FindInt(variable, "variablesReference");
		}
		catch (...) {
		}
	}

	return std::nullopt;
}

TEST(AutomationDebugAdapter, exposes_merged_aegisub_scope_and_continues_paused_session) {
	Automation4::AutomationDebugService service;
	ASSERT_TRUE(service.SetEnabled(true));

	auto connection = std::make_unique<FakeDebugConnection>();
	auto *raw_connection = connection.get();
	Automation4::AutomationDebugAdapter adapter(service, "test-token", std::move(connection));
	std::thread adapter_thread([&] { adapter.Run(); });

	size_t cursor = 0;

	raw_connection->PushRequest(MakeRequest(1, "initialize"));
	json::Object initialize_response;
	ASSERT_TRUE(WaitForResponse(*raw_connection, cursor, 1, "initialize", initialize_response));
	EXPECT_TRUE(FindBool(initialize_response, "success").value_or(false));

	json::Object attach_arguments;
	attach_arguments["token"] = "test-token";
	raw_connection->PushRequest(MakeRequest(2, "attach", std::move(attach_arguments)));
	json::Object attach_response;
	ASSERT_TRUE(WaitForResponse(*raw_connection, cursor, 2, "attach", attach_response));
	EXPECT_TRUE(FindBool(attach_response, "success").value_or(false));

	json::Object initialized_event;
	ASSERT_TRUE(WaitForEvent(*raw_connection, cursor, "initialized", initialized_event));

	json::Object breakpoint_source;
	breakpoint_source["path"] = kDebugSessionScriptPath;
	json::Object breakpoint;
	breakpoint["line"] = static_cast<json::Integer>(12);
	json::Array breakpoints;
	breakpoints.push_back(std::move(breakpoint));
	json::Object breakpoint_arguments;
	breakpoint_arguments["source"] = std::move(breakpoint_source);
	breakpoint_arguments["breakpoints"] = std::move(breakpoints);
	raw_connection->PushRequest(MakeRequest(3, "setBreakpoints", std::move(breakpoint_arguments)));
	json::Object breakpoint_response;
	ASSERT_TRUE(WaitForResponse(*raw_connection, cursor, 3, "setBreakpoints", breakpoint_response));
	EXPECT_TRUE(FindBool(breakpoint_response, "success").value_or(false));

	raw_connection->PushRequest(MakeRequest(4, "configurationDone"));
	json::Object configuration_response;
	ASSERT_TRUE(WaitForResponse(*raw_connection, cursor, 4, "configurationDone", configuration_response));
	EXPECT_TRUE(FindBool(configuration_response, "success").value_or(false));

	auto session = service.PrepareSession({
		"Lua",
		agi::fs::path(kDebugSessionScriptPath),
		"Debug smoke"
	});
	ASSERT_TRUE(session);

	auto invocation = Automation4::MakeMacroRunInvocation("Debug smoke");
	session->BeginInvocation(invocation);

	std::mutex pause_mutex;
	std::condition_variable pause_cv;
	bool pause_finished = false;

	std::thread pause_thread([&] {
		Automation4::AutomationDebugCapturedState captured;

		Automation4::AutomationDebugFrame frame;
		frame.level = 0;
		frame.kind = "lua";
		frame.function_name = "debug_smoke";
		frame.location = {
			kDebugSessionScriptPath,
			"script",
			kDebugSessionScriptName,
			12,
			1
		};
		frame.locals.push_back({"line", "dialogue", "userdata", {}});
		frame.upvalues.push_back({"decorate_text", "function", "function", {}});
		captured.frames.push_back(frame);

		captured.scopes.push_back({
			"Globals",
			{
				{"tags", "\"{\\\\bord2}\"", "string", {}},
				{"res", "table[8]", "table", {}}
			}
		});
		captured.scopes.push_back({
			"Functions",
			{
				{"hydradient", "function @[string \"automation/tests/automation/debug-session-test.lua\"]:400", "function", {}},
				{"linecheck", "function @[string \"automation/tests/automation/debug-session-test.lua\"]:385", "function", {}}
			}
		});
		captured.scopes.push_back({
			"Runtime Globals",
			{
				{"aegisub", "table[16]", "table", {}},
				{"string", "table[20]", "table", {}},
				{"print", "cfunction @[C]", "function", {}}
			}
		});
		captured.scopes.push_back({
			"Progress",
			{
				{"title", "Debug smoke", "string", {}},
				{"task", "Running macro", "string", {}}
			}
		});

		Automation4::AutomationRuntimeStateSnapshot runtime_snapshot;
		runtime_snapshot.invocation = invocation;
		runtime_snapshot.context_snapshot.has_project_context = true;
		runtime_snapshot.context_snapshot.selection.selected_rows = {18, 20};
		runtime_snapshot.context_snapshot.selection.active_row = 18;
		runtime_snapshot.context_snapshot.media.has_video = true;
		runtime_snapshot.context_snapshot.media.has_audio = true;
		runtime_snapshot.context_snapshot.media.video_width = 1920;
		runtime_snapshot.context_snapshot.media.video_height = 1080;
		runtime_snapshot.context_snapshot.media.video_aspect_ratio = 1.777778;
		runtime_snapshot.context_snapshot.media.has_keyframes = true;
		runtime_snapshot.context_snapshot.media.keyframes = {12, 48, 96};
		runtime_snapshot.context_snapshot.media.has_audio_selection = true;
		runtime_snapshot.context_snapshot.media.audio_selection_begin = 100;
		runtime_snapshot.context_snapshot.media.audio_selection_end = 220;
		runtime_snapshot.context_snapshot.project.script_filename = kDebugSessionScriptPath;
		runtime_snapshot.context_snapshot.project.subtitle_file = kDebugSessionSubtitlePath;
		runtime_snapshot.context_snapshot.project.automation_scripts = "autoload";
		runtime_snapshot.context_snapshot.project.export_filters = "Lua";
		runtime_snapshot.context_snapshot.project.export_encoding = "utf-8";
		runtime_snapshot.context_snapshot.project.style_storage = "Default";
		runtime_snapshot.context_snapshot.project.audio_file = "";
		runtime_snapshot.context_snapshot.project.video_file = "";
		runtime_snapshot.context_snapshot.project.play_res_x = 1280;
		runtime_snapshot.context_snapshot.project.play_res_y = 720;
		runtime_snapshot.context_snapshot.project.info_count = 6;
		runtime_snapshot.context_snapshot.project.style_count = 2;
		runtime_snapshot.context_snapshot.project.event_count = 9;
		runtime_snapshot.context_snapshot.project.dialogue_count = 7;
		runtime_snapshot.context_snapshot.project.comment_count = 2;
		runtime_snapshot.context_snapshot.project.attachment_count = 1;
		runtime_snapshot.context_snapshot.project.extradata_count = 3;
		runtime_snapshot.context_snapshot.project.scroll_position = 7;
		runtime_snapshot.context_snapshot.project.active_row = 18;
		runtime_snapshot.context_snapshot.project.video_position = 42;
		runtime_snapshot.context_snapshot.project.video_zoom = 1.5;
		captured.runtime_snapshot = std::move(runtime_snapshot);

		EXPECT_TRUE(session->HandleHookPause(frame.location, 1, [captured]() mutable {
			return captured;
		}));

		session->EndInvocation();
		{
			std::lock_guard<std::mutex> lock(pause_mutex);
			pause_finished = true;
		}
		pause_cv.notify_all();
	});

	json::Object process_event;
	ASSERT_TRUE(WaitForEvent(*raw_connection, cursor, "process", process_event));
	json::Object thread_event;
	ASSERT_TRUE(WaitForEvent(*raw_connection, cursor, "thread", thread_event));
	EXPECT_EQ(std::optional<std::string>{"started"}, FindString(*FindObject(thread_event, "body").value(), "reason"));

	json::Object stopped_event;
	ASSERT_TRUE(WaitForEvent(*raw_connection, cursor, "stopped", stopped_event, 5s));
	auto stopped_body = FindObject(stopped_event, "body");
	ASSERT_TRUE(stopped_body.has_value());
	EXPECT_EQ(std::optional<std::string>{"breakpoint"}, FindString(**stopped_body, "reason"));
	EXPECT_EQ(std::optional<int>{1}, FindInt(**stopped_body, "threadId"));

	json::Object stack_arguments;
	stack_arguments["threadId"] = static_cast<json::Integer>(1);
	raw_connection->PushRequest(MakeRequest(5, "stackTrace", std::move(stack_arguments)));
	json::Object stack_response;
	ASSERT_TRUE(WaitForResponse(*raw_connection, cursor, 5, "stackTrace", stack_response));
	auto stack_body = FindObject(stack_response, "body");
	ASSERT_TRUE(stack_body.has_value());
	auto stack_frames = FindArray(**stack_body, "stackFrames");
	ASSERT_TRUE(stack_frames.has_value());
	ASSERT_EQ(1u, (**stack_frames).size());

	json::Object scopes_arguments;
	scopes_arguments["frameId"] = static_cast<json::Integer>(1);
	raw_connection->PushRequest(MakeRequest(6, "scopes", std::move(scopes_arguments)));
	json::Object scopes_response;
	ASSERT_TRUE(WaitForResponse(*raw_connection, cursor, 6, "scopes", scopes_response));
	auto scope_names = ExtractScopeNames(scopes_response);
	EXPECT_EQ((std::vector<std::string>{
		"Locals",
		"Captured Variables",
		"Globals",
		"Functions",
		"Runtime Globals",
		"Automation Context"
	}), scope_names);

	auto functions_reference = FindScopeReference(scopes_response, "Functions");
	ASSERT_TRUE(functions_reference.has_value());
	EXPECT_GT(*functions_reference, 0);

	json::Object functions_variables_arguments;
	functions_variables_arguments["variablesReference"] = static_cast<json::Integer>(*functions_reference);
	raw_connection->PushRequest(MakeRequest(7, "variables", std::move(functions_variables_arguments)));
	json::Object functions_variables_response;
	ASSERT_TRUE(WaitForResponse(*raw_connection, cursor, 7, "variables", functions_variables_response));
	auto function_variable_names = ExtractVariableNames(functions_variables_response);
	EXPECT_EQ((std::vector<std::string>{
		"hydradient",
		"linecheck"
	}), function_variable_names);

	auto runtime_globals_reference = FindScopeReference(scopes_response, "Runtime Globals");
	ASSERT_TRUE(runtime_globals_reference.has_value());
	EXPECT_GT(*runtime_globals_reference, 0);

	json::Object runtime_variables_arguments;
	runtime_variables_arguments["variablesReference"] = static_cast<json::Integer>(*runtime_globals_reference);
	raw_connection->PushRequest(MakeRequest(8, "variables", std::move(runtime_variables_arguments)));
	json::Object runtime_variables_response;
	ASSERT_TRUE(WaitForResponse(*raw_connection, cursor, 8, "variables", runtime_variables_response));
	auto runtime_variable_names = ExtractVariableNames(runtime_variables_response);
	EXPECT_EQ(3u, runtime_variable_names.size());
	EXPECT_NE(runtime_variable_names.end(), std::find(runtime_variable_names.begin(), runtime_variable_names.end(), "aegisub"));
	EXPECT_NE(runtime_variable_names.end(), std::find(runtime_variable_names.begin(), runtime_variable_names.end(), "string"));
	EXPECT_NE(runtime_variable_names.end(), std::find(runtime_variable_names.begin(), runtime_variable_names.end(), "print"));
	EXPECT_EQ(runtime_variable_names.end(), std::find(runtime_variable_names.begin(), runtime_variable_names.end(), "_G"));

	auto automation_context_scope_reference = FindScopeReference(scopes_response, "Automation Context");
	ASSERT_TRUE(automation_context_scope_reference.has_value());
	EXPECT_GT(*automation_context_scope_reference, 0);

	json::Object variables_arguments;
	variables_arguments["variablesReference"] = static_cast<json::Integer>(*automation_context_scope_reference);
	raw_connection->PushRequest(MakeRequest(9, "variables", std::move(variables_arguments)));
	json::Object variables_response;
	ASSERT_TRUE(WaitForResponse(*raw_connection, cursor, 9, "variables", variables_response));
	auto variable_names = ExtractVariableNames(variables_response);
	EXPECT_EQ((std::vector<std::string>{
		"Progress",
		"Invocation",
		"Selection",
		"Subtitles",
		"video",
		"audio",
		"files"
	}), variable_names);

	auto subtitles_reference = FindVariableReference(variables_response, "Subtitles");
	ASSERT_TRUE(subtitles_reference.has_value());
	EXPECT_GT(*subtitles_reference, 0);

	json::Object subtitles_variables_arguments;
	subtitles_variables_arguments["variablesReference"] = static_cast<json::Integer>(*subtitles_reference);
	raw_connection->PushRequest(MakeRequest(10, "variables", std::move(subtitles_variables_arguments)));
	json::Object subtitles_variables_response;
	ASSERT_TRUE(WaitForResponse(*raw_connection, cursor, 10, "variables", subtitles_variables_response));
	EXPECT_EQ((std::vector<std::string>{
		"script_filename",
		"subtitle_file",
		"active_row",
		"line_count",
		"info_count",
		"style_count",
		"event_count",
		"dialogue_count",
		"comment_count",
		"attachment_count",
		"extradata_count",
		"play_res_x",
		"play_res_y",
		"automation_scripts",
		"export_filters",
		"export_encoding",
		"style_storage",
		"scroll_position"
	}), ExtractVariableNames(subtitles_variables_response));

	auto audio_reference = FindVariableReference(variables_response, "audio");
	ASSERT_TRUE(audio_reference.has_value());
	EXPECT_GT(*audio_reference, 0);

	json::Object audio_variables_arguments;
	audio_variables_arguments["variablesReference"] = static_cast<json::Integer>(*audio_reference);
	raw_connection->PushRequest(MakeRequest(11, "variables", std::move(audio_variables_arguments)));
	json::Object audio_variables_response;
	ASSERT_TRUE(WaitForResponse(*raw_connection, cursor, 11, "variables", audio_variables_response));
	EXPECT_EQ((std::vector<std::string>{
		"has_audio",
		"has_audio_selection",
		"audio_selection_begin",
		"audio_selection_end"
	}), ExtractVariableNames(audio_variables_response));

	json::Object continue_arguments;
	continue_arguments["threadId"] = static_cast<json::Integer>(1);
	raw_connection->PushRequest(MakeRequest(12, "continue", std::move(continue_arguments)));
	json::Object continue_response;
	ASSERT_TRUE(WaitForResponse(*raw_connection, cursor, 12, "continue", continue_response));
	EXPECT_TRUE(FindBool(continue_response, "success").value_or(false));

	json::Object continued_event;
	ASSERT_TRUE(WaitForEvent(*raw_connection, cursor, "continued", continued_event));

	{
		std::unique_lock<std::mutex> lock(pause_mutex);
		ASSERT_TRUE(pause_cv.wait_for(lock, 5s, [&] { return pause_finished; }));
	}
	pause_thread.join();

	raw_connection->PushRequest(MakeRequest(13, "disconnect"));
	json::Object disconnect_response;
	ASSERT_TRUE(WaitForResponse(*raw_connection, cursor, 13, "disconnect", disconnect_response));
	EXPECT_TRUE(FindBool(disconnect_response, "success").value_or(false));

	adapter_thread.join();
	service.SetEnabled(false);
}

TEST(AutomationDebugAdapter, supports_multiple_sessions_on_single_attach) {
	Automation4::AutomationDebugService service;
	ASSERT_TRUE(service.SetEnabled(true));

	auto connection = std::make_unique<FakeDebugConnection>();
	auto *raw_connection = connection.get();
	Automation4::AutomationDebugAdapter adapter(service, "test-token", std::move(connection));
	std::thread adapter_thread([&] { adapter.Run(); });

	size_t cursor = 0;

	raw_connection->PushRequest(MakeRequest(1, "initialize"));
	json::Object initialize_response;
	ASSERT_TRUE(WaitForResponse(*raw_connection, cursor, 1, "initialize", initialize_response));
	EXPECT_TRUE(FindBool(initialize_response, "success").value_or(false));

	json::Object attach_arguments;
	attach_arguments["token"] = "test-token";
	attach_arguments["stopOnEntry"] = true;
	raw_connection->PushRequest(MakeRequest(2, "attach", std::move(attach_arguments)));
	json::Object attach_response;
	ASSERT_TRUE(WaitForResponse(*raw_connection, cursor, 2, "attach", attach_response));
	EXPECT_TRUE(FindBool(attach_response, "success").value_or(false));

	json::Object initialized_event;
	ASSERT_TRUE(WaitForEvent(*raw_connection, cursor, "initialized", initialized_event));

	raw_connection->PushRequest(MakeRequest(3, "configurationDone"));
	json::Object configuration_response;
	ASSERT_TRUE(WaitForResponse(*raw_connection, cursor, 3, "configurationDone", configuration_response));
	EXPECT_TRUE(FindBool(configuration_response, "success").value_or(false));

	auto run_session = [&](int line, int continue_seq) {
		auto session = service.PrepareSession({
			"Lua",
			agi::fs::path(kDebugSessionScriptPath),
			"Debug smoke"
		});
		ASSERT_TRUE(session);

		auto invocation = Automation4::MakeMacroRunInvocation("Debug smoke");
		session->BeginInvocation(invocation);

		std::mutex pause_mutex;
		std::condition_variable pause_cv;
		bool pause_finished = false;

		std::thread pause_thread([&] {
			Automation4::AutomationDebugFrame frame;
			frame.level = 0;
			frame.kind = "lua";
			frame.function_name = "debug_smoke";
			frame.location = {
				kDebugSessionScriptPath,
				"script",
				kDebugSessionScriptName,
				line,
				1
			};

			Automation4::AutomationDebugCapturedState captured;
			captured.frames.push_back(frame);

			EXPECT_TRUE(session->HandleHookPause(frame.location, 1, [captured]() mutable {
				return captured;
			}));
			session->EndInvocation();
			{
				std::lock_guard<std::mutex> lock(pause_mutex);
				pause_finished = true;
			}
			pause_cv.notify_all();
		});

		json::Object stopped_event;
		ASSERT_TRUE(WaitForEvent(*raw_connection, cursor, "stopped", stopped_event, 5s));
		auto stopped_body = FindObject(stopped_event, "body");
		ASSERT_TRUE(stopped_body.has_value());
		EXPECT_EQ(std::optional<std::string>{"entry"}, FindString(**stopped_body, "reason"));

		json::Object continue_arguments;
		continue_arguments["threadId"] = static_cast<json::Integer>(1);
		raw_connection->PushRequest(MakeRequest(continue_seq, "continue", std::move(continue_arguments)));
		json::Object continue_response;
		ASSERT_TRUE(WaitForResponse(*raw_connection, cursor, continue_seq, "continue", continue_response));
		EXPECT_TRUE(FindBool(continue_response, "success").value_or(false));

		json::Object continued_event;
		ASSERT_TRUE(WaitForEvent(*raw_connection, cursor, "continued", continued_event));

		{
			std::unique_lock<std::mutex> lock(pause_mutex);
			ASSERT_TRUE(pause_cv.wait_for(lock, 5s, [&] { return pause_finished; }));
		}
		pause_thread.join();

		service.ClearSession(session);
		json::Object thread_event;
		ASSERT_TRUE(WaitForEvent(*raw_connection, cursor, "thread", thread_event, 5s));
		auto body = FindObject(thread_event, "body");
		ASSERT_TRUE(body.has_value());
		EXPECT_EQ(std::optional<std::string>{"exited"}, FindString(**body, "reason"));
	};

	run_session(12, 4);
	run_session(18, 5);

	raw_connection->PushRequest(MakeRequest(6, "disconnect"));
	json::Object disconnect_response;
	ASSERT_TRUE(WaitForResponse(*raw_connection, cursor, 6, "disconnect", disconnect_response));
	EXPECT_TRUE(FindBool(disconnect_response, "success").value_or(false));

	adapter_thread.join();
	service.SetEnabled(false);
}

TEST(AutomationDebugService, late_attach_prepares_future_sessions) {
	Automation4::AutomationDebugService service;
	ASSERT_TRUE(service.SetEnabled(true));

	EXPECT_FALSE(service.PrepareSession({
		"Lua",
		agi::fs::path(kDebugSessionScriptPath),
		"Debug smoke"
	}));

	auto connection = std::make_unique<FakeDebugConnection>();
	auto *raw_connection = connection.get();
	Automation4::AutomationDebugAdapter adapter(service, "test-token", std::move(connection));
	std::thread adapter_thread([&] { adapter.Run(); });

	size_t cursor = 0;

	raw_connection->PushRequest(MakeRequest(1, "initialize"));
	json::Object initialize_response;
	ASSERT_TRUE(WaitForResponse(*raw_connection, cursor, 1, "initialize", initialize_response));
	EXPECT_TRUE(FindBool(initialize_response, "success").value_or(false));

	json::Object attach_arguments;
	attach_arguments["token"] = "test-token";
	raw_connection->PushRequest(MakeRequest(2, "attach", std::move(attach_arguments)));
	json::Object attach_response;
	ASSERT_TRUE(WaitForResponse(*raw_connection, cursor, 2, "attach", attach_response));
	EXPECT_TRUE(FindBool(attach_response, "success").value_or(false));

	json::Object initialized_event;
	ASSERT_TRUE(WaitForEvent(*raw_connection, cursor, "initialized", initialized_event));

	raw_connection->PushRequest(MakeRequest(3, "configurationDone"));
	json::Object configuration_response;
	ASSERT_TRUE(WaitForResponse(*raw_connection, cursor, 3, "configurationDone", configuration_response));
	EXPECT_TRUE(FindBool(configuration_response, "success").value_or(false));

	auto session = service.PrepareSession({
		"Lua",
		agi::fs::path(kDebugSessionScriptPath),
		"Debug smoke"
	});
	ASSERT_TRUE(session);
	session->Detach();
	service.ClearSession(session);

	raw_connection->PushRequest(MakeRequest(4, "disconnect"));
	json::Object disconnect_response;
	ASSERT_TRUE(WaitForResponse(*raw_connection, cursor, 4, "disconnect", disconnect_response));
	EXPECT_TRUE(FindBool(disconnect_response, "success").value_or(false));

	adapter_thread.join();
	service.SetEnabled(false);
}

TEST(AutomationDebugService, disable_without_client_returns_promptly) {
	Automation4::AutomationDebugService service;
	ASSERT_TRUE(service.SetEnabled(true));

	auto endpoint = service.GetEndpoint();
	EXPECT_TRUE(endpoint.available);
	EXPECT_GT(endpoint.port, 0);
	EXPECT_FALSE(endpoint.token.empty());

	EXPECT_FALSE(service.SetEnabled(false));
	EXPECT_FALSE(service.IsEnabled());
}

TEST(AutomationDebugService, missing_debug_options_use_listener_defaults) {
	ScopedReplaceDebugServiceOptions options(kDebugServicePartialOptionConfig);

	Automation4::AutomationDebugService service;
	ASSERT_TRUE(service.SetEnabled(true));

	auto endpoint = service.GetEndpoint();
	EXPECT_TRUE(endpoint.available);
	EXPECT_EQ("127.0.0.1", endpoint.host);
	EXPECT_GT(endpoint.port, 0);
	EXPECT_FALSE(endpoint.token.empty());

	EXPECT_FALSE(service.SetEnabled(false));
	EXPECT_FALSE(service.IsEnabled());
}

TEST(AutomationDebugService, honors_configured_port_and_optional_token) {
	ScopedDebugServiceOptions options;
	ScopedIntOption port_option("Automation/Debug/Listen Port");
	ScopedBoolOption require_token_option("Automation/Debug/Require Token");
	ScopedStringOption token_option("Automation/Debug/Token");

	int const port = FindFreeLoopbackPort();
	ASSERT_GT(port, 0);
	OPT_SET("Automation/Debug/Listen Port")->SetInt(port);
	OPT_SET("Automation/Debug/Require Token")->SetBool(false);
	OPT_SET("Automation/Debug/Token")->SetString("ignored-when-auth-disabled");

	Automation4::AutomationDebugService service;
	ASSERT_TRUE(service.SetEnabled(true));

	auto endpoint = service.GetEndpoint();
	EXPECT_TRUE(endpoint.available);
	EXPECT_EQ(port, endpoint.port);
	EXPECT_TRUE(endpoint.token.empty());

	EXPECT_FALSE(service.SetEnabled(false));
	EXPECT_FALSE(service.IsEnabled());
}

TEST(AutomationDebugService, honors_configured_token_when_required) {
	ScopedDebugServiceOptions options;
	ScopedIntOption port_option("Automation/Debug/Listen Port");
	ScopedBoolOption require_token_option("Automation/Debug/Require Token");
	ScopedStringOption token_option("Automation/Debug/Token");

	OPT_SET("Automation/Debug/Listen Port")->SetInt(0);
	OPT_SET("Automation/Debug/Require Token")->SetBool(true);
	OPT_SET("Automation/Debug/Token")->SetString("fixed-debug-token");

	Automation4::AutomationDebugService service;
	ASSERT_TRUE(service.SetEnabled(true));

	auto endpoint = service.GetEndpoint();
	EXPECT_TRUE(endpoint.available);
	EXPECT_GT(endpoint.port, 0);
	EXPECT_EQ("fixed-debug-token", endpoint.token);

	EXPECT_FALSE(service.SetEnabled(false));
	EXPECT_FALSE(service.IsEnabled());
}
}
