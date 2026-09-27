# File Formats

## `book.bin`

### Version 3

ImHex Pattern:

```c++
import std.mem;
import std.string;
import std.core;

// === Configuration ===
#define EXPECTED_VERSION 3
#define MAX_STRING_LENGTH 65535

// === String Structure ===

struct String {
    u32 length [[hidden, comment("String byte length")]];
    if (length > MAX_STRING_LENGTH) {
        std::warning(std::format("Unusually large string length: {} bytes", length));
    }
    char data[length] [[comment("UTF-8 string data")]];
} [[sealed, format("format_string"), comment("Length-prefixed UTF-8 string")]];

fn format_string(String s) {
    return s.data;
};

// === Metadata Structure ===

struct Metadata {
    String title [[comment("Book title")]];
    String author [[comment("Book author")]];
    String coverItemHref [[comment("Path to cover image")]];
    String textReferenceHref [[comment("Path to guided first text reference")]];
} [[comment("Book metadata information")]];

// === Spine Entry Structure ===

struct SpineEntry {
    String href [[comment("Resource path")]];
    u32 cumulativeSize [[comment("Cumulative size in bytes"), color("FF6B6B")]];
    s16 tocIndex [[comment("Index into TOC (-1 if none)"), color("4ECDC4")]];
} [[comment("Spine entry defining reading order")]];

// === TOC Entry Structure ===

struct TocEntry {
    String title [[comment("Chapter/section title")]];
    String href [[comment("Resource path")]];
    String anchor [[comment("Fragment identifier")]];
    u8 level [[comment("Nesting level (0-255)"), color("95E1D3")]];
    s16 spineIndex [[comment("Index into spine (-1 if none)"), color("F38181")]];
} [[comment("Table of contents entry")]];

// === Book Bin Structure ===

struct BookBin {
    // Header
    u8 version [[comment("Format version"), color("FFD93D")]];
    
    // Version validation
    if (version != EXPECTED_VERSION) {
        std::error(std::format("Unsupported version: {} (expected {})", version, EXPECTED_VERSION));
    }
    
    u32 lutOffset [[comment("Offset to lookup tables"), color("6BCB77")]];
    u16 spineCount [[comment("Number of spine entries"), color("4D96FF")]];
    u16 tocCount [[comment("Number of TOC entries"), color("FF6B9D")]];
    
    // Metadata section
    Metadata metadata [[comment("Book metadata")]];
    
    // Validate LUT offset alignment
    u32 currentOffset = $;
    if (currentOffset != lutOffset) {
        std::warning(std::format("LUT offset mismatch: expected 0x{:X}, got 0x{:X}", lutOffset, currentOffset));
    }
    
    // Lookup Tables
    u32 spineLut[spineCount] [[comment("Spine entry offsets"), color("4D96FF")]];
    u32 tocLut[tocCount] [[comment("TOC entry offsets"), color("FF6B9D")]];
    
    // Data Entries
    SpineEntry spines[spineCount] [[comment("Spine entries (reading order)")]];
    TocEntry toc[tocCount] [[comment("Table of contents entries")]];
};

// === File Parsing ===

BookBin book @ 0x00;

// Validate we've consumed the entire file
u32 fileSize = std::mem::size();
u32 parsedSize = $;

if (parsedSize != fileSize) {
    std::warning(std::format("Unparsed data detected: {} bytes remaining at offset 0x{:X}", fileSize - parsedSize, parsedSize));
}
```

## `section.bin`

> 現在の形式は **Version 43**（`lib/Epub/Epub/Section.cpp` の `SECTION_FILE_VERSION`。
> バージョンごとの変更理由は同ファイル冒頭のコメントにある）。以下の ImHex パターンは
> Version 8 のときのもので、ブロックスタイルなど以降に追加されたフィールドを含まない。

### Version 8

ImHex Pattern:

```c++
import std.mem;
import std.string;
import std.core;

// === Configuration ===
#define EXPECTED_VERSION 8
#define MAX_STRING_LENGTH 65535

// === String Structure ===

struct String {
    u32 length [[hidden, comment("String byte length")]];
    if (length > MAX_STRING_LENGTH) {
        std::warning(std::format("Unusually large string length: {} bytes", length));
    }
    char data[length] [[comment("UTF-8 string data")]];
} [[sealed, format("format_string"), comment("Length-prefixed UTF-8 string")]];

fn format_string(String s) {
    return s.data;
};

// === Page Structure ===

enum StorageType : u8 {
    PageLine = 1
};

enum WordStyle : u8 {
    REGULAR = 0,
    BOLD = 1,
    ITALIC = 2,
    BOLD_ITALIC = 3
};

enum BlockStyle : u8 {
    JUSTIFIED = 0,
    LEFT_ALIGN = 1,
    CENTER_ALIGN = 2,
    RIGHT_ALIGN = 3,
};

struct PageLine {
  s16 xPos;
  s16 yPos;
  u16 wordCount;
  String words[wordCount];
  u16 wordXPos[wordCount];
  WordStyle wordStyle[wordCount];
  BlockStyle blockStyle;
};

struct PageElement {
    u8 pageElementType;
    if (pageElementType == 1) {
        PageLine pageLine [[inline]];
    } else {
        std::error(std::format("Unknown page element type: {}", pageElementType));
    }
};

struct Page {
    u16 elementCount;
    PageElement elements[elementCount] [[inline]];
};

// === Section Bin Structure ===

struct SectionBin {
    // Header
    u8 version [[comment("Format version"), color("FFD93D")]];
    
    // Version validation
    if (version != EXPECTED_VERSION) {
        std::error(std::format("Unsupported version: {} (expected {})", version, EXPECTED_VERSION));
    }
    
    // Cache busting parameters
    s32 fontId;
    float lineCompression;
    bool extraParagraphSpacing;
    u16 viewportWidth;
    u16 vieportHeight;
    u16 pageCount;
    u32 lutOffset;
    
    Page page[pageCount];
    
    // Validate LUT offset alignment
    u32 currentOffset = $;
    if (currentOffset != lutOffset) {
        std::warning(std::format("LUT offset mismatch: expected 0x{:X}, got 0x{:X}", lutOffset, currentOffset));
    }
    
    // Lookup Tables
    u32 lut[pageCount];
};

// === File Parsing ===

SectionBin book @ 0x00;

// Validate we've consumed the entire file
u32 fileSize = std::mem::size();
u32 parsedSize = $;

if (parsedSize != fileSize) {
    std::warning(std::format("Unparsed data detected: {} bytes remaining at offset 0x{:X}", fileSize - parsedSize, parsedSize));
}
```

