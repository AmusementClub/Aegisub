#include "skia_video_overlay_command_buffer.h"

#include <algorithm>
#include <cmath>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {
struct SetLineColourCommand {
	wxColour colour;
	float alpha = 1.0f;
	int width = 1;
};

struct SetFillColourCommand {
	wxColour colour;
	float alpha = 1.0f;
};

struct SetInvertCommand {
	bool enabled = false;
};

struct DrawLineCommand {
	Vector2D p1;
	Vector2D p2;
};

struct DrawLinesCommand {
	std::size_t dim = 0;
	std::size_t count = 0;
	std::vector<float> values;
};

struct DrawLineStripCommand {
	std::vector<Vector2D> points;
};

struct DrawRectangleCommand {
	Vector2D p1;
	Vector2D p2;
};

struct DrawPolygonCommand {
	std::vector<Vector2D> points;
};

struct DrawMultiPolygonCommand {
	std::vector<float> points;
	std::vector<int> start;
	std::vector<int> count;
	Vector2D video_pos;
	Vector2D video_size;
	bool invert_fill = false;
};

struct DrawCircleCommand {
	Vector2D center;
	float radius = 0.0f;
};

struct DrawTriangleCommand {
	Vector2D p1;
	Vector2D p2;
	Vector2D p3;
};

struct DrawTextCommand {
	std::string text;
	int x = 0;
	int y = 0;
	VideoOverlayTextStyle style;
	wxSize measured;
};

using Command = std::variant<
	SetLineColourCommand,
	SetFillColourCommand,
	SetInvertCommand,
	DrawLineCommand,
	DrawLinesCommand,
	DrawLineStripCommand,
	DrawRectangleCommand,
	DrawPolygonCommand,
	DrawMultiPolygonCommand,
	DrawCircleCommand,
	DrawTriangleCommand,
	DrawTextCommand>;

bool SameColour(wxColour const& left, wxColour const& right) noexcept {
	return left.Red() == right.Red()
		&& left.Green() == right.Green()
		&& left.Blue() == right.Blue()
		&& left.Alpha() == right.Alpha();
}

bool SamePoint(Vector2D left, Vector2D right) noexcept {
	return left.X() == right.X() && left.Y() == right.Y();
}

bool SamePoints(std::vector<Vector2D> const& left, std::vector<Vector2D> const& right) noexcept {
	if (left.size() != right.size())
		return false;
	for (std::size_t index = 0; index < left.size(); ++index) {
		if (!SamePoint(left[index], right[index]))
			return false;
	}
	return true;
}

bool SameTextStyle(VideoOverlayTextStyle const& left, VideoOverlayTextStyle const& right) noexcept {
	return left.face == right.face
		&& left.size == right.size
		&& left.bold == right.bold
		&& left.italic == right.italic
		&& SameColour(left.colour, right.colour)
		&& left.outline == right.outline;
}

