// Copyright (c) 2010, Amar Takhar
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the name of the Aegisub Group nor the names of its contributors
//     may be used to endorse or promote products derived from this software
//     without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// Aegisub Project http://www.aegisub.org/

#include "compat.h"
#include "dialog_manager.h"
#include "format.h"
#include "include/aegisub/context.h"
#include "ui_dispatch.h"

#include <libaegisub/cajun/reader.h>
#include <libaegisub/io.h>
#include <libaegisub/log.h>

#include <algorithm>
#include <ctime>
#include <deque>
#include <functional>
#include <iterator>
#include <memory>
#include <sstream>
#include <unordered_set>
#include <vector>

#include <wx/button.h>
#include <wx/choice.h>
#include <wx/dialog.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

namespace {
constexpr size_t kMaxLoadedHistoryEntries = 4000;

wxString format_log_message(agi::log::SinkMessage const& sm) {
	time_t time = sm.time / 1000000000;
#ifndef _WIN32
	tm tmtime;
	localtime_r(&time, &tmtime);
#ifdef LOG_WITH_FILE
	return fmt_wx("%c %02d:%02d:%02d %-6d <%-25s> [%s:%s:%d]  %s\n",
		agi::log::Severity_ID[sm.severity],
		tmtime.tm_hour,
		tmtime.tm_min,
		tmtime.tm_sec,
		(sm.time % 1000000000),
		sm.section,
		sm.file,
		sm.func,
		sm.line,
		sm.message);
#else
	return fmt_wx("%c %02d:%02d:%02d %-6d <%-25s> [%s:%d]  %s\n",
		agi::log::Severity_ID[sm.severity],
		tmtime.tm_hour,
		tmtime.tm_min,
		tmtime.tm_sec,
		(sm.time % 1000000000),
		sm.section,
		sm.func,
		sm.line,
		sm.message);
#endif
#else
#ifdef LOG_WITH_FILE
	return fmt_wx("%c %-6ld.%09ld <%-25s> [%s:%s:%d]  %s\n",
		agi::log::Severity_ID[sm.severity],
		(sm.time / 1000000000),
		(sm.time % 1000000000),
		sm.section,
		sm.file,
		sm.func,
		sm.line,
		sm.message);
#else
	return fmt_wx("%c %-6ld.%09ld <%-25s> [%s:%d]  %s\n",
		agi::log::Severity_ID[sm.severity],
		(sm.time / 1000000000),
		(sm.time % 1000000000),
		sm.section,
		sm.func,
		sm.line,
		sm.message);
#endif
#endif
}

struct LogEntry {
	agi::log::Severity severity = agi::log::Debug;
	int64_t time = 0;
	wxString formatted;
	wxString searchable;
};

struct ParsedLogRecord {
	agi::log::Severity severity = agi::log::Debug;
	int64_t time = 0;
	int line = 0;
	std::string section;
#ifdef LOG_WITH_FILE
	std::string file;
#endif
	std::string func;
	std::string message;
};

LogEntry make_entry(agi::log::SinkMessage const& sm) {
	auto formatted = format_log_message(sm);
	return { sm.severity, sm.time, formatted, formatted.Lower() };
}

LogEntry make_entry(ParsedLogRecord const& record) {
	agi::log::SinkMessage sm;
	sm.time = record.time;
	sm.severity = record.severity;
	sm.section = record.section.c_str();
#ifdef LOG_WITH_FILE
	sm.file = record.file.c_str();
#endif
	sm.func = record.func.c_str();
	sm.line = record.line;
	sm.message = record.message;
	return make_entry(sm);
}

std::string make_message_key(agi::log::SinkMessage const& sm) {
	return std::to_string(sm.time)
		+ "|"
		+ std::to_string(static_cast<int>(sm.severity))
		+ "|"
		+ (sm.section ? sm.section : "")
		+ "|"
#ifdef LOG_WITH_FILE
		+ (sm.file ? sm.file : "")
		+ "|"
#endif
		+ (sm.func ? sm.func : "")
		+ "|"
		+ std::to_string(sm.line)
		+ "|"
		+ sm.message;
}

std::string make_message_key(ParsedLogRecord const& record) {
	return std::to_string(record.time)
		+ "|"
		+ std::to_string(static_cast<int>(record.severity))
		+ "|"
		+ record.section
		+ "|"
#ifdef LOG_WITH_FILE
		+ record.file
		+ "|"
#endif
		+ record.func
		+ "|"
		+ std::to_string(record.line)
		+ "|"
		+ record.message;
}

bool parse_log_object(json::Object const& obj, ParsedLogRecord& record) {
	auto get_integer = [&](char const* key, int64_t& out) -> bool {
		auto it = obj.find(key);
		if (it == obj.end())
			return false;
		try {
			out = static_cast<json::Integer const&>(it->second);
			return true;
		}
		catch (json::Exception const&) {
			return false;
		}
	};

	auto get_string = [&](char const* key, std::string& out) -> bool {
		auto it = obj.find(key);
		if (it == obj.end())
			return false;
		try {
			out = static_cast<json::String const&>(it->second);
			return true;
		}
		catch (json::Exception const&) {
			return false;
		}
	};

	auto get_optional_string = [&](char const* key, std::string& out) {
		auto it = obj.find(key);
		if (it == obj.end())
			return;
		try {
			out = static_cast<json::String const&>(it->second);
		}
		catch (json::Exception const&) {
		}
	};

	int64_t sec = 0;
	int64_t usec = 0;
	int64_t severity = 0;
	int64_t line = 0;
	std::string section;
#ifdef LOG_WITH_FILE
	std::string file;
#endif
	std::string func;
	std::string message;
	if (!get_integer("sec", sec)
		|| !get_integer("usec", usec)
		|| !get_integer("severity", severity)
		|| !get_integer("line", line)
		|| !get_string("section", section)
		|| !get_string("func", func)
		|| !get_string("message", message)) {
		return false;
	}

#ifdef LOG_WITH_FILE
	get_optional_string("file", file);
#endif

	if (severity < agi::log::Exception || severity > agi::log::Debug)
		return false;

	record.time = sec * 1000000000 + usec;
	record.severity = static_cast<agi::log::Severity>(severity);
	record.section = std::move(section);
#ifdef LOG_WITH_FILE
	record.file = std::move(file);
#endif
	record.func = std::move(func);
	record.line = static_cast<int>(line);
	record.message = std::move(message);
	return true;
}

bool parse_log_object_string(std::string const& text, ParsedLogRecord& record) {
	try {
		std::istringstream stream(text);
		json::UnknownElement root;
		json::Reader::Read(root, stream);
		return parse_log_object(static_cast<json::Object const&>(root), record);
	}
	catch (json::Exception const&) {
		return false;
	}
}

std::vector<std::string> split_legacy_json_objects(std::string const& contents) {
	std::vector<std::string> objects;
	size_t object_begin = std::string::npos;
	int depth = 0;
	bool in_string = false;
	bool escaping = false;

	for (size_t i = 0; i < contents.size(); ++i) {
		char const ch = contents[i];
		if (object_begin == std::string::npos) {
			if (ch == '{') {
				object_begin = i;
				depth = 1;
				in_string = false;
				escaping = false;
			}
			continue;
		}

		if (in_string) {
			if (escaping) {
				escaping = false;
			}
			else if (ch == '\\') {
				escaping = true;
			}
			else if (ch == '"') {
				in_string = false;
			}
			continue;
		}

		if (ch == '"') {
			in_string = true;
		}
		else if (ch == '{') {
			++depth;
		}
		else if (ch == '}') {
			--depth;
			if (depth == 0) {
				objects.emplace_back(contents.substr(object_begin, i - object_begin + 1));
				object_begin = std::string::npos;
			}
		}
	}

	return objects;
}

void append_tail_message(std::deque<ParsedLogRecord>& messages, ParsedLogRecord sm) {
	if (messages.size() >= kMaxLoadedHistoryEntries)
		messages.pop_front();
	messages.emplace_back(std::move(sm));
}

void load_log_messages_from_file(agi::fs::path const& path, std::deque<ParsedLogRecord>& messages) {
	std::unique_ptr<std::istream> stream;
	try {
		stream = agi::io::Open(path);
	}
	catch (agi::Exception const&) {
		return;
	}
	if (!stream)
		return;

	if (path.extension() == ".ndjson") {
		for (std::string line; std::getline(*stream, line); ) {
			if (line.empty())
				continue;
			ParsedLogRecord sm;
			if (parse_log_object_string(line, sm))
				append_tail_message(messages, std::move(sm));
		}
		return;
	}

	std::string contents((std::istreambuf_iterator<char>(*stream)), std::istreambuf_iterator<char>());
	for (auto const& object_text : split_legacy_json_objects(contents)) {
		ParsedLogRecord sm;
		if (parse_log_object_string(object_text, sm))
			append_tail_message(messages, std::move(sm));
	}
}

std::vector<ParsedLogRecord> load_log_file_history() {
	auto const path = agi::log::GetSessionLogFile();
	if (path.empty())
		return {};

	std::deque<ParsedLogRecord> messages;
	load_log_messages_from_file(path, messages);
	return { messages.begin(), messages.end() };
}

enum class SearchMode {
	Filter,
	Find
};

class EmitLog final : public agi::log::Emitter {
	std::function<void(agi::log::SinkMessage const&)> append_message;
	agi::ui::WeakLifetime lifetime;

public:
	EmitLog(std::function<void(agi::log::SinkMessage const&)> append_message, agi::ui::WeakLifetime lifetime)
	: append_message(std::move(append_message))
	, lifetime(std::move(lifetime))
	{
	}

