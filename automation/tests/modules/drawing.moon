-- Copyright (c) 2026 Aegisub Project
--
-- Permission to use, copy, modify, and distribute this software for any
-- purpose with or without fee is hereby granted, provided that the above
-- copyright notice and this permission notice appear in all copies.
--
-- THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
-- WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
-- MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
-- ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
-- WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
-- ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
-- OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

drawing = require 'aegisub.drawing'

describe 'drawing', ->
  it 'normalizes open drawings without closing them', ->
    assert.is.equal 'm 0 0 l 10 10', drawing.normalize_open 'm 0 0 l 10 10'

  it 'normalizes filled drawings with implicit close', ->
    assert.is.equal 'm 0 0 l 10 0 10 10', drawing.normalize 'm 0 0 l 10 0 l 10 10'

  it 'translates drawings', ->
    assert.is.equal 'm 1 2 l 11 12', drawing.translate 'm 0 0 l 10 10', 1, 2

  it 'scales drawings', ->
    assert.is.equal 'm 0 0 l 20 5', drawing.scale 'm 0 0 l 10 10', 2, 0.5

  it 'rotates drawings around the origin', ->
    assert.is.equal 'm 0 1 l -1 0', drawing.rotate 'm 1 0 l 0 1', 90

  it 'supports explicit affine transforms', ->
    assert.is.equal 'm 5 7 l 7 10', drawing.transform 'm 1 1 l 2 2', 2, 0, 0, 3, 3, 4

  it 'accepts libass compatibility mode', ->
    assert.is.equal 'm 0.016 0 l 1.016 0', drawing.normalize_open 'm 0.01 0 l 1.01 0', 'libass'

  it 'computes drawing bounds', ->
    x, y, width, height = drawing.bounds 'm 0 0 b 0 10 10 10 10 0'
    assert.is.equal 0, x
    assert.is.equal 0, y
    assert.is.equal 10, width
    assert.is.equal 7.5, height

  it 'flattens drawings', ->
    assert.is.equal 'm 0 0 l 10 0', drawing.flatten 'm 0 0 b 0 10 10 10 10 0', 100

  it 'reverses drawings', ->
    assert.is.equal 'm 20 0 b 20 10 10 10 10 0 l 0 0', drawing.reverse 'm 0 0 l 10 0 b 10 10 20 10 20 0'

  it 'measures drawing length and positions', ->
    assert.is.equal 10, drawing.length 'm 0 0 l 3 4 l 6 8'
    assert.is.equal 0.25, drawing.percent_at_length 'm 0 0 l 10 0', 2.5
    x, y = drawing.point_at_percent 'm 0 0 l 10 0', 0.25
    assert.is.equal 2.5, x
    assert.is.equal 0, y

  it 'computes area and centroid', ->
    assert.is.equal 100, drawing.area 'm 0 0 l 10 0 l 10 10 l 0 10'
    x, y = drawing.centroid 'm 0 0 l 10 0 l 10 10 l 0 10'
    assert.is.equal 5, x
    assert.is.equal 5, y

  it 'exposes legacy shape names', ->
    assert.is.equal 'm 0 0 l 10 0 10 10 0 10', drawing.shape_rect 0, 0, 10, 10
    assert.is.equal 'm 0 0 l 10 0 10 10 0 10', drawing.compact 'm 10 10 l 0 10 0 0 10 0 10 10'
    assert.is.equal 'm 1 2 l 11 12', drawing.shape_translate 'm 0 0 l 10 10', 1, 2
    assert.is.equal 5, drawing.shape_length 'm 0 0 l 3 4'
    assert.is.equal 'm 20 10', drawing.shape_arc_move_to '', 0, 0, 20, 20, 0

  it 'exposes public shape names without debug helpers', ->
    expected = {
      'shape_angle_at_percent'
      'shape_arc_move_to'
      'shape_arc_to'
      'shape_bouding'
      'shape_bouding_coords'
      'shape_contains_point'
      'shape_contains_rect'
      'shape_ellipse'
      'shape_intersected'
      'shape_length'
      'shape_normalize_ass'
      'shape_normalize_ass_with_mode'
      'shape_outline'
      'shape_pattern_outline'
      'shape_percent_at_length'
      'shape_point_at_percent'
      'shape_rect'
      'shape_rotate'
      'shape_rounded_rect'
      'shape_scale'
      'shape_shear'
      'shape_slope_at_percent'
      'shape_subtracted'
      'shape_translate'
      'shape_united'
      'shape_xored'
    }

    for name in *expected
      assert.is.equal 'function', type drawing[name]

    for name in pairs drawing
      assert.is.falsy name\match '^shape_debug_'