bool SameCommand(Command const& left, Command const& right) noexcept {
	if (left.index() != right.index())
		return false;
	return std::visit([](auto const& lhs, auto const& rhs) noexcept {
		using Left = std::decay_t<decltype(lhs)>;
		using Right = std::decay_t<decltype(rhs)>;
		if constexpr (!std::is_same_v<Left, Right>) {
			return false;
		}
		else if constexpr (std::is_same_v<Left, SetLineColourCommand>) {
			return SameColour(lhs.colour, rhs.colour)
				&& lhs.alpha == rhs.alpha
				&& lhs.width == rhs.width;
		}
		else if constexpr (std::is_same_v<Left, SetFillColourCommand>) {
			return SameColour(lhs.colour, rhs.colour) && lhs.alpha == rhs.alpha;
		}
		else if constexpr (std::is_same_v<Left, SetInvertCommand>) {
			return lhs.enabled == rhs.enabled;
		}
		else if constexpr (std::is_same_v<Left, DrawLineCommand>) {
			return SamePoint(lhs.p1, rhs.p1) && SamePoint(lhs.p2, rhs.p2);
		}
		else if constexpr (std::is_same_v<Left, DrawLinesCommand>) {
			return lhs.dim == rhs.dim && lhs.count == rhs.count && lhs.values == rhs.values;
		}
		else if constexpr (std::is_same_v<Left, DrawLineStripCommand>
			|| std::is_same_v<Left, DrawPolygonCommand>) {
			return SamePoints(lhs.points, rhs.points);
		}
		else if constexpr (std::is_same_v<Left, DrawRectangleCommand>) {
			return SamePoint(lhs.p1, rhs.p1) && SamePoint(lhs.p2, rhs.p2);
		}
		else if constexpr (std::is_same_v<Left, DrawMultiPolygonCommand>) {
			return lhs.points == rhs.points
				&& lhs.start == rhs.start
				&& lhs.count == rhs.count
				&& SamePoint(lhs.video_pos, rhs.video_pos)
				&& SamePoint(lhs.video_size, rhs.video_size)
				&& lhs.invert_fill == rhs.invert_fill;
		}
		else if constexpr (std::is_same_v<Left, DrawCircleCommand>) {
			return SamePoint(lhs.center, rhs.center) && lhs.radius == rhs.radius;
		}
		else if constexpr (std::is_same_v<Left, DrawTriangleCommand>) {
			return SamePoint(lhs.p1, rhs.p1)
				&& SamePoint(lhs.p2, rhs.p2)
				&& SamePoint(lhs.p3, rhs.p3);
		}
		else if constexpr (std::is_same_v<Left, DrawTextCommand>) {
			return lhs.text == rhs.text
				&& lhs.x == rhs.x
				&& lhs.y == rhs.y
				&& SameTextStyle(lhs.style, rhs.style)
				&& lhs.measured == rhs.measured;
		}
		else {
			return false;
		}
	}, left, right);
}

void IncludePoint(SkiaOverlayLogicalBounds& bounds, float x, float y) noexcept {
	if (!std::isfinite(x) || !std::isfinite(y))
		return;
	if (!bounds.valid) {
		bounds = { true, x, y, x, y };
		return;
	}
	bounds.left = std::min(bounds.left, x);
	bounds.top = std::min(bounds.top, y);
	bounds.right = std::max(bounds.right, x);
	bounds.bottom = std::max(bounds.bottom, y);
}

void IncludeRect(
	SkiaOverlayLogicalBounds& bounds,
	float left,
	float top,
	float right,
	float bottom) noexcept {
	IncludePoint(bounds, std::min(left, right), std::min(top, bottom));
	IncludePoint(bounds, std::max(left, right), std::max(top, bottom));
}

void Outset(SkiaOverlayLogicalBounds& bounds, float amount) noexcept {
	if (!bounds.valid || !std::isfinite(amount) || amount <= 0.0f)
		return;
	bounds.left -= amount;
	bounds.top -= amount;
	bounds.right += amount;
	bounds.bottom += amount;
}

SkiaOverlayLogicalBounds UnionBounds(
	SkiaOverlayLogicalBounds left,
	SkiaOverlayLogicalBounds const& right) noexcept {
	if (!right.valid)
		return left;
	IncludeRect(left, right.left, right.top, right.right, right.bottom);
	return left;
}