	void log(agi::log::SinkMessage const& sm) override {
		if (agi::ui::CheckAccess()) {
			if (!lifetime.lock())
				return;
			append_message(sm);
			return;
		}

		auto append = append_message;
		agi::ui::MainAsyncIfAlive(lifetime, [append, sm] {
			append(sm);
		});
	}
};

class LogWindow : public wxDialog {
	agi::log::Emitter *emit_log = nullptr;
	wxChoice *level_choice = nullptr;
	wxChoice *search_mode_choice = nullptr;
	wxTextCtrl *search_ctrl = nullptr;
	wxTextCtrl *text_ctrl = nullptr;
	wxStaticText *status_text = nullptr;
	std::vector<LogEntry> log_entries;
	std::vector<long> match_positions;
	std::unordered_set<std::string> seen_message_keys;
	wxTextAttr match_text_style;
	wxTextAttr active_match_text_style;
	size_t visible_entries = 0;
	size_t active_match = 0;
	agi::ui::UiActivationScope ui_activation;

	void AddMessage(agi::log::SinkMessage const& sm);
	bool AddMessageIfNew(agi::log::SinkMessage const& sm);
	bool AddMessageIfNew(ParsedLogRecord const& record);
	void AppendVisibleEntry(LogEntry const& entry);
	SearchMode GetSearchMode() const;
	wxString GetSearchText() const;
	bool MatchesLevelFilter(LogEntry const& entry) const;
	bool MatchesDisplayFilters(LogEntry const& entry, wxString const& search_text) const;
	void AppendMatchPositions(wxString const& searchable, wxString const& search_text, long base_offset);
	void RebuildMatches(wxString const& contents, wxString const& search_text);
	void ApplyHighlights();
	void SelectActiveMatch();
	void AdvanceMatch(int step);
	void RefreshView();
	void UpdateStatus();

public:
	LogWindow(agi::Context *c);
	~LogWindow();
	agi::ui::WeakLifetime GetAsyncUiLifetime() const { return ui_activation.GetLifetime(); }
};

LogWindow::LogWindow(agi::Context *c)
: wxDialog(c->parent, -1, _("Log window"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX | wxRESIZE_BORDER)
{
	const wxString level_labels[] = {
		_("All"),
		_("Debug"),
		_("Info"),
		_("Warning"),
		_("Assert"),
		_("Exception")
	};
	level_choice = new wxChoice(this, -1, wxDefaultPosition, wxDefaultSize, 6, level_labels);
	level_choice->SetSelection(0);

	const wxString search_mode_labels[] = {
		_("Filter"),
		_("Find")
	};
	search_mode_choice = new wxChoice(this, -1, wxDefaultPosition, wxDefaultSize, 2, search_mode_labels);
	search_mode_choice->SetSelection(1);

	search_ctrl = new wxTextCtrl(this, -1, "", wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);

	auto filters = new wxBoxSizer(wxHORIZONTAL);
	filters->Add(new wxStaticText(this, -1, _("Level:")), wxSizerFlags().Center().Border(wxRIGHT));
	filters->Add(level_choice, wxSizerFlags().Center().Border(wxRIGHT));
	filters->Add(new wxStaticText(this, -1, _("Search:")), wxSizerFlags().Center().Border(wxRIGHT));
	filters->Add(search_ctrl, wxSizerFlags(1).Expand().Border(wxRIGHT));
	filters->Add(new wxStaticText(this, -1, _("Mode:")), wxSizerFlags().Center().Border(wxRIGHT));
	filters->Add(search_mode_choice, wxSizerFlags().Center());

	long text_style = wxTE_MULTILINE | wxTE_READONLY;
#ifdef __WXMSW__
	text_style |= wxTE_RICH2;
#endif
	text_ctrl = new wxTextCtrl(this, -1, "", wxDefaultPosition, FromDIP(wxSize(700, 320)), text_style);

	auto mono_font = wxFont(8, wxFONTFAMILY_MODERN, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL);
	match_text_style = wxTextAttr(text_ctrl->GetForegroundColour(), wxColour(255, 245, 157), mono_font);
	active_match_text_style = wxTextAttr(text_ctrl->GetForegroundColour(), wxColour(255, 204, 128), mono_font);
	text_ctrl->SetFont(mono_font);
	text_ctrl->SetDefaultStyle(wxTextAttr(text_ctrl->GetForegroundColour(), text_ctrl->GetBackgroundColour(), mono_font));

	status_text = new wxStaticText(this, -1, wxEmptyString);
	auto bottom = new wxBoxSizer(wxHORIZONTAL);
	bottom->Add(status_text, wxSizerFlags(1).Center().Border());
	bottom->Add(new wxButton(this, wxID_OK), wxSizerFlags().Border());

	auto sizer = new wxBoxSizer(wxVERTICAL);
	sizer->Add(filters, wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxTOP));
	sizer->Add(text_ctrl, wxSizerFlags(1).Expand().Border());
	sizer->Add(bottom, wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM));
	SetSizerAndFit(sizer);

