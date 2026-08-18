// Copyright (c) 2022, arch1t3cht <arch1t3cht@gmail.com>
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

// Inverse Perspective decomposition adapted from arch1t3cht/Aegisub commits
// f44883844f3179fb2f52b937854b1e6f2e014bee,
// 438aa6882b41f5488e31f3f4af31b324507e900b, and
// 947af58ad98e36dee47c6fbb88d89ae271cfa517.

#pragma once

#include "perspective_forward.h"

#include <cstddef>
#include <optional>
#include <string>

namespace perspective {

struct OutputCoordinateMapping {
	double scale_x = 1.0;
	double scale_y = 1.0;
};

enum class CandidateFamily {
	NoOp,
	CurrentRepresentation,
	Translation,
	Similarity,
	AffineFax,
	AffineFay,
	ProjectiveImplicitFax,
	ProjectiveImplicitFay,
	ProjectiveExplicitOrigin,
};

struct SerializedTransformState {
	std::string position;
	std::optional<std::string> origin;
	std::string scale_x;
	std::string scale_y;
	std::string shear_x;
	std::string shear_y;
	std::string rotation_x;
	std::string rotation_y;
	std::string rotation_z;
};

struct CandidateScore {
	int changed_tag_count = 0;
	int explicit_origin_penalty = 0;
	int perspective_penalty = 0;
	std::size_t token_count = 0;
	double condition_penalty = 0.0;
	int family_rank = 0;
	double residual = 0.0;
};

struct SolverCandidate {
	CandidateFamily family = CandidateFamily::NoOp;
	EvaluatedTransformState state;
	SerializedTransformState serialized;
	CandidateScore score;
	double max_error = 0.0;
};

struct SolverInput {
	ForwardInput source;
	Quad target;
	OutputCoordinateMapping output_mapping;
	double max_error = 0.1;
};

enum class ResidualError {
	None,
	InvalidCandidate,
	InvalidTarget,
	InvalidOutputMapping,
	ProjectionDomain,
};

struct ResidualResult {
	ResidualError error = ResidualError::None;
	GeometryError geometry_error = GeometryError::None;
	ForwardError forward_error = ForwardError::None;
	double max_error = 0.0;

	explicit operator bool() const { return error == ResidualError::None; }
};

enum class SolverError {
	None,
	InvalidSource,
	InvalidTarget,
	InvalidOutputMapping,
	InvalidErrorBudget,
	NoFeasibleCandidate,
};

struct SolverResult {
	SolverError error = SolverError::None;
	GeometryError geometry_error = GeometryError::None;
	ForwardError forward_error = ForwardError::None;
	std::optional<SolverCandidate> candidate;
	std::size_t considered_candidates = 0;

	explicit operator bool() const { return error == SolverError::None && candidate.has_value(); }
};

[[nodiscard]] char const* DescribeSolverError(SolverError error);
[[nodiscard]] char const* DescribeResidualError(ResidualError error);
[[nodiscard]] std::string FormatAssNumber(double value, int maximum_decimals);
[[nodiscard]] std::string FormatAssPoint(Vec2 point, int maximum_decimals);
// Measures a re-evaluated candidate against a target quad in final output
// pixels. The candidate's bounds supply all source-space residual samples.
[[nodiscard]] ResidualResult MeasurePerspectiveResidual(
	ForwardInput const& candidate,
	Quad const& target,
	OutputCoordinateMapping output_mapping);
[[nodiscard]] SolverResult SolvePerspectiveTags(SolverInput const& input);

}