void ReplayCommand(Command const& command, VideoOverlayDrawContext& target) {
	std::visit([&](auto const& value) {
		using Type = std::decay_t<decltype(value)>;
		if constexpr (std::is_same_v<Type, SetLineColourCommand>)
			target.SetLineColour(value.colour, value.alpha, value.width);
		else if constexpr (std::is_same_v<Type, SetFillColourCommand>)
			target.SetFillColour(value.colour, value.alpha);
		else if constexpr (std::is_same_v<Type, SetInvertCommand>) {
			if (value.enabled)
				target.SetInvert();
			else
				target.ClearInvert();
		}
		else if constexpr (std::is_same_v<Type, DrawLineCommand>)
			target.DrawLine(value.p1, value.p2);
		else if constexpr (std::is_same_v<Type, DrawLinesCommand>)
			target.DrawLines(value.dim, value.values.data(), value.count);
		else if constexpr (std::is_same_v<Type, DrawLineStripCommand>)
			target.DrawLineStrip(value.points.data(), value.points.size());
		else if constexpr (std::is_same_v<Type, DrawRectangleCommand>)
			target.DrawRectangle(value.p1, value.p2);
		else if constexpr (std::is_same_v<Type, DrawPolygonCommand>)
			target.DrawPolygon(value.points.data(), value.points.size());
		else if constexpr (std::is_same_v<Type, DrawMultiPolygonCommand>)
			target.DrawMultiPolygon(
				value.points,
				value.start,
				value.count,
				value.video_pos,
				value.video_size,
				value.invert_fill);
		else if constexpr (std::is_same_v<Type, DrawCircleCommand>)
			target.DrawCircle(value.center, value.radius);
		else if constexpr (std::is_same_v<Type, DrawTriangleCommand>)
			target.DrawTriangle(value.p1, value.p2, value.p3);
		else if constexpr (std::is_same_v<Type, DrawTextCommand>)
			target.DrawText(value.text, value.x, value.y, value.style);
	}, command);
}
}

struct SkiaVideoOverlayCommandBuffer::Impl {
	std::vector<Command> commands;
	SkiaOverlayLogicalBounds normal_bounds;
	SkiaOverlayLogicalBounds invert_bounds;
	bool has_normal_content = false;
	bool has_invert_content = false;

	SkiaOverlayLogicalBounds& BoundsFor(bool invert) noexcept {
		return invert ? invert_bounds : normal_bounds;
	}

	void MarkContent(bool invert) noexcept {
		if (invert)
			has_invert_content = true;
		else
			has_normal_content = true;
	}

	void IncludeContentBounds(bool invert, SkiaOverlayLogicalBounds const& bounds) noexcept {
		if (!bounds.valid)
			return;
		BoundsFor(invert) = UnionBounds(BoundsFor(invert), bounds);
		MarkContent(invert);
	}
};

SkiaOverlayDeviceBounds PlanSkiaOverlayDeviceBounds(
	SkiaOverlayLogicalBounds const& logical_bounds,
	float device_scale,
	int canvas_width,
	int canvas_height) noexcept {
	if (!logical_bounds.valid
		|| !std::isfinite(device_scale)
		|| device_scale <= 0.0f
		|| canvas_width <= 0
		|| canvas_height <= 0) {
		return {};
	}

	auto clamp_coordinate = [](double value, int limit) noexcept {
		if (value <= 0.0)
			return 0;
		if (value >= static_cast<double>(limit))
			return limit;
		return static_cast<int>(value);
	};
	double const scale = device_scale;
	int const left = clamp_coordinate(std::floor(logical_bounds.left * scale) - 1.0, canvas_width);
	int const top = clamp_coordinate(std::floor(logical_bounds.top * scale) - 1.0, canvas_height);
	int const right = clamp_coordinate(std::ceil(logical_bounds.right * scale) + 1.0, canvas_width);
	int const bottom = clamp_coordinate(std::ceil(logical_bounds.bottom * scale) + 1.0, canvas_height);
	if (right <= left || bottom <= top)
		return {};
	return { left, top, right - left, bottom - top };
}

SkiaOverlayDeviceBounds AlignSkiaOverlayDeviceBoundsForAllocation(
	SkiaOverlayDeviceBounds const& device_bounds,
	int canvas_width,
	int canvas_height,
	int alignment) noexcept {
	if (device_bounds.IsEmpty()
		|| canvas_width <= 0
		|| canvas_height <= 0
		|| alignment <= 0) {
		return {};
	}

	auto const clamp = [](long long value, int limit) noexcept {
		return static_cast<int>(std::clamp(value, 0LL, static_cast<long long>(limit)));
	};
	int const clipped_left = clamp(device_bounds.x, canvas_width);
	int const clipped_top = clamp(device_bounds.y, canvas_height);
	int const clipped_right = clamp(
		static_cast<long long>(device_bounds.x) + device_bounds.width,
		canvas_width);
	int const clipped_bottom = clamp(
		static_cast<long long>(device_bounds.y) + device_bounds.height,
		canvas_height);
	if (clipped_right <= clipped_left || clipped_bottom <= clipped_top)
		return {};

	auto const align_down = [alignment](int value) noexcept {
		return value / alignment * alignment;
	};
	auto const align_up = [alignment](int value, int limit) noexcept {
		long long const aligned =
			(static_cast<long long>(value) + alignment - 1) / alignment * alignment;
		return static_cast<int>(std::min(aligned, static_cast<long long>(limit)));
	};
	int const left = align_down(clipped_left);
	int const top = align_down(clipped_top);
	int const right = align_up(clipped_right, canvas_width);
	int const bottom = align_up(clipped_bottom, canvas_height);
	return { left, top, right - left, bottom - top };
}