## `progress.bin`

読書位置と読了フラグ、本全体の進捗率を保存する。書籍ごとのキャッシュディレクトリ
（`epub_<hash>/` `xtc_<hash>/` `txt_<hash>/`）の直下に置かれる。バージョン番号は持たず、
**末尾にフィールドを追記する**ことでのみ拡張する。読む側は「読めたバイト数」で有無を判定し、
短いファイル（旧形式）でも動作する。

進捗率は `ReadingStatusHelper` が book.bin を開かずにホーム画面へ表示するために、
リーダーがページ描画時に計算した値（0〜100）をそのまま保存したもの。
`255` は「未保存」（進捗率フィールド追加前に書かれたファイルと同じ扱い）。

### EPUB（8 バイト）

| Offset | Size | 内容 |
|--------|------|------|
| 0 | 2 | spineIndex（リトルエンディアン） |
| 2 | 2 | 章内ページ番号 |
| 4 | 2 | 章のページ数（ページ数が変わったときの位置補正に使う） |
| 6 | 1 | 読了フラグ（1 = 読了。本の 95% 以降で立つ） |
| 7 | 1 | 進捗率 0〜100、255 = 未保存 |

旧形式は 4 / 6 / 7 バイト。

### XTC（6 バイト）

| Offset | Size | 内容 |
|--------|------|------|
| 0 | 4 | ページ番号（リトルエンディアン） |
| 4 | 1 | 読了フラグ |
| 5 | 1 | 進捗率 0〜100、255 = 未保存 |

旧形式は 5 バイト。

### TXT / Markdown（5 バイト）

| Offset | Size | 内容 |
|--------|------|------|
| 0 | 2 | ページ番号（リトルエンディアン） |
| 2 | 2 | 予約（0） |
| 4 | 1 | 進捗率 0〜100、255 = 未保存 |

旧形式は 4 バイト。読了フラグは無い。

## `.aozora_index.bin`

青空文庫のダウンロード履歴。SD カード上の `/Aozora/.aozora_index.bin` に固定長の
append-only レコードで保存する。実装は [src/AozoraIndexManager.cpp](../src/AozoraIndexManager.cpp)。

削除は tombstone マーク（status バイトの書き換え）で行うため、レコードのオフセットは
一度決まったら変わらない。`AozoraIndexManager` はメモリ上に workId のソート済み配列と
アクティブレコードのオフセット配列のみを保持し、title などの詳細は表示時にファイルから
ページ単位で読み出す。

### Header (8 bytes)

| Offset | Size | 内容 |
|--------|------|------|
| 0 | 4 | magic `"AZBI"` |
| 4 | 1 | format version |
| 5 | 3 | reserved (0x00) |

### Record

`status(1) + entry` の固定長。status は `0xA5` = active、`0x00` = tombstone。
（`0x00` と `0xFF` のいずれも active と衝突しない値を選んでいる）

#### Version 2 (current) — record size 249 bytes

| Offset | Size | フィールド |
|--------|------|-----------|
| 0 | 4 | `int32_t workId`（リトルエンディアン） |
| 4 | 64 | `char title[64]` |
| 68 | 32 | `char author[32]` |
| 100 | 80 | `char filename[80]`（`/Aozora` からの相対パス） |
| 180 | 48 | `char subtitle[48]` |
| 228 | 20 | `char variant[20]`（文字遣い。「新字新仮名」等 UTF-8 15 バイト） |

#### Version 1 (legacy) — record size 181 bytes

v2 の先頭 180 バイトと完全に一致する（`workId` / `title` / `author` / `filename` のみ）。

起動時に version 1 を検出すると `migrateBinV1ToV2_()` が `.bin.tmp` へ v2 形式で書き出し、
rename による atomic swap で置き換える。tombstone レコードはこの機会に落とされる。
マイグレーション中に電源が落ちても元の v1 ファイルは無傷で、次回起動時に `.tmp` が掃除される。

### バージョン更新ルール

レコードのバイナリ構造を変更する場合は、変更前に `BIN_HEADER_VERSION` を
インクリメントし、旧バージョンからのマイグレーションを追加すること。
契約は [test/aozora_index/AozoraIndexTest.cpp](../test/aozora_index/AozoraIndexTest.cpp) の
ホストテストで固定されている（`./test/run_aozora_index_test.sh` で実行）。

不明なバージョンや magic 不一致を検出した場合は bin を破棄し、
`/Aozora/著者名/workId_タイトル.epub` の配置から `rebuildFromDirectoryScan_()` で再構築する。
この経路では副題・文字遣いは復元できず空欄になる。
