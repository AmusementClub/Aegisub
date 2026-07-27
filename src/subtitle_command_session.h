#pragma once

#include "ass_file.h"

#include <libaegisub/scope_exit.h>

#include <string>
#include <utility>

namespace aegisub {

enum class LocalCommitFeedback {
	SkipSelf,
	ObserveSelf,
};

class SubtitleCommandSession {
	AssFile *ass = nullptr;
	int commit_id = -1;
	int local_commit_depth = 0;
	LocalCommitFeedback current_feedback = LocalCommitFeedback::SkipSelf;

public:
	explicit SubtitleCommandSession(AssFile *ass)
	: ass(ass) {
	}

	bool IsLocalCommitInProgress() const noexcept { return local_commit_depth > 0; }
	bool ShouldObserveLocalCommit() const noexcept { return current_feedback == LocalCommitFeedback::ObserveSelf; }
	int GetCommitId() const noexcept { return commit_id; }
	void ResetCommitId() noexcept { commit_id = -1; }

	int CommitWithFeedback(
		std::string const& description,
		int type,
		int amend_id,
		AssDialogue *single_line,
		LocalCommitFeedback feedback) {
		++local_commit_depth;
		auto const previous_feedback = current_feedback;
		current_feedback = feedback;
		auto reset_local_commit_depth = agi::make_scope_exit([&] {
			current_feedback = previous_feedback;
			--local_commit_depth;
		});
		commit_id = ass->Commit(description, type, amend_id, single_line);
		return commit_id;
	}

	int CommitWithFeedback(
		std::string const& description,
		int type,
		int amend_id,
		AssDialogue *single_line,
		AssDialogueCommitSpan changed_lines,
		LocalCommitFeedback feedback) {
		++local_commit_depth;
		auto const previous_feedback = current_feedback;
		current_feedback = feedback;
		auto reset_local_commit_depth = agi::make_scope_exit([&] {
			current_feedback = previous_feedback;
			--local_commit_depth;
		});
		commit_id = ass->Commit(description, type, amend_id, single_line, changed_lines);
		return commit_id;
	}

	int Commit(std::string const& description, int type, int amend_id = -1, AssDialogue *single_line = nullptr) {
		return CommitWithFeedback(description, type, amend_id, single_line, LocalCommitFeedback::SkipSelf);
	}

	int Commit(std::string const& description, int type, int amend_id, AssDialogue *single_line,
		AssDialogueCommitSpan changed_lines) {
		return CommitWithFeedback(description, type, amend_id, single_line, changed_lines, LocalCommitFeedback::SkipSelf);
	}

	template <typename Mutator>
	int RunWithFeedback(
		std::string const& description,
		int type,
		int amend_id,
		AssDialogue *single_line,
		LocalCommitFeedback feedback,
		Mutator&& mutator) {
		std::forward<Mutator>(mutator)();
		return CommitWithFeedback(description, type, amend_id, single_line, feedback);
	}

	template <typename Mutator>
	int Run(std::string const& description, int type, int amend_id, AssDialogue *single_line, Mutator&& mutator) {
		return RunWithFeedback(description, type, amend_id, single_line, LocalCommitFeedback::SkipSelf, std::forward<Mutator>(mutator));
	}
};

}
