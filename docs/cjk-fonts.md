# CJK Font Support

This guide explains how to use Chinese, Japanese, and Korean (CJK) fonts on your CrossPoint Reader.

## Overview

CrossPoint Reader supports external CJK fonts for both:

- **UI Font** - Used for menus, settings, and system interface
- **Reader Font** - Used for reading ebook content

The device includes a built-in CJK UI font (Source Han Sans subset) for basic interface rendering, and supports loading custom external fonts from the SD card.

## Prerequisites

- CrossPoint Reader device with firmware version supporting CJK fonts
- SD card with available space for font files
- TrueType font files (.ttf) converted to the CrossPoint font format

---

## Font File Format

CrossPoint Reader uses a custom binary font format optimized for e-ink displays. Font files must be placed in the `/fonts/` directory on the SD card.

### File Naming Convention

Font files must follow this naming pattern:

```
{FontName}_{Size}_{Width}x{Height}.bin
```

**Examples:**
- `SourceHanSansCN-Medium_20_20x20.bin`
- `KingHwaOldSong_38_33x39.bin`
- `Yozai-Medium_36_31x31.bin`

### Font File Structure

The binary font file contains:

1. **Header** (variable length)
   - Font name (null-terminated string)
   - Font size (uint8_t)
   - Character width (uint8_t)
   - Character height (uint8_t)
   - Bytes per character (uint16_t)

2. **Character Data**
   - Sequential bitmap data for each character
   - Characters are stored in Unicode order
   - Each character uses `width * height / 8` bytes (1-bit per pixel)

---

## Generating SD Card Font Files

SD card fonts (`/fonts/*.bin`) are produced by the scripts under
`lib/EpdFont/scripts/` — see `lib/EpdFont/README`. The script described below
(`scripts/generate_cjk_ui_font.py`) generates the **built-in** font header, not
SD card fonts.

---

## Installing Fonts