	level_choice->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { RefreshView(); });
	search_mode_choice->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { RefreshView(); });
	search_ctrl->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { RefreshView(); });
	search_ctrl->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) {
		if (GetSearchMode() == SearchMode::Find)
			AdvanceMatch(1);
	});

	for (auto const& sm : load_log_file_history())
		AddMessageIfNew(sm);
	for (auto const& sm : agi::log::log->GetMessages())
		AddMessageIfNew(sm);
	RefreshView();

	agi::log::log->Subscribe(std::unique_ptr<agi::log::Emitter>(emit_log = new EmitLog([this](agi::log::SinkMessage const& sm) {
		AddMessage(sm);
	}, GetAsyncUiLifetime())));
}

LogWindow::~LogWindow() {
	ui_activation.Deactivate();
	agi::log::log->Unsubscribe(emit_log);
}

bool LogWindow::AddMessageIfNew(agi::log::SinkMessage const& sm) {
	auto [_, inserted] = seen_message_keys.insert(make_message_key(sm));
	if (!inserted)
		return false;

	log_entries.push_back(make_entry(sm));
	return true;
}

bool LogWindow::AddMessageIfNew(ParsedLogRecord const& record) {
	auto [_, inserted] = seen_message_keys.insert(make_message_key(record));
	if (!inserted)
		return false;

	log_entries.push_back(make_entry(record));
	return true;
}