SkiaOverlayDeviceBounds SelectSkiaOverlayBackingBounds(
	SkiaOverlayDeviceBounds const& required_bounds,
	SkiaOverlayDeviceBounds const& reusable_bounds,
	int canvas_width,
	int canvas_height,
	int alignment,
	int guard) noexcept {
	if (required_bounds.IsEmpty()
		|| canvas_width <= 0
		|| canvas_height <= 0
		|| alignment <= 0
		|| guard < 0) {
		return {};
	}

	auto const clamp = [](long long value, int limit) noexcept {
		return static_cast<int>(std::clamp(value, 0LL, static_cast<long long>(limit)));
	};
	auto const clip = [&](SkiaOverlayDeviceBounds const& bounds) noexcept {
		int const left = clamp(bounds.x, canvas_width);
		int const top = clamp(bounds.y, canvas_height);
		int const right = clamp(
			static_cast<long long>(bounds.x) + bounds.width,
			canvas_width);
		int const bottom = clamp(
			static_cast<long long>(bounds.y) + bounds.height,
			canvas_height);
		if (right <= left || bottom <= top)
			return SkiaOverlayDeviceBounds {};
		return SkiaOverlayDeviceBounds { left, top, right - left, bottom - top };
	};
	auto const required = clip(required_bounds);
	if (required.IsEmpty())
		return {};
	auto const fit_origin = [](int required_start, int required_size, int capacity, int limit, int preferred) noexcept {
		long long const min_origin = std::max(
			0LL,
			static_cast<long long>(required_start) + required_size - capacity);
		long long const max_origin = std::min(
			static_cast<long long>(required_start),
			static_cast<long long>(limit) - capacity);
		if (min_origin > max_origin)
			return 0;
		return static_cast<int>(std::clamp(static_cast<long long>(preferred), min_origin, max_origin));
	};

	auto const reusable_is_valid = [&]() noexcept {
		return !reusable_bounds.IsEmpty()
			&& reusable_bounds.x >= 0
			&& reusable_bounds.y >= 0
			&& reusable_bounds.width <= canvas_width
			&& reusable_bounds.height <= canvas_height
			&& static_cast<long long>(reusable_bounds.x) + reusable_bounds.width <= canvas_width
			&& static_cast<long long>(reusable_bounds.y) + reusable_bounds.height <= canvas_height;
	}();

	// A backing texture can move without being reallocated. Keep its capacity
	// while sliding the origin just far enough to contain the new content.
	if (reusable_is_valid
		&& reusable_bounds.width >= required.width
		&& reusable_bounds.height >= required.height) {
		return {
			fit_origin(required.x, required.width, reusable_bounds.width, canvas_width, reusable_bounds.x),
			fit_origin(required.y, required.height, reusable_bounds.height, canvas_height, reusable_bounds.y),
			reusable_bounds.width,
			reusable_bounds.height,
		};
	}

	long long const required_right = static_cast<long long>(required.x) + required.width;
	long long const required_bottom = static_cast<long long>(required.y) + required.height;
	auto const guarded = SkiaOverlayDeviceBounds {
		clamp(static_cast<long long>(required.x) - guard, canvas_width),
		clamp(static_cast<long long>(required.y) - guard, canvas_height),
		0,
		0,
	};
	int const guarded_right = clamp(required_right + guard, canvas_width);
	int const guarded_bottom = clamp(required_bottom + guard, canvas_height);
	auto allocation = AlignSkiaOverlayDeviceBoundsForAllocation(
		{
			guarded.x,
			guarded.y,
			guarded_right - guarded.x,
			guarded_bottom - guarded.y,
		},
		canvas_width,
		canvas_height,
		alignment);
	if (allocation.IsEmpty())
		return {};

	// Preserve capacity on an axis which did not need to grow. This avoids
	// reallocating both texture dimensions when only one dimension changed.
	if (reusable_is_valid) {
		allocation.width = std::max(allocation.width, reusable_bounds.width);
		allocation.height = std::max(allocation.height, reusable_bounds.height);
		allocation.width = std::min(allocation.width, canvas_width);
		allocation.height = std::min(allocation.height, canvas_height);

		allocation.x = fit_origin(
			required.x,
			required.width,
			allocation.width,
			canvas_width,
			reusable_bounds.x);
		allocation.y = fit_origin(
			required.y,
			required.height,
			allocation.height,
			canvas_height,
			reusable_bounds.y);
	}
	return allocation;
}