1. **Create the fonts directory** (if it doesn't exist):
   ```
   /fonts/
   ```

2. **Copy font files** to the `/fonts/` directory on your SD card

3. **Restart the device** or go to Settings to scan for new fonts

---

## Selecting Fonts

### UI Font

1. Go to **Settings** → **Display**
2. Select **External UI Font**
3. Choose from available fonts or select **Built-in (Disabled)** to use the default font

### Reader Font

1. Go to **Settings** → **Reader**
2. Select **External Reader Font**
3. Choose from available fonts or select **Built-in (Disabled)** to use the default font

---

## Built-in CJK UI Font

The firmware embeds a pre-rendered 20×20 bitmap subset of **Source Han Sans JP
Medium** in `lib/GfxRenderer/cjk_ui_font_20.h` (7,488 glyphs, ~440 KB of flash).
It is the only source of CJK glyphs for UI text — the built-in UI fonts
(`ubuntu_10/12`) contain no CJK at all, so anything missing from this header is
drawn as `?`.

The built-in font is used when:
- No external UI font is selected
- The external font file is missing or corrupted

### Coverage

| Source | Characters |
|--------|-----------|
| `scripts/codepoints_baseline.txt` | 3,420 — snapshot of the glyph set shipped before the JIS level 2 expansion |
| `scripts/codepoints_jis_level1.txt` | 2,965 — JIS X 0208 level 1 kanji |
| `scripts/codepoints_jis_level2.txt` | 3,390 — JIS X 0208 level 2 kanji |
| `scripts/codepoints_cp932_ext.txt` | 454 — CP932 NEC/IBM extensions (髙 﨑 彅 ① № Ⅰ ㈱ …) |
| `scripts/codepoints_ui_symbols.txt` | 225 — CJK punctuation, fullwidth alphanumerics, halfwidth katakana, ・ 〜 ※ ○ ★ ← … |
| `lib/I18n/translations/*.yaml` | every non-ASCII character used by UI strings (extracted automatically) |
| `BASE_UI_CHARS` in the generator | ASCII, kana, common punctuation |

`scripts/check_cjk_ui_font.py` runs as a PlatformIO pre-build step and fails the
build if any code point from these lists is absent from the header. Add new
requirements to a `codepoints_*.txt` file so the check can catch regressions —
book titles and file names are dynamic and can never be validated automatically.

**Do not add non-CJK code points that `ubuntu_10/12` already covers.**
`renderChar()` checks `hasCjkUiGlyph()` before falling back to the EPD font even
for non-CJK characters (`GfxRenderer.cpp:2499-2504`), so such a character would
jump from its correct 10/12pt size to a fixed 20px. `× ÷ ∑ √ ∫` are excluded for
exactly this reason. Symbols the ubuntu fonts lack (`№ ← ○ ★ ■ …`) are fine —
they were rendering as `?` before.

### Regenerating

Requires **Source Han Sans JP Medium** (OFL) from
[adobe-fonts/source-han-sans](https://github.com/adobe-fonts/source-han-sans/releases)
(`17_SourceHanSansJP.zip` → `SubsetOTF/JP/SourceHanSansJP-Medium.otf`). The OTF
is not committed.

```bash
python3 scripts/generate_cjk_ui_font.py \
  --size 20 \
  --font /path/to/SourceHanSansJP-Medium.otf \
  --force-pt 20 --force-descent 3 \
  --inherit-header lib/GfxRenderer/cjk_ui_font_20.h \
  --codepoints-file scripts/codepoints_baseline.txt \
  --codepoints-file scripts/codepoints_jis_level1.txt \
  --codepoints-file scripts/codepoints_jis_level2.txt \
  --codepoints-file scripts/codepoints_cp932_ext.txt \
  --codepoints-file scripts/codepoints_ui_symbols.txt
```

`--force-pt 20 --force-descent 3` is **required**. Different Source Han Sans
releases report very different hhea metrics (880/-120 vs 1160/-288); without the
override the automatic fit would pick `pt=17, baseline=15` and shrink every UI
glyph by 15% while shifting it 2px up. The header comment records the metrics
that must be reproduced.

`--inherit-header` copies glyphs the source font does not cover from the previous
header instead of emitting `.notdef` tofu. The JP subset OTF is missing 54 Latin
Extended / Cyrillic characters (`Ł` `İ` `Š` `І` `Ә` …) plus `↺` `↻` that the UI
translations use.

After regenerating, verify:

```bash
python3 scripts/check_cjk_ui_font.py    # coverage
pio run                                  # flash usage
```

### Known limitations

- Halfwidth katakana (U+FF61–FF9F) are rasterised into the left half of the 20px
  cell but the shipped width table still reports 20px, so `GfxRenderer` overrides
  their advance to 10px at runtime (`builtinCjkAdvance`). The generator now emits
  10px directly, so the override becomes a no-op after the next regeneration
- `＾` (U+FF3E) and `｀` (U+FF40) come out blank: they sit above the 20px cell at
  baseline 17 and get clipped
- `hasCjkUiGlyph()` returns false above U+FFFF (the lookup table is `uint16_t`),
  so emoji and CJK Extension B are still drawn as `?`
- JIS X 0213 level 3/4 is not included, apart from `鷗` (U+9DD7)

---

## External UI Font Features

When an external UI font is selected:

### Full Character Coverage
- **All characters** (including ASCII letters, numbers, and punctuation) are rendered using the external font
- This ensures consistent visual style across the entire UI
- If a character is missing from the external font, the system falls back to built-in fonts

### Proportional Spacing
- External fonts use **proportional spacing** (variable width)
- Each character advances by its actual width, not a fixed width
- This makes English text look more natural with proper letter spacing
- CJK characters still use their full width as designed

---

## Recommended Fonts

### For UI (Small sizes, 18-24pt)

| Font | Description | License |
|------|-------------|---------|
| Source Han Sans | Clean, modern sans-serif | OFL |
| Noto Sans CJK | Google's CJK font family | OFL |
| WenQuanYi Micro Hei | Compact Chinese font | GPL |

### For Reading (Larger sizes, 28-40pt)

| Font | Description | License |
|------|-------------|---------|
| Source Han Serif | Traditional serif style | OFL |
| Noto Serif CJK | Google's serif CJK font | OFL |
| FangSong | Classic Chinese style | Varies |

---

## Memory Considerations

External fonts consume RAM when loaded. Consider these guidelines:

| Font Size | Approx. Memory Usage |
|-----------|---------------------|
| 20pt | ~50KB per 1000 characters |
| 28pt | ~100KB per 1000 characters |
| 36pt | ~160KB per 1000 characters |

**Tips:**
- Use smaller font sizes for UI (18-24pt)
- Larger fonts (32pt+) are better for reader content
- Only one UI font and one reader font are loaded at a time

---

## Troubleshooting

### Font not appearing in selection list

1. Check the file is in `/fonts/` directory
2. Verify the filename follows the naming convention
3. Ensure the file is not corrupted (try regenerating)

### Characters displaying as boxes or question marks

1. The character may not be included in the font
2. Try a font with broader character coverage
3. Check if the font file was generated correctly

### Device running slowly after selecting font

1. The font file may be too large
2. Try a smaller font size
3. Reduce the character set when generating the font

### Font looks blurry or pixelated

1. E-ink displays work best with specific font sizes
2. Try sizes that are multiples of the display's native resolution
3. Ensure anti-aliasing is disabled for 1-bit rendering

---

## Technical Details

### Font Manager API

The `FontManager` class provides:

```cpp
// Scan for available fonts
FontMgr.scanFonts();

// Get font count
int count = FontMgr.getFontCount();

// Get font info
const FontInfo* info = FontMgr.getFontInfo(index);

// Select reader font (-1 to disable)
FontMgr.selectFont(index);

// Select UI font (-1 to disable)
FontMgr.selectUiFont(index);

// Check if external font is enabled
bool enabled = FontMgr.isExternalFontEnabled();
```

### Font Info Structure

```cpp
struct FontInfo {
    char name[32];      // Font name
    uint8_t size;       // Font size in points
    uint8_t width;      // Character width in pixels
    uint8_t height;     // Character height in pixels
    uint16_t bytesPerChar; // Bytes per character
    char path[64];      // Full path to font file
};
```

---

## Related Documentation

- [Internationalization (I18N)](./i18n.md) - Multi-language support
- [File Formats](./file-formats.md) - Binary file format specifications
- [Troubleshooting](./troubleshooting.md) - General troubleshooting guide
