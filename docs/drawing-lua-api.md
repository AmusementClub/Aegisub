# Drawing Lua API

The drawing module is available as:

```lua
local drawing = require "aegisub.drawing"
```

It parses and emits ASS drawing text. Emitted text is normalized for renderer
stability and compactness: repeated drawing commands are coalesced where ASS
syntax allows it, filled contours are implicitly closed, and coordinates are
written with no trailing zeroes and at most three decimal places.

Debug-only helpers are not part of this module. Public compatibility functions
use the `shape_*` names listed below.

## Compatibility modes

Most parsing functions accept an optional compatibility mode as either a number
or a string:

- `0`, `"vsfilter"`, `"xy-vsfilter"`, or `"xy"`: VSFilter-style coordinate
  handling.
- `1` or `"libass"`: libass-style coordinate handling.

When omitted, VSFilter-style handling is used.

## General helpers

- `normalize(shape[, mode]) -> shape`
  Parses a filled drawing and emits normalized ASS text.
- `normalize_open(shape[, mode]) -> shape`
  Parses an open drawing and preserves dangling anchors.
- `compact(shape[, mode]) -> shape`
  Parses a filled drawing and emits a shorter equivalent ASS form when possible.
- `transform(shape, m11, m12, m21, m22, dx, dy[, mode]) -> shape`
  Applies an affine transform to an open drawing.
- `translate(shape, dx, dy[, mode]) -> shape`
- `scale(shape, sx, sy[, mode]) -> shape`
- `rotate(shape, degrees[, mode]) -> shape`
- `shear(shape, horizontal, vertical[, mode]) -> shape`
- `bounds(shape[, mode]) -> x, y, width, height`
  Returns `nil` for an empty drawing.
- `flatten(shape[, tolerance[, mode]]) -> shape`
- `reverse(shape[, mode]) -> shape`
- `length(shape[, mode]) -> number`
- `percent_at_length(shape, distance[, mode]) -> number`
- `point_at_percent(shape, percent[, mode]) -> x, y`
  Returns `nil` if no position is available.
- `point_at_length(shape, distance[, mode]) -> x, y`
  Returns `nil` if no position is available.
- `angle_at_percent(shape, percent[, mode]) -> degrees`
- `slope_at_percent(shape, percent[, mode]) -> number`
- `area(shape[, tolerance[, mode]]) -> number`
  Returns `nil` if the filled drawing has no measurable area.
- `centroid(shape[, tolerance[, mode]]) -> x, y`
  Returns `nil` if the filled drawing has no measurable area.

## Public shape_* API

These names are exposed for script compatibility:

- `shape_rect(x, y, width, height) -> shape`
- `shape_ellipse(x, y, width, height) -> shape`
- `shape_rounded_rect(x, y, width, height, radius_x, radius_y) -> shape`
- `shape_arc_move_to(shape, x, y, width, height, angle[, mode]) -> shape`
- `shape_arc_to(shape, x, y, width, height, start_angle, sweep[, mode]) -> shape`
- `shape_normalize_ass(shape[, mode]) -> shape`
- `shape_normalize_ass_with_mode(shape[, mode]) -> shape`
- `shape_translate(shape, dx, dy[, mode]) -> shape`
- `shape_rotate(shape, degrees[, mode]) -> shape`
- `shape_scale(shape, sx, sy[, mode]) -> shape`
- `shape_shear(shape, horizontal, vertical[, mode]) -> shape`
- `shape_bouding(shape[, mode]) -> x, y, width, height`
  The misspelling is intentional for compatibility. Empty shapes return
  `0, 0, 0, 0`.
- `shape_bouding_coords(shape[, mode]) -> x1, y1, x2, y2`
  Empty shapes return `0, 0, 0, 0`.
- `shape_length(shape[, mode]) -> number`
- `shape_percent_at_length(shape, distance[, mode]) -> number`
- `shape_point_at_percent(shape, percent[, mode]) -> x, y`
- `shape_angle_at_percent(shape, percent[, mode]) -> degrees`
- `shape_slope_at_percent(shape, percent[, mode]) -> number`

The following functions use the drawing Skia backend. They are available only
when the application is built with `WITH_DRAWING_SKIA`; otherwise they raise a
Lua error.

- `shape_contains_point(shape, x, y[, mode]) -> boolean`
- `shape_contains_rect(shape, x, y, width, height[, mode]) -> boolean`
- `shape_united(lhs, rhs[, mode]) -> shape`
- `shape_intersected(lhs, rhs[, mode]) -> shape`
- `shape_subtracted(lhs, rhs[, mode]) -> shape`
- `shape_xored(lhs, rhs[, mode]) -> shape`
- `shape_outline(shape, width, cap, join[, mode]) -> shape`
- `shape_outline_with_flatten(shape, width, cap, join, tolerance[, mode]) -> shape`
- `shape_pattern_outline(shape, width, cap, join, pattern_length, space_length, dash_offset[, mode]) -> shape`

`cap` accepts `"flat"`, `"butt"`, `"round"`, or `"square"`.
`join` accepts `"miter"`, `"svgmiter"`, `"round"`, or `"bevel"`.

## Example

```lua
local drawing = require "aegisub.drawing"

local rect = drawing.shape_rect(0, 0, 100, 40)
local moved = drawing.shape_translate(rect, 20, 10)
local x, y, width, height = drawing.shape_bouding(moved)
```