SkiaVideoOverlayCommandBuffer::SkiaVideoOverlayCommandBuffer()
: impl(std::make_unique<Impl>()) {
}

SkiaVideoOverlayCommandBuffer::~SkiaVideoOverlayCommandBuffer() = default;
SkiaVideoOverlayCommandBuffer::SkiaVideoOverlayCommandBuffer(SkiaVideoOverlayCommandBuffer&&) noexcept = default;
SkiaVideoOverlayCommandBuffer& SkiaVideoOverlayCommandBuffer::operator=(SkiaVideoOverlayCommandBuffer&&) noexcept = default;

bool SkiaVideoOverlayCommandBuffer::Empty() const noexcept {
	return !impl->has_normal_content && !impl->has_invert_content;
}

bool SkiaVideoOverlayCommandBuffer::HasNormalContent() const noexcept {
	return impl->has_normal_content;
}

bool SkiaVideoOverlayCommandBuffer::HasInvertContent() const noexcept {
	return impl->has_invert_content;
}

std::size_t SkiaVideoOverlayCommandBuffer::CommandCount() const noexcept {
	return impl->commands.size();
}

SkiaOverlayLogicalBounds SkiaVideoOverlayCommandBuffer::NormalBounds() const noexcept {
	return impl->normal_bounds;
}

SkiaOverlayLogicalBounds SkiaVideoOverlayCommandBuffer::InvertBounds() const noexcept {
	return impl->invert_bounds;
}

SkiaOverlayLogicalBounds SkiaVideoOverlayCommandBuffer::CombinedBounds() const noexcept {
	return UnionBounds(impl->normal_bounds, impl->invert_bounds);
}

bool SkiaVideoOverlayCommandBuffer::EquivalentTo(
	SkiaVideoOverlayCommandBuffer const& other) const noexcept {
	if (impl->commands.size() != other.impl->commands.size()
		|| impl->has_normal_content != other.impl->has_normal_content
		|| impl->has_invert_content != other.impl->has_invert_content) {
		return false;
	}
	for (std::size_t index = 0; index < impl->commands.size(); ++index) {
		if (!SameCommand(impl->commands[index], other.impl->commands[index]))
			return false;
	}
	return true;
}

void SkiaVideoOverlayCommandBuffer::Replay(VideoOverlayDrawContext& target) const {
	for (auto const& command : impl->commands)
		ReplayCommand(command, target);
}

SkiaVideoOverlayRecorder::SkiaVideoOverlayRecorder(
	SkiaVideoOverlayTextMeasurer text_measurer,
	float device_scale)
: text_measurer(std::move(text_measurer))
, device_scale(std::max(1.0f, device_scale)) {
}

SkiaVideoOverlayCommandBuffer SkiaVideoOverlayRecorder::TakeBuffer() noexcept {
	return std::move(buffer);
}

