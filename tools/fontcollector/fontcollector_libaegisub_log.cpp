// Copyright (c) 2026, MIRIMIRIM

#include <libaegisub/log.h>
#include <libaegisub/dispatch.h>

#include <algorithm>
#include <chrono>
#include <iostream>

namespace agi { namespace log {

LogSink *log = nullptr;
const char *Severity_ID = "EAWID";

LogSink::LogSink() = default;
LogSink::~LogSink() = default;

void LogSink::Log(SinkMessage const& sm) {
	if (messages.size() < 250)
		messages.push_back(sm);
	else {
		messages[next_idx] = sm;
		if (++next_idx == 250)
			next_idx = 0;
	}

	for (auto& emitter : emitters)
		emitter->log(sm);
}

void LogSink::Subscribe(std::unique_ptr<Emitter> em) {
	emitters.push_back(std::move(em));
}

void LogSink::Unsubscribe(Emitter *em) {
	emitters.erase(
		std::remove_if(emitters.begin(), emitters.end(), [=](std::unique_ptr<Emitter> const& ptr) { return ptr.get() == em; }),
		emitters.end());
}

std::vector<SinkMessage> LogSink::GetMessages() const {
	std::vector<SinkMessage> ret;
	ret.reserve(messages.size());
	ret.insert(ret.end(), messages.begin() + next_idx, messages.end());
	ret.insert(ret.end(), messages.begin(), messages.begin() + next_idx);
	return ret;
}

#ifdef LOG_WITH_FILE
Message::Message(const char *section, Severity severity, const char *file, const char *func, int line)
#else
Message::Message(const char *section, Severity severity, const char *func, int line)
#endif
: msg(buffer, sizeof buffer)
{
	using namespace std::chrono;
	sm.section = section;
	sm.severity = severity;
#ifdef LOG_WITH_FILE
	sm.file = file;
#endif
	sm.func = func;
	sm.line = line;
	sm.time = duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

Message::~Message() {
	sm.message = std::string(buffer, static_cast<std::string::size_type>(msg.tellp()));
	if (agi::log::log)
		agi::log::log->Log(sm);
}

JsonEmitter::JsonEmitter(fs::path const&) {
}

JsonEmitter::~JsonEmitter() {
}

void JsonEmitter::log(SinkMessage const&) {
}

void JsonEmitter::Flush() {
}

fs::path GetSessionLogFile() {
	return {};
}

void EmitSTDOUT::log(SinkMessage const& sm) {
	std::cout << Severity_ID[sm.severity] << " [" << (sm.section ? sm.section : "") << "] "
		<< sm.message << "\n";
}

} }
