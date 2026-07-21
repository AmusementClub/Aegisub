# SPDX-License-Identifier: CC0-1.0
"""Generate the minimal GDI variant fixtures in this directory."""

from pathlib import Path

from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen


FAMILY = "Aegisub GDI Variant Fixture"
OUTPUT_DIR = Path(__file__).resolve().parent


def rectangle(x_min: int, y_min: int, x_max: int, y_max: int):
    pen = TTGlyphPen(None)
    pen.moveTo((x_min, y_min))
    pen.lineTo((x_max, y_min))
    pen.lineTo((x_max, y_max))
    pen.lineTo((x_min, y_max))
    pen.closePath()
    return pen.glyph()


def empty_glyph():
    return TTGlyphPen(None).glyph()


def build(style: str, weight: int, italic: bool, output_name: str) -> None:
    bold = weight == 700
    builder = FontBuilder(1000, isTTF=True)
    glyph_order = [".notdef", "space", "A"]
    builder.setupGlyphOrder(glyph_order)
    builder.setupCharacterMap({0x20: "space", 0x41: "A"})
    builder.setupGlyf(
        {
            ".notdef": rectangle(50, 0, 550, 700),
            "space": empty_glyph(),
            "A": rectangle(80 if bold else 120, 0, 620 if bold else 580, 700),
        }
    )
    builder.setupHorizontalMetrics({name: (700, 0) for name in glyph_order})
    builder.setupHorizontalHeader(ascent=800, descent=-200)
    builder.setupNameTable(
        {
            "familyName": FAMILY,
            "styleName": style,
            "uniqueFontIdentifier": f"AegisubFixture-{style}",
            "fullName": f"{FAMILY} {style}",
            "psName": f"AegisubGdiVariantFixture-{style}",
            "version": "Version 1.000",
            "copyright": "Dedicated to the public domain under CC0 1.0.",
            "licenseDescription": "CC0 1.0 Universal",
            "licenseInfoURL": "https://creativecommons.org/publicdomain/zero/1.0/",
            "typographicFamily": FAMILY,
            "typographicSubfamily": style,
        }
    )
    builder.setupOS2(
        sTypoAscender=800,
        sTypoDescender=-200,
        usWinAscent=800,
        usWinDescent=200,
        usWeightClass=weight,
        fsSelection=(0x20 if bold else 0) | (0x01 if italic else 0) |
        (0x40 if not bold and not italic else 0),
        achVendID="TEST",
    )
    builder.setupPost(italicAngle=-12.0 if italic else 0.0)
    builder.setupMaxp()
    builder.setupHead(macStyle=(1 if bold else 0) | (2 if italic else 0))
    builder.font.recalcTimestamp = False
    builder.font["head"].created = 3_800_000_000
    builder.font["head"].modified = 3_800_000_000
    builder.save(OUTPUT_DIR / output_name)


if __name__ == "__main__":
    build("Regular", 400, False, "regular.ttf")
    build("Bold", 700, False, "bold.ttf")
    build("Italic", 400, True, "italic.ttf")
    build("Bold Italic", 700, True, "bold-italic.ttf")