void SkiaVideoOverlayRecorder::SetLineColour(wxColour const& colour, float alpha, int width) {
	line_colour = colour;
	line_alpha = alpha;
	line_width = width;
	buffer.impl->commands.emplace_back(SetLineColourCommand { colour, alpha, width });
}

void SkiaVideoOverlayRecorder::SetFillColour(wxColour const& colour, float alpha) {
	fill_colour = colour;
	fill_alpha = alpha;
	buffer.impl->commands.emplace_back(SetFillColourCommand { colour, alpha });
}

void SkiaVideoOverlayRecorder::SetInvert() {
	invert = true;
	buffer.impl->commands.emplace_back(SetInvertCommand { true });
}

void SkiaVideoOverlayRecorder::ClearInvert() {
	invert = false;
	buffer.impl->commands.emplace_back(SetInvertCommand { false });
}

void SkiaVideoOverlayRecorder::DrawLine(Vector2D p1, Vector2D p2) {
	buffer.impl->commands.emplace_back(DrawLineCommand { p1, p2 });
	if (!invert && line_alpha <= 0.0f)
		return;
	SkiaOverlayLogicalBounds bounds;
	IncludePoint(bounds, p1.X(), p1.Y());
	IncludePoint(bounds, p2.X(), p2.Y());
	Outset(bounds, std::max(1, line_width) / (2.0f * device_scale));
	buffer.impl->IncludeContentBounds(invert, bounds);
}

void SkiaVideoOverlayRecorder::DrawLines(size_t dim, float const *lines, size_t n) {
	if (dim != 2 || !lines || n < 2)
		return;
	DrawLinesCommand command;
	command.dim = dim;
	command.count = n;
	command.values.assign(lines, lines + dim * n);
	buffer.impl->commands.emplace_back(std::move(command));
	if (!invert && line_alpha <= 0.0f)
		return;
	SkiaOverlayLogicalBounds bounds;
	for (std::size_t index = 0; index < n; ++index)
		IncludePoint(bounds, lines[index * dim], lines[index * dim + 1]);
	Outset(bounds, std::max(1, line_width) / (2.0f * device_scale));
	buffer.impl->IncludeContentBounds(invert, bounds);
}

void SkiaVideoOverlayRecorder::DrawLineStrip(Vector2D const *points, size_t n) {
	if (!points || n < 2)
		return;
	DrawLineStripCommand command;
	command.points.assign(points, points + n);
	buffer.impl->commands.emplace_back(std::move(command));
	if (!invert && line_alpha <= 0.0f)
		return;
	SkiaOverlayLogicalBounds bounds;
	for (std::size_t index = 0; index < n; ++index)
		IncludePoint(bounds, points[index].X(), points[index].Y());
	Outset(bounds, std::max(1, line_width) / (2.0f * device_scale));
	buffer.impl->IncludeContentBounds(invert, bounds);
}

void SkiaVideoOverlayRecorder::DrawRectangle(Vector2D p1, Vector2D p2) {
	buffer.impl->commands.emplace_back(DrawRectangleCommand { p1, p2 });
	bool const visible_fill = fill_alpha > 0.0f;
	bool const visible_line = line_alpha > 0.0f;
	if (!visible_fill && !visible_line)
		return;
	SkiaOverlayLogicalBounds bounds;
	IncludeRect(bounds, p1.X(), p1.Y(), p2.X(), p2.Y());
	if (visible_line)
		Outset(bounds, std::max(1, line_width) / (2.0f * device_scale));
	buffer.impl->IncludeContentBounds(invert, bounds);
}

void SkiaVideoOverlayRecorder::DrawPolygon(Vector2D const *points, size_t n) {
	if (!points || n < 3)
		return;
	DrawPolygonCommand command;
	command.points.assign(points, points + n);
	buffer.impl->commands.emplace_back(std::move(command));
	bool const visible_fill = fill_alpha > 0.0f;
	bool const visible_line = line_alpha > 0.0f;
	if (!visible_fill && !visible_line)
		return;
	SkiaOverlayLogicalBounds bounds;
	for (std::size_t index = 0; index < n; ++index)
		IncludePoint(bounds, points[index].X(), points[index].Y());
	if (visible_line)
		Outset(bounds, std::max(1, line_width) / (2.0f * device_scale));
	buffer.impl->IncludeContentBounds(invert, bounds);
}