void LogWindow::AddMessage(agi::log::SinkMessage const& sm) {
	if (!AddMessageIfNew(sm))
		return;

	auto const& entry = log_entries.back();

	if (!MatchesLevelFilter(entry)) {
		UpdateStatus();
		return;
	}

	auto const search_text = GetSearchText();

	if (GetSearchMode() == SearchMode::Find) {
		if (search_text.empty()) {
			AppendVisibleEntry(entry);
		}
		else {
			auto match_count = match_positions.size();
			auto base_offset = text_ctrl->GetLastPosition();

			AppendVisibleEntry(entry);
			AppendMatchPositions(entry.searchable, search_text, base_offset);

			if (match_positions.size() != match_count) {
				ApplyHighlights();
				if (match_count == 0)
					SelectActiveMatch();
			}
		}

		UpdateStatus();
		return;
	}

	if (search_text.empty() || entry.searchable.Contains(search_text))
		AppendVisibleEntry(entry);

	UpdateStatus();
}

void LogWindow::AppendVisibleEntry(LogEntry const& entry) {
	text_ctrl->AppendText(entry.formatted);
	++visible_entries;
}

SearchMode LogWindow::GetSearchMode() const {
	return search_mode_choice->GetSelection() == 1 ? SearchMode::Find : SearchMode::Filter;
}

wxString LogWindow::GetSearchText() const {
	return search_ctrl->GetValue().Lower();
}