void SkiaVideoOverlayRecorder::DrawMultiPolygon(
	std::vector<float> const& points,
	std::vector<int> const& start,
	std::vector<int> const& count,
	Vector2D video_pos,
	Vector2D video_size,
	bool invert_fill) {
	if (points.empty() || start.empty() || count.empty() || start.size() != count.size())
		return;
	buffer.impl->commands.emplace_back(DrawMultiPolygonCommand {
		points,
		start,
		count,
		video_pos,
		video_size,
		invert_fill,
	});
	bool const visible_fill = fill_alpha > 0.0f;
	bool const visible_line = line_alpha > 0.0f;
	if (!visible_fill && !visible_line)
		return;
	SkiaOverlayLogicalBounds bounds;
	if (invert_fill && visible_fill) {
		IncludeRect(
			bounds,
			video_pos.X(),
			video_pos.Y(),
			video_pos.X() + video_size.X(),
			video_pos.Y() + video_size.Y());
	}
	else {
		for (std::size_t index = 0; index + 1 < points.size(); index += 2)
			IncludePoint(bounds, points[index], points[index + 1]);
	}
	if (visible_line)
		Outset(bounds, std::max(1, line_width) / (2.0f * device_scale));
	buffer.impl->IncludeContentBounds(invert, bounds);
}

void SkiaVideoOverlayRecorder::DrawCircle(Vector2D center, float radius) {
	buffer.impl->commands.emplace_back(DrawCircleCommand { center, radius });
	bool const visible_fill = fill_alpha > 0.0f;
	bool const visible_line = line_alpha > 0.0f;
	if ((!visible_fill && !visible_line) || radius < 0.0f)
		return;
	SkiaOverlayLogicalBounds bounds;
	IncludeRect(
		bounds,
		center.X() - radius,
		center.Y() - radius,
		center.X() + radius,
		center.Y() + radius);
	if (visible_line)
		Outset(bounds, std::max(1, line_width) / (2.0f * device_scale));
	buffer.impl->IncludeContentBounds(invert, bounds);
}

void SkiaVideoOverlayRecorder::DrawTriangle(Vector2D p1, Vector2D p2, Vector2D p3) {
	buffer.impl->commands.emplace_back(DrawTriangleCommand { p1, p2, p3 });
	bool const visible_fill = fill_alpha > 0.0f;
	bool const visible_line = line_alpha > 0.0f;
	if (!visible_fill && !visible_line)
		return;
	SkiaOverlayLogicalBounds bounds;
	IncludePoint(bounds, p1.X(), p1.Y());
	IncludePoint(bounds, p2.X(), p2.Y());
	IncludePoint(bounds, p3.X(), p3.Y());
	if (visible_line)
		Outset(bounds, std::max(1, line_width) / (2.0f * device_scale));
	buffer.impl->IncludeContentBounds(invert, bounds);
}

wxSize SkiaVideoOverlayRecorder::MeasureText(
	std::string const& text,
	VideoOverlayTextStyle const& style) {
	return text_measurer ? text_measurer(text, style) : wxSize{};
}

void SkiaVideoOverlayRecorder::DrawText(
	std::string const& text,
	int x,
	int y,
	VideoOverlayTextStyle const& style) {
	wxSize const measured = MeasureText(text, style);
	buffer.impl->commands.emplace_back(DrawTextCommand { text, x, y, style, measured });
	if (!invert && !style.outline && style.colour.Alpha() == 0)
		return;
	SkiaOverlayLogicalBounds bounds;
	float const outline = style.outline ? 2.0f : 0.0f;
	IncludeRect(
		bounds,
		static_cast<float>(x) - outline,
		static_cast<float>(y) - outline,
		static_cast<float>(x + measured.GetWidth()) + outline,
		static_cast<float>(y + measured.GetHeight()) + outline);
	buffer.impl->IncludeContentBounds(invert, bounds);
}