bool LogWindow::MatchesLevelFilter(LogEntry const& entry) const {
	auto selection = level_choice->GetSelection();
	if (selection <= 0)
		return true;

	static const agi::log::Severity thresholds[] = {
		agi::log::Debug,
		agi::log::Info,
		agi::log::Warning,
		agi::log::Assert,
		agi::log::Exception
	};

	auto threshold = thresholds[selection - 1];
	return static_cast<int>(entry.severity) <= static_cast<int>(threshold);
}

bool LogWindow::MatchesDisplayFilters(LogEntry const& entry, wxString const& search_text) const {
	if (!MatchesLevelFilter(entry))
		return false;

	if (GetSearchMode() == SearchMode::Find)
		return true;

	return search_text.empty() || entry.searchable.Contains(search_text);
}

void LogWindow::AppendMatchPositions(wxString const& searchable, wxString const& search_text, long base_offset) {
	if (search_text.empty())
		return;

	auto step = std::max<long>(1, search_text.length());
	long from = 0;

	while (from <= static_cast<long>(searchable.length())) {
		auto found = searchable.find(search_text, from);
		if (found == wxNOT_FOUND)
			break;

		match_positions.push_back(base_offset + found);
		from = found + step;
	}
}

void LogWindow::RebuildMatches(wxString const& contents, wxString const& search_text) {
	match_positions.clear();
	active_match = 0;

	if (GetSearchMode() != SearchMode::Find || search_text.empty())
		return;

	AppendMatchPositions(contents.Lower(), search_text, 0);
}

void LogWindow::ApplyHighlights() {
	if (GetSearchMode() != SearchMode::Find || match_positions.empty())
		return;

	auto search_length = static_cast<long>(search_ctrl->GetValue().length());
	if (search_length <= 0)
		return;

	for (auto pos : match_positions)
		text_ctrl->SetStyle(pos, pos + search_length, match_text_style);

	if (GetSearchMode() == SearchMode::Find && !match_positions.empty()) {
		auto start = match_positions[active_match];
		text_ctrl->SetStyle(start, start + search_length, active_match_text_style);
	}
}

void LogWindow::SelectActiveMatch() {
	if (GetSearchMode() != SearchMode::Find || match_positions.empty()) {
		auto end = text_ctrl->GetLastPosition();
		text_ctrl->SetSelection(end, end);
		return;
	}

	auto search_length = static_cast<long>(search_ctrl->GetValue().length());
	auto start = match_positions[active_match];
	text_ctrl->ShowPosition(start);
	text_ctrl->SetSelection(start, start + search_length);
}

void LogWindow::AdvanceMatch(int step) {
	if (match_positions.empty())
		return;

	auto count = static_cast<int>(match_positions.size());
	auto index = static_cast<int>(active_match);
	index = (index + step) % count;
	if (index < 0)
		index += count;
	active_match = static_cast<size_t>(index);

	SelectActiveMatch();
	UpdateStatus();
}

void LogWindow::RefreshView() {
	wxString contents;
	visible_entries = 0;
	auto const search_text = GetSearchText();

	for (auto const& entry : log_entries) {
		if (!MatchesDisplayFilters(entry, search_text))
			continue;

		contents += entry.formatted;
		++visible_entries;
	}

	text_ctrl->Freeze();
	text_ctrl->ChangeValue(contents);
	if (!contents.empty())
		text_ctrl->SetStyle(0, text_ctrl->GetLastPosition(), wxTextAttr(text_ctrl->GetForegroundColour(), text_ctrl->GetBackgroundColour(), text_ctrl->GetFont()));
	RebuildMatches(contents, search_text);
	ApplyHighlights();
	if (GetSearchMode() == SearchMode::Filter && !contents.empty())
		text_ctrl->ShowPosition(text_ctrl->GetLastPosition());
	text_ctrl->Thaw();

	SelectActiveMatch();
	UpdateStatus();
}

void LogWindow::UpdateStatus() {
	auto label = fmt_tl("Showing %d of %d log entries", static_cast<int>(visible_entries), static_cast<int>(log_entries.size()));

	if (GetSearchMode() == SearchMode::Find && !search_ctrl->GetValue().empty()) {
		if (match_positions.empty())
			label += _(" | 0 matches");
		else
			label += fmt_tl(" | Match %d of %d", static_cast<int>(active_match + 1), static_cast<int>(match_positions.size()));
	}

	status_text->SetLabel(label);
}
}

void ShowLogWindow(agi::Context *c) {
	c->dialog->Show<LogWindow>(c);
}
