#include "ChapterHtmlSlimParser.h"

#include <Arduino.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Utf8.h>
#include <expat.h>

#include "../../Epub.h"
#include "../InlineImage.h"
#include "../Page.h"
#include "../SectionBuildPerf.h"
#include "../blocks/TableRowBlock.h"
#include "../blocks/TextBlock.h"
#include "../converters/ImageDecoderFactory.h"
#include "../converters/ImageToFramebufferDecoder.h"
#include "../htmlEntities.h"

const char* HEADER_TAGS[] = {"h1", "h2", "h3", "h4", "h5", "h6"};
constexpr int NUM_HEADER_TAGS = sizeof(HEADER_TAGS) / sizeof(HEADER_TAGS[0]);

// Minimum file size (in bytes) to show indexing popup - smaller chapters don't benefit from it
constexpr size_t MIN_SIZE_FOR_POPUP = 10 * 1024;  // 10KB
constexpr size_t PARSE_BUFFER_SIZE = 1024;
// Minimum free heap to continue parsing. Below this, stop gracefully
// to prevent abort() from failed allocations (no C++ exceptions on ESP32).
constexpr size_t MIN_FREE_HEAP_FOR_PARSING = 20 * 1024;  // 20KB

// words vector の capacity 上限に近付いたら中間 flush する閾値。
// ParsedText::addWord() は 512 要素で reserve するため、その手前で flush する必要がある。
// 超えると _M_realloc_append が発生し、1024 要素分の連続領域（~32KB）を要求してヒープ不足で abort する。
// 青空文庫のように <p> を持たない超長段落の EPUB で顕在化する（Issue #72）。
constexpr size_t MID_BLOCK_FLUSH_WORDS = 400;

const char* BLOCK_TAGS[] = {"p", "li", "div", "br", "blockquote"};
constexpr int NUM_BLOCK_TAGS = sizeof(BLOCK_TAGS) / sizeof(BLOCK_TAGS[0]);

const char* BOLD_TAGS[] = {"b", "strong"};
constexpr int NUM_BOLD_TAGS = sizeof(BOLD_TAGS) / sizeof(BOLD_TAGS[0]);

const char* ITALIC_TAGS[] = {"i", "em"};
constexpr int NUM_ITALIC_TAGS = sizeof(ITALIC_TAGS) / sizeof(ITALIC_TAGS[0]);

const char* UNDERLINE_TAGS[] = {"u", "ins"};
constexpr int NUM_UNDERLINE_TAGS = sizeof(UNDERLINE_TAGS) / sizeof(UNDERLINE_TAGS[0]);

const char* IMAGE_TAGS[] = {"img"};
constexpr int NUM_IMAGE_TAGS = sizeof(IMAGE_TAGS) / sizeof(IMAGE_TAGS[0]);

const char* SKIP_TAGS[] = {"head", "rp"};
constexpr int NUM_SKIP_TAGS = sizeof(SKIP_TAGS) / sizeof(SKIP_TAGS[0]);

bool isWhitespace(const char c) { return c == ' ' || c == '\r' || c == '\n' || c == '\t'; }

// <pre> の中でタブ 1 つを何個の空白として扱うか。画面幅が狭いので 8 ではなく 4 にする。
constexpr int PRE_TAB_WIDTH = 4;

// Check if a Unicode codepoint is an invisible/zero-width character that should be skipped
bool isInvisibleCodepoint(const uint32_t cp) {
  if (cp == 0xFEFF) return true;                  // BOM / Zero Width No-Break Space
  if (cp == 0x200B) return true;                  // Zero Width Space
  if (cp == 0x200C) return true;                  // Zero Width Non-Joiner
  if (cp == 0x200D) return true;                  // Zero Width Joiner
  if (cp == 0x200E) return true;                  // Left-to-Right Mark
  if (cp == 0x200F) return true;                  // Right-to-Left Mark
  if (cp == 0x2060) return true;                  // Word Joiner
  if (cp == 0x00AD) return true;                  // Soft Hyphen
  if (cp == 0x034F) return true;                  // Combining Grapheme Joiner
  if (cp == 0x061C) return true;                  // Arabic Letter Mark
  if (cp >= 0x2066 && cp <= 0x2069) return true;  // Directional isolates
  if (cp >= 0x202A && cp <= 0x202E) return true;  // Directional formatting
  return false;
}

// Check if a Unicode codepoint is CJK (Chinese/Japanese/Korean)
// CJK characters should be treated as individual "words" for line breaking
bool isCjkCodepointForSplit(const uint32_t cp) {
  // CJK Unified Ideographs: U+4E00 - U+9FFF
  if (cp >= 0x4E00 && cp <= 0x9FFF) return true;
  // CJK Unified Ideographs Extension A: U+3400 - U+4DBF
  if (cp >= 0x3400 && cp <= 0x4DBF) return true;
  // CJK Punctuation: U+3000 - U+303F
  if (cp >= 0x3000 && cp <= 0x303F) return true;
  // Hiragana: U+3040 - U+309F
  if (cp >= 0x3040 && cp <= 0x309F) return true;
  // Katakana: U+30A0 - U+30FF
  if (cp >= 0x30A0 && cp <= 0x30FF) return true;
  // CJK Compatibility Ideographs: U+F900 - U+FAFF
  if (cp >= 0xF900 && cp <= 0xFAFF) return true;
  // Fullwidth forms: U+FF00 - U+FFEF
  if (cp >= 0xFF00 && cp <= 0xFFEF) return true;
  return false;
}

// 青空文庫では、底本の編集部が補ったルビを亀甲括弧「〔　〕」で括ったまま <rt> に入れている
// （例: 『三四郎』の <rt>〔たし〕</rt>、<rt>〔くん〕じゆ</rt>）。ルビとして描くときは括弧を落とす。
// 0xE3 は UTF-8 のリードバイトで継続バイトには現れないので、バイト列のまま探して詰めてよい。
void stripAozoraRubyBrackets(std::string& ruby) {
  constexpr char OPEN[] = "\xE3\x80\x94";   // 〔 U+3014
  constexpr char CLOSE[] = "\xE3\x80\x95";  // 〕 U+3015
  size_t out = 0;
  for (size_t i = 0; i < ruby.size();) {
    if (ruby.compare(i, 3, OPEN) == 0 || ruby.compare(i, 3, CLOSE) == 0) {
      i += 3;
      continue;
    }
    ruby[out++] = ruby[i++];
  }
  ruby.resize(out);
}

// Get UTF-8 byte length for a lead byte
int getUtf8ByteLength(unsigned char leadByte) {
  if ((leadByte & 0x80) == 0) return 1;     // ASCII: 0xxxxxxx
  if ((leadByte & 0xE0) == 0xC0) return 2;  // 2-byte: 110xxxxx
  if ((leadByte & 0xF0) == 0xE0) return 3;  // 3-byte: 1110xxxx
  if ((leadByte & 0xF8) == 0xF0) return 4;  // 4-byte: 11110xxx
  return 1;                                 // Invalid, treat as single byte
}

// Decode UTF-8 codepoint from bytes
uint32_t decodeUtf8Codepoint(const char* s, int len) {
  if (len <= 0) return 0;
  unsigned char b0 = static_cast<unsigned char>(s[0]);

  if ((b0 & 0x80) == 0) {
    return b0;  // ASCII
  }
  if (len >= 2 && (b0 & 0xE0) == 0xC0) {
    unsigned char b1 = static_cast<unsigned char>(s[1]);
    return ((b0 & 0x1F) << 6) | (b1 & 0x3F);
  }
  if (len >= 3 && (b0 & 0xF0) == 0xE0) {
    unsigned char b1 = static_cast<unsigned char>(s[1]);
    unsigned char b2 = static_cast<unsigned char>(s[2]);
    return ((b0 & 0x0F) << 12) | ((b1 & 0x3F) << 6) | (b2 & 0x3F);
  }
  if (len >= 4 && (b0 & 0xF8) == 0xF0) {
    unsigned char b1 = static_cast<unsigned char>(s[1]);
    unsigned char b2 = static_cast<unsigned char>(s[2]);
    unsigned char b3 = static_cast<unsigned char>(s[3]);
    return ((b0 & 0x07) << 18) | ((b1 & 0x3F) << 12) | ((b2 & 0x3F) << 6) | (b3 & 0x3F);
  }
  return b0;  // Fallback
}

// given the start and end of a tag, check to see if it matches a known tag
bool matches(const char* tag_name, const char* possible_tags[], const int possible_tag_count) {
  for (int i = 0; i < possible_tag_count; i++) {
    if (strcmp(tag_name, possible_tags[i]) == 0) {
      return true;
    }
  }
  return false;
}

const char* getAttribute(const XML_Char** atts, const char* attrName) {
  if (!atts) return nullptr;
  for (int i = 0; atts[i]; i += 2) {
    if (strcmp(atts[i], attrName) == 0) return atts[i + 1];
  }
  return nullptr;
}

bool isInternalEpubLink(const char* href) {
  if (!href || href[0] == '\0') return false;
  if (strncmp(href, "http://", 7) == 0 || strncmp(href, "https://", 8) == 0) return false;
  if (strncmp(href, "mailto:", 7) == 0) return false;
  if (strncmp(href, "ftp://", 6) == 0) return false;
  if (strncmp(href, "tel:", 4) == 0) return false;
  if (strncmp(href, "javascript:", 11) == 0) return false;
  return true;
}

bool isHeaderOrBlock(const char* name) {
  return matches(name, HEADER_TAGS, NUM_HEADER_TAGS) || matches(name, BLOCK_TAGS, NUM_BLOCK_TAGS);
}

bool isTableStructuralTag(const char* name) {
  return strcmp(name, "table") == 0 || strcmp(name, "tr") == 0 || strcmp(name, "td") == 0 || strcmp(name, "th") == 0;
}

// Update effective bold/italic/underline based on block style and inline style stack
void ChapterHtmlSlimParser::updateEffectiveInlineStyle() {
  // Start with block-level styles
  effectiveBold = currentCssStyle.hasFontWeight() && currentCssStyle.fontWeight == CssFontWeight::Bold;
  effectiveItalic = currentCssStyle.hasFontStyle() && currentCssStyle.fontStyle == CssFontStyle::Italic;
  effectiveUnderline =
      currentCssStyle.hasTextDecoration() && currentCssStyle.textDecoration == CssTextDecoration::Underline;

  // Apply inline style stack in order
  for (const auto& entry : inlineStyleStack) {
    if (entry.hasBold) {
      effectiveBold = entry.bold;
    }
    if (entry.hasItalic) {
      effectiveItalic = entry.italic;
    }
    if (entry.hasUnderline) {
      effectiveUnderline = entry.underline;
    }
  }
}

// flush the contents of partWordBuffer to currentTextBlock
void ChapterHtmlSlimParser::flushPartWordBuffer() {
  // Determine font style from depth-based tracking and CSS effective style
  const bool isBold = boldUntilDepth < depth || effectiveBold;
  const bool isItalic = italicUntilDepth < depth || effectiveItalic;
  const bool isUnderline = underlineUntilDepth < depth || effectiveUnderline;
  const bool isSuperscript = superscriptUntilDepth < depth;
  const bool isSubscript = subscriptUntilDepth < depth;

  // Combine style flags using bitwise OR
  EpdFontFamily::Style fontStyle = EpdFontFamily::REGULAR;
  if (isBold) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::BOLD);
  }
  if (isItalic) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::ITALIC);
  }
  if (isUnderline) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::UNDERLINE);
  }
  // <sub> が <sup> の内側にあるような壊れた入れ子では上付きを優先する
  if (isSuperscript) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::SUPERSCRIPT);
  } else if (isSubscript) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::SUBSCRIPT);
  }

  // flush the buffer
  partWordBuffer[partWordBufferIndex] = '\0';
  // リストマーカーの直後の語は必ずくっつける（マーカーと本文の間で折り返さない）
  const bool attachToPrevious = nextWordContinues || listMarkerPending;
  if (verticalMode) {
    // Classify for vertical: count ASCII digits to determine TateChuYoko vs Sideways
    bool allDigits = true;
    int asciiCharCount = 0;
    for (int ci = 0; ci < partWordBufferIndex; ci++) {
      if ((static_cast<uint8_t>(partWordBuffer[ci]) & 0xC0) != 0x80) asciiCharCount++;
      if (partWordBuffer[ci] < '0' || partWordBuffer[ci] > '9') allDigits = false;
    }
    auto vb = VerticalTextUtils::VerticalBehavior::Sideways;  // default for Latin text
    if (allDigits && asciiCharCount <= 2) {
      vb = VerticalTextUtils::VerticalBehavior::TateChuYoko;
    }
    currentTextBlock->addWord(partWordBuffer, fontStyle, vb, false, attachToPrevious, pendingSpace);
  } else {
    currentTextBlock->addWord(partWordBuffer, fontStyle, false, attachToPrevious, pendingSpace);
  }
  partWordBufferIndex = 0;
  nextWordContinues = false;
  pendingSpace = false;
  listMarkerPending = false;
}

// <pre> で保留していた改行を行区切りとして確定させる。
// 2 つ以上続いていた場合は、間の行を空行として出す（コードの空行を潰さないため）。
// 新しい行は枠の左右だけを持ち、上辺・下辺は <pre> の開始と終了で付ける。
// <pre> の 2 行目以降に使うブロックスタイル。
// 枠と中身の間のアキは最初の行と最後の行だけが持つ。CSS の `pre { padding: 0.8em }` の
// ような指定はブロック全体に 1 回かかるべきもので、行ごとに付けると 1 行おきに空白が
// 入り、左右の縦線も途切れる（縦線は 1 行の送りぶんしか引かないため）。
BlockStyle ChapterHtmlSlimParser::preContinuationStyle() const {
  auto lineStyle = currentTextBlock->getBlockStyle();
  lineStyle.frameEdges = static_cast<uint8_t>(lineStyle.frameEdges & ~BlockStyle::FRAME_TOP);
  lineStyle.marginTop = 0;
  lineStyle.paddingTop = 0;
  lineStyle.marginBottom = 0;
  lineStyle.paddingBottom = 0;
  return lineStyle;
}

void ChapterHtmlSlimParser::preFlushPendingNewlines() {
  while (prePendingNewlines > 0) {
    // コードが空行で始まる場合、まだ語の無いこのブロックがそのまま 1 行目になる。
    // ここで新しいブロックに移ると枠の上辺と上のアキを落としてしまうので、
    // 高さだけ確保して次の行へ進む。
    if (currentTextBlock->isEmpty() && (currentTextBlock->getBlockStyle().frameEdges & BlockStyle::FRAME_TOP) != 0) {
      currentTextBlock->addWord(" ", EpdFontFamily::REGULAR);
    }
    startNewTextBlock(preContinuationStyle());
    prePendingNewlines--;
    if (prePendingNewlines > 0) {
      // 空行。語が 1 つも無いブロックはページに積まれないので空白を 1 つ置く。
      currentTextBlock->addWord(" ", EpdFontFamily::REGULAR);
    }
  }
}

// start a new text block if needed
void ChapterHtmlSlimParser::startNewTextBlock(const BlockStyle& blockStyle) {
  nextWordContinues = false;  // New block = new paragraph, no continuation
  pendingSpace = false;
  listMarkerPending = false;
  if (currentTextBlock) {
    // already have a text block running and it is empty - just reuse it
    if (currentTextBlock->isEmpty()) {
      // Merge with existing block style to accumulate CSS styling from parent block elements.
      // This handles cases like <div style="margin-bottom:2em"><h1>text</h1></div> where the
      // div's margin should be preserved, even though it has no direct text content.
      currentTextBlock->setBlockStyle(currentTextBlock->getBlockStyle().getCombinedBlockStyle(blockStyle));

      if (!pendingAnchorId.empty()) {
        anchorData.push_back({std::move(pendingAnchorId), static_cast<uint16_t>(completedPageCount)});
        pendingAnchorId.clear();
      }
      return;
    }

    makePages();
  }
  // Record deferred anchor after previous block is flushed
  if (!pendingAnchorId.empty()) {
    anchorData.push_back({std::move(pendingAnchorId), static_cast<uint16_t>(completedPageCount)});
    pendingAnchorId.clear();
  }
  currentTextBlock.reset(new ParsedText(hyphenationEnabled, blockStyle, firstLineIndent));
  wordsExtractedInBlock = 0;
}

void XMLCALL ChapterHtmlSlimParser::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);

  // Middle of skip
  if (self->skipUntilDepth < self->depth) {
    self->depth += 1;
    return;
  }

  // <ol> / <ul>: <li> のマーカーを連番にするか中黒にするかを決めるためにネストを記録する。
  // 要素自体は中身を持たないので、この後の分岐はそのまま通す。display:none の判定より
  // 手前に置いてあるのは、endElement 側の pop と対称にするため（隠しリストも push する）。
  if (strcmp(name, "ol") == 0 || strcmp(name, "ul") == 0) {
    const bool ordered = strcmp(name, "ol") == 0;
    uint16_t start = 1;
    if (ordered && atts != nullptr) {
      for (int i = 0; atts[i]; i += 2) {
        if (strcmp(atts[i], "start") == 0) {
          const long v = strtol(atts[i + 1], nullptr, 10);
          if (v > 0 && v <= 9999) start = static_cast<uint16_t>(v);
        }
      }
    }
    if (self->listDepth < MAX_LIST_NESTING) {
      self->listStack[self->listDepth].ordered = ordered;
      self->listStack[self->listDepth].counter = start;
    }
    self->listDepth++;
  }

  // Extract class, style, and id attributes
  std::string classAttr;
  std::string styleAttr;
  if (atts != nullptr) {
    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "class") == 0) {
        classAttr = atts[i + 1];
      } else if (strcmp(atts[i], "style") == 0) {
        styleAttr = atts[i + 1];
      } else if (strcmp(atts[i], "id") == 0) {
        // Defer recording until startNewTextBlock, after previous block is flushed to pages
        self->pendingAnchorId = atts[i + 1];
      }
    }
  }

  auto centeredBlockStyle = BlockStyle();
  centeredBlockStyle.textAlignDefined = true;
  centeredBlockStyle.alignment = CssTextAlign::Center;

  // Compute CSS style for this element early so display:none can short-circuit
  // before tag-specific branches emit any content or metadata.
  CssStyle cssStyle;
  if (self->cssParser) {
    cssStyle = self->cssParser->resolveStyle(name, classAttr);
    if (!styleAttr.empty()) {
      CssStyle inlineStyle = CssParser::parseInlineStyle(styleAttr);
      cssStyle.applyOver(inlineStyle);
    }
  }

  // Skip elements with display:none before all fast paths (tables, links, etc.).
  if (cssStyle.hasDisplay() && cssStyle.display == CssDisplay::None) {
    self->skipUntilDepth = self->depth;
    self->depth += 1;
    return;
  }

  // Special handling for tables: buffer cell data for grid rendering.
  if (strcmp(name, "table") == 0) {
    if (self->tableDepth > 0) {
      self->tableDepth += 1;
      self->depth += 1;
      return;
    }

    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
    }
    if (self->currentTextBlock && !self->currentTextBlock->isEmpty()) {
      self->makePages();
    }
    self->tableDepth += 1;
    self->tableRowIndex = 0;
    self->tableColIndex = 0;
    self->tableBuffer.clear();
    self->depth += 1;
    return;
  }

  if (self->tableDepth == 1 && strcmp(name, "tr") == 0) {
    self->tableRowIndex += 1;
    self->tableColIndex = 0;
    self->tableBuffer.emplace_back();
    self->depth += 1;
    return;
  }

  if (self->tableDepth == 1 && (strcmp(name, "td") == 0 || strcmp(name, "th") == 0)) {
    self->tableColIndex += 1;
    self->tableCellTextBuffer.clear();
    self->tableCellIsHeader = (strcmp(name, "th") == 0);
    self->depth += 1;
    return;
  }

  if (matches(name, IMAGE_TAGS, NUM_IMAGE_TAGS)) {
    SECTION_PERF_SCOPE(imageUs);
    std::string src;
    std::string alt;
    if (atts != nullptr) {
      for (int i = 0; atts[i]; i += 2) {
        if (strcmp(atts[i], "src") == 0) {
          src = atts[i + 1];
        } else if (strcmp(atts[i], "alt") == 0) {
          alt = atts[i + 1];
        }
      }

      // imageRendering: 0=display, 1=placeholder (alt text only), 2=suppress entirely
      if (self->imageRendering == 2) {
        self->skipUntilDepth = self->depth;
        self->depth += 1;
        return;
      }

      // Skip image if CSS display:none
      if (self->cssParser) {
        CssStyle imgDisplayStyle = self->cssParser->resolveStyle("img", classAttr);
        if (!styleAttr.empty()) {
          imgDisplayStyle.applyOver(CssParser::parseInlineStyle(styleAttr));
        }
        if (imgDisplayStyle.hasDisplay() && imgDisplayStyle.display == CssDisplay::None) {
          self->skipUntilDepth = self->depth;
          self->depth += 1;
          return;
        }
      }

      if (!src.empty() && self->imageRendering != 1) {
        LOG_DBG("EHP", "Found image: src=%s", src.c_str());

        {
          // Resolve the image path relative to the HTML file
          std::string resolvedPath = FsHelpers::normalisePath(self->contentBase + src);

          // If format is not directly supported (e.g. SVG), try same-name fallback
          // with supported extensions (.png, .jpg, .jpeg)
          if (!ImageDecoderFactory::isFormatSupported(resolvedPath)) {
            const size_t dotPos = resolvedPath.rfind('.');
            if (dotPos != std::string::npos) {
              const std::string baseName = resolvedPath.substr(0, dotPos);
              static const char* fallbackExts[] = {".png", ".jpg", ".jpeg"};
              for (const auto* ext : fallbackExts) {
                const std::string candidate = baseName + ext;
                size_t candidateSize = 0;
                if (self->epub->getItemSize(candidate, &candidateSize) && candidateSize > 0) {
                  LOG_DBG("EHP", "Using fallback image: %s -> %s", resolvedPath.c_str(), candidate.c_str());
                  resolvedPath = candidate;
                  break;
                }
              }
            }
          }

          if (ImageDecoderFactory::isFormatSupported(resolvedPath)) {
            // Create a unique filename for the cached image
            std::string ext;
            size_t extPos = resolvedPath.rfind('.');
            if (extPos != std::string::npos) {
              ext = resolvedPath.substr(extPos);
            }
            std::string cachedImagePath = self->imageBasePath + std::to_string(self->imageCounter++) + ext;

            // Extract image to cache file
            FsFile cachedImageFile;
            bool extractSuccess = false;
            if (Storage.openFileForWrite("EHP", cachedImagePath, cachedImageFile)) {
              extractSuccess = self->epub->readItemContentsToStream(resolvedPath, cachedImageFile, 4096);
              cachedImageFile.flush();
              cachedImageFile.close();
              delay(50);  // Give SD card time to sync
            }

            if (extractSuccess) {
              // Get image dimensions
              ImageDimensions dims = {0, 0};
              ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(cachedImagePath);
              if (decoder && decoder->getDimensions(cachedImagePath, dims)) {
                LOG_DBG("EHP", "Image dimensions: %dx%d", dims.width, dims.height);

                int displayWidth = 0;
                int displayHeight = 0;
                const float emSize = static_cast<float>(self->renderer.getFontAscenderSize(self->fontId));
                CssStyle imgStyle = self->cssParser ? self->cssParser->resolveStyle("img", classAttr) : CssStyle{};
                // Merge inline style (e.g. style="height: 2em") so it overrides stylesheet rules
                if (!styleAttr.empty()) {
                  imgStyle.applyOver(CssParser::parseInlineStyle(styleAttr));
                }
                const bool hasCssHeight = imgStyle.hasImageHeight();
                const bool hasCssWidth = imgStyle.hasImageWidth();

                if (hasCssHeight && hasCssWidth && dims.width > 0 && dims.height > 0) {
                  // Both CSS height and width set: resolve both as max bounds, then fit image
                  // within those bounds preserving the original aspect ratio. Image decoders use
                  // a single scale factor for both axes; non-uniform scaling causes diagonal distortion.
                  int maxW = static_cast<int>(
                      imgStyle.imageWidth.toPixels(emSize, static_cast<float>(self->viewportWidth)) + 0.5f);
                  int maxH = static_cast<int>(
                      imgStyle.imageHeight.toPixels(emSize, static_cast<float>(self->viewportHeight)) + 0.5f);
                  if (maxW > self->viewportWidth) maxW = self->viewportWidth;
                  if (maxH > self->viewportHeight) maxH = self->viewportHeight;
                  if (maxW < 1) maxW = 1;
                  if (maxH < 1) maxH = 1;
                  float scaleX = static_cast<float>(maxW) / dims.width;
                  float scaleY = static_cast<float>(maxH) / dims.height;
                  float scale = (scaleX < scaleY) ? scaleX : scaleY;
                  displayWidth = static_cast<int>(dims.width * scale + 0.5f);
                  displayHeight = static_cast<int>(dims.height * scale + 0.5f);
                  if (displayWidth < 1) displayWidth = 1;
                  if (displayHeight < 1) displayHeight = 1;
                  LOG_DBG("EHP", "Display size from CSS height+width: %dx%d", displayWidth, displayHeight);
                } else if (hasCssHeight && !hasCssWidth && dims.width > 0 && dims.height > 0) {
                  // Use CSS height (resolve % against viewport height) and derive width from aspect ratio
                  displayHeight = static_cast<int>(
                      imgStyle.imageHeight.toPixels(emSize, static_cast<float>(self->viewportHeight)) + 0.5f);
                  if (displayHeight < 1) displayHeight = 1;
                  displayWidth =
                      static_cast<int>(displayHeight * (static_cast<float>(dims.width) / dims.height) + 0.5f);
                  if (displayHeight > self->viewportHeight) {
                    displayHeight = self->viewportHeight;
                    // Rescale width to preserve aspect ratio when height is clamped
                    displayWidth =
                        static_cast<int>(displayHeight * (static_cast<float>(dims.width) / dims.height) + 0.5f);
                    if (displayWidth < 1) displayWidth = 1;
                  }
                  if (displayWidth > self->viewportWidth) {
                    displayWidth = self->viewportWidth;
                    // Rescale height to preserve aspect ratio when width is clamped
                    displayHeight =
                        static_cast<int>(displayWidth * (static_cast<float>(dims.height) / dims.width) + 0.5f);
                    if (displayHeight < 1) displayHeight = 1;
                  }
                  if (displayWidth < 1) displayWidth = 1;
                  LOG_DBG("EHP", "Display size from CSS height: %dx%d", displayWidth, displayHeight);
                } else if (hasCssWidth && !hasCssHeight && dims.width > 0 && dims.height > 0) {
                  // Use CSS width (resolve % against viewport width) and derive height from aspect ratio
                  displayWidth = static_cast<int>(
                      imgStyle.imageWidth.toPixels(emSize, static_cast<float>(self->viewportWidth)) + 0.5f);
                  if (displayWidth > self->viewportWidth) displayWidth = self->viewportWidth;
                  if (displayWidth < 1) displayWidth = 1;
                  displayHeight =
                      static_cast<int>(displayWidth * (static_cast<float>(dims.height) / dims.width) + 0.5f);
                  if (displayHeight > self->viewportHeight) {
                    displayHeight = self->viewportHeight;
                    // Rescale width to preserve aspect ratio when height is clamped
                    displayWidth =
                        static_cast<int>(displayHeight * (static_cast<float>(dims.width) / dims.height) + 0.5f);
                    if (displayWidth < 1) displayWidth = 1;
                  }
                  if (displayHeight < 1) displayHeight = 1;
                  LOG_DBG("EHP", "Display size from CSS width: %dx%d", displayWidth, displayHeight);
                } else {
                  // Scale to fit viewport while maintaining aspect ratio
                  int maxWidth = self->viewportWidth;
                  int maxHeight = self->viewportHeight;
                  float scaleX = (dims.width > maxWidth) ? (float)maxWidth / dims.width : 1.0f;
                  float scaleY = (dims.height > maxHeight) ? (float)maxHeight / dims.height : 1.0f;
                  float scale = (scaleX < scaleY) ? scaleX : scaleY;
                  if (scale > 1.0f) scale = 1.0f;

                  displayWidth = (int)(dims.width * scale);
                  displayHeight = (int)(dims.height * scale);
                  LOG_DBG("EHP", "Display size: %dx%d (scale %.2f)", displayWidth, displayHeight, scale);
                }

                // 縦書きで、1 文字ぶん程度の小さい画像（外字の代用など）は、
                // ブロックとして置かずに本文の「語」として流す。ブロックにすると段落が
                // そこで分断され、画像だけが本文と無関係な位置に置かれる（Issue #107）。
                // 本来は CSS の display で判定すべきだが、パーサはその情報を持たないので
                // 大きさ（縦横とも行高の 2 倍以内）で代用する。横書きは従来どおりブロック。
                if (self->verticalMode && self->currentTextBlock) {
                  const int lineHeight = self->renderer.getLineHeight(self->fontId);
                  const int inlineMaxSide = lineHeight * 2;
                  if (displayWidth <= inlineMaxSide && displayHeight <= inlineMaxSide) {
                    // 列に収める都合で、幅が列幅を超える画像はアスペクト比を保って列幅に縮める
                    // （列間隔は列幅の 1/4 しかなく、はみ出すと両隣の列の文字に重なる）
                    const int columnWidth = static_cast<int>(lineHeight * self->lineCompression);
                    if (columnWidth > 0 && displayWidth > columnWidth) {
                      displayHeight =
                          static_cast<int>(displayHeight * (static_cast<float>(columnWidth) / displayWidth) + 0.5f);
                      if (displayHeight < 1) displayHeight = 1;
                      displayWidth = columnWidth;
                    }
                    if (self->partWordBufferIndex > 0) {
                      self->flushPartWordBuffer();
                    }
                    self->currentTextBlock->addWord(InlineImage::encode(cachedImagePath, displayWidth, displayHeight),
                                                    EpdFontFamily::REGULAR,
                                                    VerticalTextUtils::VerticalBehavior::Upright);
                    LOG_DBG("EHP", "Inline image: %dx%d", displayWidth, displayHeight);
                    self->depth += 1;
                    return;
                  }
                }

                // Flush any pending text block so it appears before the image
                if (self->partWordBufferIndex > 0) {
                  self->flushPartWordBuffer();
                }
                if (self->currentTextBlock && !self->currentTextBlock->isEmpty()) {
                  const BlockStyle parentBlockStyle = self->currentTextBlock->getBlockStyle();
                  self->startNewTextBlock(parentBlockStyle);
                }

                // Create ImageBlock and add to page
                auto imageBlock = std::make_shared<ImageBlock>(cachedImagePath, displayWidth, displayHeight);
                if (!imageBlock) {
                  LOG_ERR("EHP", "Failed to create ImageBlock");
                  return;
                }

                if (self->verticalMode) {
                  // 縦書き: ブロック画像は列と同じく右から左へ並べ、幅ぶんの列を消費する。
                  // 横書きの「行送り方向に積み、行内方向は中央」に対応させ、
                  // 行送り方向（x）は現在の列位置に右端を合わせ、行内方向（y）は中央に置く。
                  const int lineHeight = self->renderer.getLineHeight(self->fontId) * self->lineCompression;
                  const int columnWidth = lineHeight;
                  const int columnSpacing = columnWidth / 4;
                  if (!self->currentPage) {
                    self->currentPage.reset(new Page());
                    if (!self->currentPage) {
                      LOG_ERR("EHP", "Failed to create initial page");
                      return;
                    }
                    self->currentPageNextX = self->viewportWidth - columnWidth;
                  }
                  // 現在の列の右端。残りの幅に収まらなければ改ページ
                  int rightEdge = self->currentPageNextX + columnWidth;
                  if (!self->currentPage->elements.empty() && displayWidth > rightEdge) {
                    self->completePageFn(std::move(self->currentPage));
                    self->completedPageCount++;
                    self->currentPage.reset(new Page());
                    if (!self->currentPage) {
                      LOG_ERR("EHP", "Failed to create new page");
                      return;
                    }
                    rightEdge = self->viewportWidth;
                  }
                  int xPos = rightEdge - displayWidth;
                  if (xPos < 0) xPos = 0;
                  const int yPos = (self->viewportHeight - displayHeight) / 2;
                  auto pageImage = std::make_shared<PageImage>(imageBlock, static_cast<int16_t>(xPos),
                                                               static_cast<int16_t>(yPos > 0 ? yPos : 0));
                  if (!pageImage) {
                    LOG_ERR("EHP", "Failed to create PageImage");
                    return;
                  }
                  self->currentPage->elements.push_back(pageImage);
                  // 次の列は画像の左（列間隔ぶん空ける）。負になれば addLineToPage が改ページする
                  self->currentPageNextX = xPos - columnSpacing - columnWidth;

                  self->depth += 1;
                  return;
                }

                // Create page for image - only break if image won't fit remaining space
                if (self->currentPage && !self->currentPage->elements.empty() &&
                    (self->currentPageNextY + displayHeight > self->viewportHeight)) {
                  self->completePageFn(std::move(self->currentPage));
                  self->completedPageCount++;
                  self->currentPage.reset(new Page());
                  if (!self->currentPage) {
                    LOG_ERR("EHP", "Failed to create new page");
                    return;
                  }
                  self->currentPageNextY = 0;
                } else if (!self->currentPage) {
                  self->currentPage.reset(new Page());
                  if (!self->currentPage) {
                    LOG_ERR("EHP", "Failed to create initial page");
                    return;
                  }
                  self->currentPageNextY = 0;
                }

                int xPos = (self->viewportWidth - displayWidth) / 2;
                auto pageImage = std::make_shared<PageImage>(imageBlock, xPos, self->currentPageNextY);
                if (!pageImage) {
                  LOG_ERR("EHP", "Failed to create PageImage");
                  return;
                }
                self->currentPage->elements.push_back(pageImage);
                self->currentPageNextY += displayHeight;

                self->depth += 1;
                return;
              } else {
                LOG_ERR("EHP", "Failed to get image dimensions");
                Storage.remove(cachedImagePath.c_str());
              }
            } else {
              LOG_ERR("EHP", "Failed to extract image");
            }
          }  // isFormatSupported
        }
      }

      // Fallback to alt text or placeholder if image processing fails
      {
        std::string placeholder = alt.empty() ? "[Image]" : "[Image: " + alt + "]";
        self->startNewTextBlock(centeredBlockStyle);
        self->italicUntilDepth = std::min(self->italicUntilDepth, self->depth);
        self->depth += 1;
        self->characterData(userData, placeholder.c_str(), placeholder.length());
        // Skip any child content (skip until parent as we pre-advanced depth above)
        self->skipUntilDepth = self->depth - 1;
        return;
      }
    }
  }

  // Ruby tag handling
  if (strcmp(name, "ruby") == 0) {
    self->flushPartWordBuffer();
    self->inRuby = true;
    self->rubyStartWordIndex = self->currentTextBlock ? static_cast<int>(self->currentTextBlock->size()) : 0;
    self->rubyTextBuffer.clear();
    self->depth += 1;
    return;
  }
  if (strcmp(name, "rt") == 0) {
    self->flushPartWordBuffer();
    self->collectingRubyText = true;
    self->depth += 1;
    return;
  }

  if (matches(name, SKIP_TAGS, NUM_SKIP_TAGS)) {
    // start skip
    self->skipUntilDepth = self->depth;
    self->depth += 1;
    return;
  }

  // Skip blocks with role="doc-pagebreak" and epub:type="pagebreak"
  if (atts != nullptr) {
    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "role") == 0 && strcmp(atts[i + 1], "doc-pagebreak") == 0 ||
          strcmp(atts[i], "epub:type") == 0 && strcmp(atts[i + 1], "pagebreak") == 0) {
        self->skipUntilDepth = self->depth;
        self->depth += 1;
        return;
      }
    }
  }

  // Detect internal <a href="..."> links (footnotes, cross-references)
  // Note: <aside epub:type="footnote"> elements are rendered as normal content
  // without special handling. Links pointing to them are collected as footnotes.
  // <pre>: 空白と改行を原文のまま出す。行は左揃えで、自動字下げもハイフネーションも止める。
  // 等幅フォントは持っていないので字送りは揃わないが、1 段落に潰れてしまう今までよりは読める。
  if (strcmp(name, "pre") == 0) {
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
    }
    const auto preEmSize = static_cast<float>(self->renderer.getFontAscenderSize(self->fontId));
    auto preStyle = BlockStyle::fromCssStyle(cssStyle, preEmSize, CssTextAlign::Left, self->viewportWidth);
    preStyle.alignment = CssTextAlign::Left;
    preStyle.textAlignDefined = true;
    // textIndentDefined を立てて字下げ 0 にすることで、段落頭の自動インデント
    // （EmSpace の挿入や CJK 1 文字ぶんの字下げ）を抑える。
    preStyle.textIndent = 0;
    preStyle.textIndentDefined = true;
    preStyle.frameEdges = BlockStyle::FRAME_SIDES | BlockStyle::FRAME_TOP;
    // 枠の前後の余白。marginTop/Bottom はブロック単位で効くので、
    // 最初の行に上、最後の行（</pre> の処理）に下を付ける。
    preStyle.marginTop = static_cast<int16_t>(preStyle.marginTop + self->renderer.getLineHeight(self->fontId) / 3);
    // 枠線と本文がくっつかないように左右に余白を取る（枠は viewport の端に引かれる）
    const auto framePadding = static_cast<int16_t>(std::max(4, self->renderer.getLineHeight(self->fontId) / 4));
    preStyle.paddingLeft = static_cast<int16_t>(preStyle.paddingLeft + framePadding);
    preStyle.paddingRight = static_cast<int16_t>(preStyle.paddingRight + framePadding);
    self->prePendingNewlines = 0;
    const bool outermostPre = (self->preUntilDepth == INT_MAX);
    if (outermostPre) {
      self->preSavedHyphenation = self->hyphenationEnabled;
      self->hyphenationEnabled = false;
    }
    self->preUntilDepth = std::min(self->preUntilDepth, self->depth);
    self->preSkipLeadingNewline = true;
    self->startNewTextBlock(preStyle);
    // 下のアキは最後の行に付け替える。ここで実際のブロックから読み直すのは、
    // startNewTextBlock が空ブロックを使い回すときに親（<div class="sourceCode"> など）の
    // アキを足し込むため。入れ子の <pre>（不正な HTML）では外側の値を上書きしないよう、
    // いちばん外側の <pre> でだけ退避する。
    if (self->currentTextBlock && outermostPre) {
      auto firstLineStyle = self->currentTextBlock->getBlockStyle();
      self->preSavedMarginBottom = firstLineStyle.marginBottom;
      self->preSavedPaddingBottom = firstLineStyle.paddingBottom;
      firstLineStyle.marginBottom = 0;
      firstLineStyle.paddingBottom = 0;
      self->currentTextBlock->setBlockStyle(firstLineStyle);
    }
    self->updateEffectiveInlineStyle();
    self->depth += 1;
    return;
  }

  // <hr>: 場面転換などの区切り線。h1/h2 の下線と同じ仕組み（drawSeparatorBelow）で描く。
  // 語が 1 つも無いブロックはページに積まれないので、幅ゼロの全角スペースではなく
  // 半角スペースを 1 つ持たせて 1 行分の高さを確保する。
  // 縦書きでは TextBlock::render が横罫を抑止するので、空き 1 列になる。
  if (strcmp(name, "hr") == 0) {
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
    }
    self->startNewTextBlock(BlockStyle());
    BlockStyle hrStyle;
    hrStyle.drawSeparatorBelow = true;
    self->addLineToPage(std::make_shared<TextBlock>(std::vector<std::string>{" "}, std::vector<int16_t>{0},
                                                    std::vector<EpdFontFamily::Style>{EpdFontFamily::REGULAR},
                                                    hrStyle));
    self->depth += 1;
    return;
  }

  if (strcmp(name, "a") == 0) {
    const char* href = getAttribute(atts, "href");

    bool isInternalLink = isInternalEpubLink(href);

    // Special case: javascript:void(0) links with data attributes
    // Example: <a href="javascript:void(0)"
    // data-xyz="{&quot;name&quot;:&quot;OPS/ch2.xhtml&quot;,&quot;frag&quot;:&quot;id46&quot;}">
    if (href && strncmp(href, "javascript:", 11) == 0) {
      isInternalLink = false;
      // TODO: Parse data-* attributes to extract actual href
    }

    if (isInternalLink) {
      // Flush buffer before style change
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
        self->nextWordContinues = true;
      }
      self->insideFootnoteLink = true;
      self->footnoteLinkDepth = self->depth;
      strncpy(self->currentFootnoteLinkHref, href, sizeof(self->currentFootnoteLinkHref) - 1);
      self->currentFootnoteLinkHref[sizeof(self->currentFootnoteLinkHref) - 1] = '\0';
      self->currentFootnoteLinkText[0] = '\0';
      self->currentFootnoteLinkTextLen = 0;

      // Apply underline style to visually indicate the link
      self->underlineUntilDepth = std::min(self->underlineUntilDepth, self->depth);
      StyleStackEntry entry;
      entry.depth = self->depth;
      entry.hasUnderline = true;
      entry.underline = true;
      self->inlineStyleStack.push_back(entry);
      self->updateEffectiveInlineStyle();

      // Skip CSS resolution — we already handled styling for this <a> tag
      self->depth += 1;
      return;
    }
  }

  const float emSize = static_cast<float>(self->renderer.getFontAscenderSize(self->fontId));
  const auto userAlignmentBlockStyle = BlockStyle::fromCssStyle(
      cssStyle, emSize, static_cast<CssTextAlign>(self->paragraphAlignment), self->viewportWidth);

  if (matches(name, HEADER_TAGS, NUM_HEADER_TAGS)) {
    self->currentCssStyle = cssStyle;
    auto headerBlockStyle = BlockStyle::fromCssStyle(
        cssStyle, emSize, static_cast<CssTextAlign>(self->paragraphAlignment), self->viewportWidth);

    // Heading level (h1→1, h2→2, etc.)
    const int level = name[1] - '0';

    // Per-heading fontId override
    if (level >= 1 && level <= 6) {
      headerBlockStyle.fontId = self->headingFontIds[level - 1];
    }

    // Default margins when CSS does not specify them
    const int bodyLineHeight = self->renderer.getLineHeight(self->fontId);
    if (!cssStyle.hasMarginTop()) {
      headerBlockStyle.marginTop = static_cast<int16_t>(bodyLineHeight * 3 / 4);
    }
    if (!cssStyle.hasMarginBottom()) {
      headerBlockStyle.marginBottom = static_cast<int16_t>(bodyLineHeight / 4);
    }

    headerBlockStyle.isHeading = true;

    // Separator line below h1/h2
    if (level <= 2) {
      headerBlockStyle.drawSeparatorBelow = true;
    }

    self->startNewTextBlock(headerBlockStyle);
    self->boldUntilDepth = std::min(self->boldUntilDepth, self->depth);
    self->updateEffectiveInlineStyle();
  } else if (matches(name, BLOCK_TAGS, NUM_BLOCK_TAGS)) {
    if (strcmp(name, "br") == 0) {
      if (self->partWordBufferIndex > 0) {
        // flush word preceding <br/> to currentTextBlock before calling startNewTextBlock
        self->flushPartWordBuffer();
      }
      // <pre> の中の <br/> も 1 行の区切り。スタイルをそのまま複製すると枠の上辺と
      // 上下のアキが行ごとに付いてしまうので、改行と同じ扱いにする。
      self->startNewTextBlock(self->preUntilDepth < self->depth ? self->preContinuationStyle()
                                                                : self->currentTextBlock->getBlockStyle());
    } else {
      self->currentCssStyle = cssStyle;

      if (strcmp(name, "li") == 0) {
        // Force a fresh text block for <li> — the "merge with empty block" path
        // in startNewTextBlock can lose our hanging indent settings.
        if (self->currentTextBlock && self->currentTextBlock->isEmpty()) {
          self->currentTextBlock.reset();
        }
        auto liBlockStyle = userAlignmentBlockStyle;
        liBlockStyle.isListItem = true;

        // リストマーカー。<ol> の中なら連番、それ以外は中黒。
        // <ol type="a"> などの英字・ローマ数字は未対応で、いずれも十進で振る。
        char marker[24] = "\xe2\x80\xa2";  // •
        ListContext* listCtx = (self->listDepth > 0 && self->listDepth <= MAX_LIST_NESTING)
                                   ? &self->listStack[self->listDepth - 1]
                                   : nullptr;
        if (listCtx != nullptr && listCtx->ordered) {
          // <li value="N"> は以降の連番の起点も動かす（HTML と同じ）。
          if (atts != nullptr) {
            for (int i = 0; atts[i]; i += 2) {
              if (strcmp(atts[i], "value") == 0) {
                const long v = strtol(atts[i + 1], nullptr, 10);
                if (v > 0 && v <= 9999) listCtx->counter = static_cast<uint16_t>(v);
              }
            }
          }
          snprintf(marker, sizeof(marker), "%u.", static_cast<unsigned>(listCtx->counter));
          if (listCtx->counter < 9999) listCtx->counter++;
        }

        // マーカーと本文の間の空きは、レイアウトが入れる語間ではなくマーカー語そのものに持たせる。
        // 語間は中身次第（CJK どうしなら 0、欧文なら空白 1 つ）で変わるので、それを当てにすると
        // ぶら下げ幅と本文の開始位置がずれて、折り返した行だけ余分に下がってしまう。
        const size_t markerTextLen = strlen(marker);

        // 幅を測る前に、マーカーに使う字の advance を用意しておく。SD カードフォントの
        // advance テーブルはブロック単位で用意される（ParsedText の ensureSdCardFontReady）が、
        // ここはまだその手前。用意しないと lookupAdvance が NotCached を返して幅 0 になり、
        // ぶら下げ幅が狂ううえ、下のループが上限まで空白を足してしまう。
        if (self->renderer.isSdCardFont(self->fontId)) {
          self->renderer.ensureSdCardFontReady(self->fontId, "0123456789. \xe2\x80\xa2", 1u << EpdFontFamily::REGULAR);
        }

        // 空きを 1 つ入れたうえで、行として意味のある字下げ（行高の半分）に届くまで足す。
        // 縦書きでは事情が違う: ぶら下げインデント（paddingLeft / textIndent）は
        // layoutVerticalColumns が使わないので詰め物は効かず、列方向の空きになるだけ。
        // よって縦書きでは足さない。
        // 幅が 0 に見えるフォントでも暴走しないよう、足す数には上限を置く。
        static constexpr size_t MAX_MARKER_PADDING = 4;
        const int minIndent = self->renderer.getLineHeight(self->fontId) / 2;
        size_t markerLen = markerTextLen;
        int markerW = self->renderer.getTextAdvanceX(self->fontId, marker, EpdFontFamily::REGULAR);
        if (!self->verticalMode) {
          do {
            marker[markerLen++] = ' ';
            marker[markerLen] = '\0';
            markerW = self->renderer.getTextAdvanceX(self->fontId, marker, EpdFontFamily::REGULAR);
          } while (markerW < minIndent && markerLen - markerTextLen < MAX_MARKER_PADDING);
        }
        // ぶら下げ幅はマーカー語の実幅そのもの。次の語は語間 0 でくっつくので、
        // 折り返した行の頭が本文 1 文字目にちょうど揃う。
        // 万一 0 になっても字下げが消えないよう最低値を入れる。
        const auto hangIndent = static_cast<int16_t>(std::max(markerW, minIndent));
        // 入れ子の分の字下げ。<ol> / <ul> 自体はブロックとして扱っていないので CSS の
        // margin-left が効かず、これを入れないと内側のリストが外側と同じ位置に並ぶ。
        const int nestIndent = (self->listDepth > 1 ? self->listDepth - 1 : 0) * hangIndent;
        liBlockStyle.paddingLeft = static_cast<int16_t>(liBlockStyle.paddingLeft + hangIndent + nestIndent);
        liBlockStyle.textIndent = static_cast<int16_t>(-hangIndent);
        liBlockStyle.textIndentDefined = true;
        self->startNewTextBlock(liBlockStyle);
        self->updateEffectiveInlineStyle();
        if (self->verticalMode) {
          // 縦書きでは wordVerticalBehaviors が words と並列なので、ここで積まないと
          // 以降の語の縦書き挙動が 1 つずつずれる。1〜2 桁の番号は縦中横にする。
          // 詰め物の空白を含まない番号部分の長さで判定する（"1." = 2、"10." = 3）
          const auto vb = (listCtx != nullptr && listCtx->ordered && markerTextLen <= 3)
                              ? VerticalTextUtils::VerticalBehavior::TateChuYoko
                              : VerticalTextUtils::VerticalBehavior::Upright;
          self->currentTextBlock->addWord(marker, EpdFontFamily::REGULAR, vb);
        } else {
          self->currentTextBlock->addWord(marker, EpdFontFamily::REGULAR);
        }
        self->listMarkerPending = true;
        self->pendingSpace = false;
      } else {
        self->startNewTextBlock(userAlignmentBlockStyle);
        self->updateEffectiveInlineStyle();
      }
    }
  } else if (matches(name, UNDERLINE_TAGS, NUM_UNDERLINE_TAGS)) {
    // Flush buffer before style change so preceding text gets current style
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      self->nextWordContinues = true;
    }
    self->underlineUntilDepth = std::min(self->underlineUntilDepth, self->depth);
    // Push inline style entry for underline tag
    StyleStackEntry entry;
    entry.depth = self->depth;  // Track depth for matching pop
    entry.hasUnderline = true;
    entry.underline = true;
    if (cssStyle.hasFontWeight()) {
      entry.hasBold = true;
      entry.bold = cssStyle.fontWeight == CssFontWeight::Bold;
    }
    if (cssStyle.hasFontStyle()) {
      entry.hasItalic = true;
      entry.italic = cssStyle.fontStyle == CssFontStyle::Italic;
    }
    self->inlineStyleStack.push_back(entry);
    self->updateEffectiveInlineStyle();
  } else if (strcmp(name, "sup") == 0 || strcmp(name, "sub") == 0) {
    // 上付き・下付き。小さいフォントに切り替えるだけなので inlineStyleStack は使わず、
    // <b> / <i> と同じ深さ追跡で扱う。
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      self->nextWordContinues = true;
    }
    if (strcmp(name, "sup") == 0) {
      self->superscriptUntilDepth = std::min(self->superscriptUntilDepth, self->depth);
    } else {
      self->subscriptUntilDepth = std::min(self->subscriptUntilDepth, self->depth);
    }
  } else if (matches(name, BOLD_TAGS, NUM_BOLD_TAGS)) {
    // Flush buffer before style change so preceding text gets current style
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      self->nextWordContinues = true;
    }
    self->boldUntilDepth = std::min(self->boldUntilDepth, self->depth);
    // Push inline style entry for bold tag
    StyleStackEntry entry;
    entry.depth = self->depth;  // Track depth for matching pop
    entry.hasBold = true;
    entry.bold = true;
    if (cssStyle.hasFontStyle()) {
      entry.hasItalic = true;
      entry.italic = cssStyle.fontStyle == CssFontStyle::Italic;
    }
    if (cssStyle.hasTextDecoration()) {
      entry.hasUnderline = true;
      entry.underline = cssStyle.textDecoration == CssTextDecoration::Underline;
    }
    self->inlineStyleStack.push_back(entry);
    self->updateEffectiveInlineStyle();
  } else if (matches(name, ITALIC_TAGS, NUM_ITALIC_TAGS)) {
    // Flush buffer before style change so preceding text gets current style
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      self->nextWordContinues = true;
    }
    self->italicUntilDepth = std::min(self->italicUntilDepth, self->depth);
    // Push inline style entry for italic tag
    StyleStackEntry entry;
    entry.depth = self->depth;  // Track depth for matching pop
    entry.hasItalic = true;
    entry.italic = true;
    if (cssStyle.hasFontWeight()) {
      entry.hasBold = true;
      entry.bold = cssStyle.fontWeight == CssFontWeight::Bold;
    }
    if (cssStyle.hasTextDecoration()) {
      entry.hasUnderline = true;
      entry.underline = cssStyle.textDecoration == CssTextDecoration::Underline;
    }
    self->inlineStyleStack.push_back(entry);
    self->updateEffectiveInlineStyle();
  } else if (strcmp(name, "span") == 0 || !isHeaderOrBlock(name)) {
    // Handle span and other inline elements for CSS styling
    if (cssStyle.hasFontWeight() || cssStyle.hasFontStyle() || cssStyle.hasTextDecoration()) {
      // Flush buffer before style change so preceding text gets current style
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
        self->nextWordContinues = true;
      }
      StyleStackEntry entry;
      entry.depth = self->depth;  // Track depth for matching pop
      if (cssStyle.hasFontWeight()) {
        entry.hasBold = true;
        entry.bold = cssStyle.fontWeight == CssFontWeight::Bold;
      }
      if (cssStyle.hasFontStyle()) {
        entry.hasItalic = true;
        entry.italic = cssStyle.fontStyle == CssFontStyle::Italic;
      }
      if (cssStyle.hasTextDecoration()) {
        entry.hasUnderline = true;
        entry.underline = cssStyle.textDecoration == CssTextDecoration::Underline;
      }
      self->inlineStyleStack.push_back(entry);
      self->updateEffectiveInlineStyle();
    }
  }

  // Unprocessed tag, just increasing depth and continue forward
  self->depth += 1;
}

void XMLCALL ChapterHtmlSlimParser::characterData(void* userData, const XML_Char* s, const int len) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);

  // Skip content of nested table
  if (self->tableDepth > 1) {
    return;
  }

  // Buffer text for table cell (top-level table)
  if (self->tableDepth == 1) {
    for (int i = 0; i < len; i++) {
      char c = s[i];
      if (c == '\n' || c == '\r' || c == '\t') c = ' ';
      if (c == ' ' && (self->tableCellTextBuffer.empty() || self->tableCellTextBuffer.back() == ' ')) {
        continue;
      }
      self->tableCellTextBuffer += c;
    }
    return;
  }

  // Middle of skip
  if (self->skipUntilDepth < self->depth) {
    return;
  }

  // Collect ruby text instead of normal word processing
  if (self->collectingRubyText) {
    self->rubyTextBuffer.append(s, len);
    return;
  }

  // Collect footnote link display text (for the number label)
  // Skip whitespace and brackets to normalize noterefs like "[1]" → "1"
  if (self->insideFootnoteLink) {
    for (int i = 0; i < len; i++) {
      unsigned char c = static_cast<unsigned char>(s[i]);
      if (isWhitespace(c) || c == '[' || c == ']') continue;
      if (self->currentFootnoteLinkTextLen < static_cast<int>(sizeof(self->currentFootnoteLinkText)) - 1) {
        self->currentFootnoteLinkText[self->currentFootnoteLinkTextLen++] = c;
        self->currentFootnoteLinkText[self->currentFootnoteLinkTextLen] = '\0';
      }
    }
  }

  int i = 0;
  while (i < len) {
    // <pre> の中では空白と改行を原文のまま残す。
    if (self->preUntilDepth < self->depth && isWhitespace(s[i])) {
      if (s[i] == '\r') {
        i++;
        continue;
      }
      if (s[i] == '\n') {
        if (self->partWordBufferIndex > 0) {
          self->flushPartWordBuffer();
        }
        // <pre> の直後の改行 1 つは表示しない（HTML の規定）
        if (self->preSkipLeadingNewline && self->currentTextBlock && self->currentTextBlock->isEmpty() &&
            self->prePendingNewlines == 0) {
          self->preSkipLeadingNewline = false;
          i++;
          continue;
        }
        self->preSkipLeadingNewline = false;
        // ここでは行を切らず数えるだけ。次の中身が来たときに確定させる。
        self->prePendingNewlines++;
        i++;
        continue;
      }
      // 空白・タブの連なりを 1 つの語にして、直前の語にくっつける。
      // くっつけるのは、その手前で折り返すと行頭に空白が出てしまうため。
      // 折り返しは次の語の前で起きる（下で nextWordContinues を戻している）。
      self->preSkipLeadingNewline = false;
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
      }
      self->preFlushPendingNewlines();
      int spaces = 0;
      while (i < len && (s[i] == ' ' || s[i] == '\t')) {
        spaces += (s[i] == '\t') ? PRE_TAB_WIDTH : 1;
        i++;
      }
      if (spaces > MAX_WORD_SIZE) spaces = MAX_WORD_SIZE;
      const std::string spaceWord(static_cast<size_t>(spaces), ' ');
      if (self->verticalMode) {
        self->currentTextBlock->addWord(spaceWord, EpdFontFamily::REGULAR, VerticalTextUtils::VerticalBehavior::Upright,
                                        false, true, false);
      } else {
        self->currentTextBlock->addWord(spaceWord, EpdFontFamily::REGULAR, false, true, false);
      }
      self->nextWordContinues = false;  // 次の語の前では折り返してよい
      continue;
    }

    // Check for whitespace (ASCII only)
    if (isWhitespace(s[i])) {
      // Flush any buffered content as a word
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
      }
      // Whitespace is a real word boundary -- reset continuation state
      self->nextWordContinues = false;

      // 空白の連なりをまとめて読む。改行を含むかどうかで扱いが変わるため。
      bool sawSegmentBreak = false;
      while (i < len && isWhitespace(s[i])) {
        if (s[i] == '\n' || s[i] == '\r') sawSegmentBreak = true;
        i++;
      }

      // 改行を含む空白は「行の折り返し」であって空白ではない（CSS Text のセグメント改行）。
      // 和文どうしの間の改行は取り除かれるのが正しく、空白にしてはいけない。整形のために
      // 折り返してある XHTML は日本語の EPUB では珍しくないので、ここを間違えると
      // 原文の折り返し位置すべてに半角空白が入ってしまう。
      // 改行の前後にある空白も同じ扱いになる（CSS は改行に隣接する空白を先に落とす）ので、
      // 連なり全体を見て決める。
      //
      // 実際の空白（' ' / '\t'）だけなら語間として残す。ブロック先頭の空白は字下げではなく
      // HTML の整形由来なので無視する。リストマーカーの直後も同じで、<li> の後ろの空白を
      // 語間にしてしまうとぶら下げ幅とずれる。
      if (!sawSegmentBreak && self->currentTextBlock && !self->currentTextBlock->isEmpty() &&
          !self->listMarkerPending) {
        self->pendingSpace = true;
      }
      continue;
    }

    // Detect U+00A0 (non-breaking space, UTF-8: 0xC2 0xA0) or
    //        U+202F (narrow no-break space, UTF-8: 0xE2 0x80 0xAF).
    //
    // Both are rendered as a visible space but must never allow a line break around them.
    // We split the no-break space into its own word token and link the surrounding words
    // with continuation flags so the layout engine treats them as an indivisible group.
    if (static_cast<uint8_t>(s[i]) == 0xC2 && i + 1 < len && static_cast<uint8_t>(s[i + 1]) == 0xA0) {
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
      }

      self->partWordBuffer[0] = ' ';
      self->partWordBuffer[1] = '\0';
      self->partWordBufferIndex = 1;
      self->nextWordContinues = true;  // Attach space to previous word (no break).
      self->flushPartWordBuffer();

      self->nextWordContinues = true;  // Next real word attaches to this space (no break).

      i++;  // Skip the second byte (0xA0)
      continue;
    }

    // U+202F (narrow no-break space) -- identical logic to U+00A0 above.
    if (static_cast<uint8_t>(s[i]) == 0xE2 && i + 2 < len && static_cast<uint8_t>(s[i + 1]) == 0x80 &&
        static_cast<uint8_t>(s[i + 2]) == 0xAF) {
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
      }

      self->partWordBuffer[0] = ' ';
      self->partWordBuffer[1] = '\0';
      self->partWordBufferIndex = 1;
      self->nextWordContinues = true;
      self->flushPartWordBuffer();

      self->nextWordContinues = true;

      i += 2;  // Skip the remaining two bytes (0x80 0xAF)
      continue;
    }

    if (self->prePendingNewlines > 0) {
      self->preFlushPendingNewlines();
    }

    // Determine UTF-8 character length
    const unsigned char b0 = static_cast<unsigned char>(s[i]);
    const int charLen = getUtf8ByteLength(b0);
    if (i + charLen > len) {
      // Incomplete UTF-8 sequence at end, just add the byte
      if (self->partWordBufferIndex < MAX_WORD_SIZE) {
        self->partWordBuffer[self->partWordBufferIndex++] = s[i];
      }
      i++;
      continue;
    }

    // Decode the codepoint to check if it's CJK or invisible
    const uint32_t cp = decodeUtf8Codepoint(&s[i], charLen);

    // Skip invisible/zero-width Unicode characters that fonts can't render
    if (isInvisibleCodepoint(cp)) {
      i += charLen;
      continue;
    }

    // 全角スペース（U+3000）は捨てずに CJK 1 文字の「語」として流す（下の分岐で処理）。
    // 日本語組版では字下げ（行頭の全角スペース）や「第一章　題名」の区切りとして
    // 意味を持つ空白であり、ASCII 空白のように折りたたむと消えてしまう。
    // フォント側に U+3000 のグリフ（空白・全角送り）があることが前提で、
    // SD カードフォント（fontconvert_sdcard.py の cjk 範囲）と内蔵 CJK UI フォントは持っている。
    if (isCjkCodepointForSplit(cp)) {
      // CJK character: flush any buffered content first
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
      }

      // Add this CJK character as its own "word"
      char cjkWord[5] = {0};  // Max 4 bytes for UTF-8 + null terminator
      for (int j = 0; j < charLen && j < 4; j++) {
        cjkWord[j] = s[i + j];
      }
      if (self->verticalMode) {
        self->currentTextBlock->addWord(cjkWord, EpdFontFamily::REGULAR, VerticalTextUtils::VerticalBehavior::Upright,
                                        false, self->listMarkerPending, self->pendingSpace);
      } else {
        self->currentTextBlock->addWord(cjkWord, EpdFontFamily::REGULAR, false, self->listMarkerPending,
                                        self->pendingSpace);
      }
      self->pendingSpace = false;
      self->listMarkerPending = false;
      i += charLen;
      // 中間flush: 1 回の characterData 呼び出しで CJK 単語が大量追加される場合、
      // characterData 末尾の flush 判定では間に合わず words vector の realloc で abort する。
      // 青空文庫のような <p> なし超長段落 EPUB で顕在化する（Issue #72）。
      if (self->currentTextBlock->size() >= MID_BLOCK_FLUSH_WORDS) {
        self->flushCurrentBlockMidPage();
      }
      continue;
    }

    // Non-CJK character: buffer it
    // If we're about to run out of space, flush the buffer first
    if (self->partWordBufferIndex + charLen >= MAX_WORD_SIZE) {
      self->flushPartWordBuffer();
    }

    // Add all bytes of this character to the buffer
    for (int j = 0; j < charLen; j++) {
      self->partWordBuffer[self->partWordBufferIndex++] = s[i + j];
    }
    i += charLen;
  }

  // 末尾判定: ヒープ低下時に早期flush。
  // 通常flush（MID_BLOCK_FLUSH_WORDS 到達）はループ内で判定済みなのでここでは不要。
  const size_t wordCount = self->currentTextBlock->size();
  const bool earlyFlush = wordCount > 100 && ESP.getFreeHeap() < MIN_FREE_HEAP_FOR_PARSING * 2;
  if (earlyFlush) {
    LOG_DBG("EHP", "Text block too long, splitting into multiple pages");
    self->flushCurrentBlockMidPage();
  }
}

// Mid-block flush: 現在の text block を layout に流し込み、消費済み単語を erase する。
// characterData のループ内でも呼ばれるため、境界での partial column を残す (includeLast=false)。
void ChapterHtmlSlimParser::flushCurrentBlockMidPage() {
  if (!currentTextBlock || currentTextBlock->isEmpty()) return;

  if (verticalMode) {
    currentTextBlock->layoutVerticalColumns(
        renderer, fontId, viewportHeight,
        [this](const std::shared_ptr<TextBlock>& textBlock) { addLineToPage(textBlock); }, false);
  } else {
    const int horizontalInset = currentTextBlock->getBlockStyle().totalHorizontalInset();
    const uint16_t effectiveWidth =
        (horizontalInset < viewportWidth) ? static_cast<uint16_t>(viewportWidth - horizontalInset) : viewportWidth;
    currentTextBlock->layoutAndExtractLines(
        renderer, fontId, effectiveWidth,
        [this](const std::shared_ptr<TextBlock>& textBlock) { addLineToPage(textBlock); }, false);
  }
}

void XMLCALL ChapterHtmlSlimParser::defaultHandlerExpand(void* userData, const XML_Char* s, const int len) {
  // Check if this looks like an entity reference (&...;)
  if (len >= 3 && s[0] == '&' && s[len - 1] == ';') {
    const char* utf8Value = lookupHtmlEntity(s, static_cast<size_t>(len));
    if (utf8Value != nullptr) {
      // Known entity: expand to its UTF-8 value
      characterData(userData, utf8Value, strlen(utf8Value));
      return;
    }
    // Unknown entity: preserve original &...; sequence
    characterData(userData, s, len);
    return;
  }
  // Not an entity we recognize - skip it
}

void XMLCALL ChapterHtmlSlimParser::endElement(void* userData, const XML_Char* name) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);

  // Check if any style state will change after we decrement depth
  // If so, we MUST flush the partWordBuffer with the CURRENT style first
  // Note: depth hasn't been decremented yet, so we check against (depth - 1)
  const bool willPopStyleStack =
      !self->inlineStyleStack.empty() && self->inlineStyleStack.back().depth == self->depth - 1;
  const bool willClearBold = self->boldUntilDepth == self->depth - 1;
  const bool willClearScript =
      self->superscriptUntilDepth == self->depth - 1 || self->subscriptUntilDepth == self->depth - 1;
  const bool willClearItalic = self->italicUntilDepth == self->depth - 1;
  const bool willClearUnderline = self->underlineUntilDepth == self->depth - 1;

  const bool styleWillChange =
      willPopStyleStack || willClearBold || willClearItalic || willClearUnderline || willClearScript;
  const bool headerOrBlockTag = isHeaderOrBlock(name);
  const bool tableStructuralTag = isTableStructuralTag(name);

  // <ol> / <ul> のネストを戻す。depth はまだ減っていないので、この要素自身の深さは
  // depth - 1。startElement が push を飛ばす条件（skip の内側）と対称にする。
  if ((strcmp(name, "ol") == 0 || strcmp(name, "ul") == 0) && self->skipUntilDepth >= self->depth - 1) {
    if (self->listDepth > 0) self->listDepth--;
  }

  if (self->tableDepth > 1 && strcmp(name, "table") == 0) {
    self->tableDepth -= 1;
    LOG_DBG("EHP", "nested table detected, skipped");
    return;
  }

  if (self->tableDepth == 1 && (strcmp(name, "td") == 0 || strcmp(name, "th") == 0)) {
    if (!self->tableBuffer.empty()) {
      ChapterHtmlSlimParser::TableCellData cell;
      if (!self->tableCellTextBuffer.empty() && self->tableCellTextBuffer.back() == ' ') {
        self->tableCellTextBuffer.pop_back();
      }
      cell.text = std::move(self->tableCellTextBuffer);
      cell.isHeader = self->tableCellIsHeader;
      self->tableBuffer.back().cells.push_back(std::move(cell));
    }
    self->tableCellTextBuffer.clear();
  }

  if (self->tableDepth == 1 && strcmp(name, "tr") == 0) {
    // Row complete
  }

  if (self->tableDepth == 1 && strcmp(name, "table") == 0) {
    self->tableDepth -= 1;
    self->flushTableAsGrid();
    self->tableBuffer.clear();
    self->tableRowIndex = 0;
    self->tableColIndex = 0;

    // tableの後で改ページ — テーブルと後続テキストの重なりを防ぐ
    if (self->currentPage) {
      self->completePageFn(std::move(self->currentPage));
      self->completedPageCount++;
      self->currentPage.reset();
      self->currentPageNextY = 0;
      if (self->verticalMode) {
        const int lineHeight = self->renderer.getLineHeight(self->fontId) * self->lineCompression;
        self->currentPageNextX = self->viewportWidth - lineHeight;
      }
    }
  }

  // Flush buffer with current style BEFORE any style changes
  if (self->partWordBufferIndex > 0) {
    // Flush if style will change OR if we're closing a block/structural element
    const bool isInlineTag =
        !headerOrBlockTag && !tableStructuralTag && !matches(name, IMAGE_TAGS, NUM_IMAGE_TAGS) && self->depth != 1;
    const bool shouldFlush = styleWillChange || headerOrBlockTag || matches(name, BOLD_TAGS, NUM_BOLD_TAGS) ||
                             matches(name, ITALIC_TAGS, NUM_ITALIC_TAGS) ||
                             matches(name, UNDERLINE_TAGS, NUM_UNDERLINE_TAGS) || tableStructuralTag ||
                             matches(name, IMAGE_TAGS, NUM_IMAGE_TAGS) || self->depth == 1;

    if (shouldFlush) {
      self->flushPartWordBuffer();
      // If closing an inline element, the next word fragment continues the same visual word
      if (isInlineTag) {
        self->nextWordContinues = true;
      }
    }
  }

  self->depth -= 1;

  // Ruby closing tags
  if (strcmp(name, "rt") == 0) {
    self->collectingRubyText = false;
  }
  if (strcmp(name, "ruby") == 0 && self->inRuby && self->currentTextBlock) {
    const int currentWordCount = static_cast<int>(self->currentTextBlock->size());
    const int baseWordCount = currentWordCount - self->rubyStartWordIndex;
    // 括弧を除いた文字数で親文字に配分する（配分後に消すと、括弧の分だけ配分が偏る）
    stripAozoraRubyBrackets(self->rubyTextBuffer);
    if (baseWordCount > 0 && !self->rubyTextBuffer.empty()) {
      // Count UTF-8 characters in ruby text
      std::vector<size_t> charOffsets;
      const char* p = self->rubyTextBuffer.c_str();
      while (*p) {
        charOffsets.push_back(p - self->rubyTextBuffer.c_str());
        if ((*p & 0x80) == 0)
          p += 1;
        else if ((*p & 0xE0) == 0xC0)
          p += 2;
        else if ((*p & 0xF0) == 0xE0)
          p += 3;
        else
          p += 4;
      }
      charOffsets.push_back(self->rubyTextBuffer.size());
      const int rubyCharCount = static_cast<int>(charOffsets.size() - 1);

      for (int i = 0; i < baseWordCount; i++) {
        const int start = i * rubyCharCount / baseWordCount;
        const int end = (i + 1) * rubyCharCount / baseWordCount;
        if (start < end) {
          std::string portion = self->rubyTextBuffer.substr(charOffsets[start], charOffsets[end] - charOffsets[start]);
          self->currentTextBlock->setRubyForWordAt(self->rubyStartWordIndex + i, portion);
        }
      }
    }
    self->inRuby = false;
    self->rubyStartWordIndex = -1;
    self->rubyTextBuffer.clear();
  }

  // Closing a footnote link — create entry from collected text and href
  if (self->insideFootnoteLink && self->depth == self->footnoteLinkDepth) {
    if (self->currentFootnoteLinkText[0] != '\0' && self->currentFootnoteLinkHref[0] != '\0') {
      FootnoteEntry entry;
      strncpy(entry.number, self->currentFootnoteLinkText, sizeof(entry.number) - 1);
      entry.number[sizeof(entry.number) - 1] = '\0';
      strncpy(entry.href, self->currentFootnoteLinkHref, sizeof(entry.href) - 1);
      entry.href[sizeof(entry.href) - 1] = '\0';
      int wordIndex =
          self->wordsExtractedInBlock + (self->currentTextBlock ? static_cast<int>(self->currentTextBlock->size()) : 0);
      self->pendingFootnotes.push_back({wordIndex, entry});
    }
    self->insideFootnoteLink = false;
  }

  // Leaving skip
  if (self->skipUntilDepth == self->depth) {
    self->skipUntilDepth = INT_MAX;
  }

  // Leaving bold tag
  if (self->boldUntilDepth == self->depth) {
    self->boldUntilDepth = INT_MAX;
  }

  // Leaving pre
  if (self->preUntilDepth == self->depth) {
    self->preUntilDepth = INT_MAX;
    self->preSkipLeadingNewline = false;
    self->prePendingNewlines = 0;  // 末尾の改行は行にしない
    self->hyphenationEnabled = self->preSavedHyphenation;
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
    }
    // 手元に残っている行が <pre> の最終行。ここで枠の下辺を付けてから流す。
    // 中身が空の <pre> でも、開始時に外した下のアキは戻す（戻さないと次の段落との
    // 間隔が CSS の指定より詰まる）。枠の下辺は行が無ければ引かない。
    if (self->currentTextBlock) {
      auto lastStyle = self->currentTextBlock->getBlockStyle();
      if (!self->currentTextBlock->isEmpty()) {
        lastStyle.frameEdges = static_cast<uint8_t>(lastStyle.frameEdges | BlockStyle::FRAME_BOTTOM);
      }
      // <pre> の開始時に外しておいた下のアキをここで戻す
      lastStyle.paddingBottom = static_cast<int16_t>(lastStyle.paddingBottom + self->preSavedPaddingBottom);
      lastStyle.marginBottom = static_cast<int16_t>(lastStyle.marginBottom + self->preSavedMarginBottom +
                                                    self->renderer.getLineHeight(self->fontId) / 3);
      self->currentTextBlock->setBlockStyle(lastStyle);
    }
    // 直後のテキストが <pre> のスタイル（左揃え・字下げなし）を引きずらないよう
    // ブロックを切り替える。currentTextBlock は常に非 null に保つ必要がある。
    self->startNewTextBlock(BlockStyle());
  }

  // Leaving sup / sub
  if (self->superscriptUntilDepth == self->depth) {
    self->superscriptUntilDepth = INT_MAX;
  }
  if (self->subscriptUntilDepth == self->depth) {
    self->subscriptUntilDepth = INT_MAX;
  }

  // Leaving italic tag
  if (self->italicUntilDepth == self->depth) {
    self->italicUntilDepth = INT_MAX;
  }

  // Leaving underline tag
  if (self->underlineUntilDepth == self->depth) {
    self->underlineUntilDepth = INT_MAX;
  }

  // Pop from inline style stack if we pushed an entry at this depth
  // This handles all inline elements: b, i, u, span, etc.
  if (!self->inlineStyleStack.empty() && self->inlineStyleStack.back().depth == self->depth) {
    self->inlineStyleStack.pop_back();
    self->updateEffectiveInlineStyle();
  }

  // Clear block style when leaving header or block elements
  if (headerOrBlockTag) {
    self->currentCssStyle.reset();
    self->updateEffectiveInlineStyle();

    // Reset alignment on empty text blocks to prevent stale alignment from bleeding
    // into the next sibling element. This fixes issue #1026 where an empty <h1> (default
    // Center) followed by an image-only <p> causes Center to persist through the chain
    // of empty block reuse into subsequent text paragraphs.
    // Margins/padding are preserved so parent element spacing still accumulates correctly.
    if (self->currentTextBlock && self->currentTextBlock->isEmpty()) {
      auto style = self->currentTextBlock->getBlockStyle();
      style.textAlignDefined = false;
      style.alignment = (self->paragraphAlignment == static_cast<uint8_t>(CssTextAlign::None))
                            ? CssTextAlign::Justify
                            : static_cast<CssTextAlign>(self->paragraphAlignment);
      self->currentTextBlock->setBlockStyle(style);
    }
  }
}

bool ChapterHtmlSlimParser::parseAndBuildPages() {
  auto paragraphAlignmentBlockStyle = BlockStyle();
  paragraphAlignmentBlockStyle.textAlignDefined = true;
  // Resolve None sentinel to Justify for initial block (no CSS context yet)
  const auto align = (this->paragraphAlignment == static_cast<uint8_t>(CssTextAlign::None))
                         ? CssTextAlign::Justify
                         : static_cast<CssTextAlign>(this->paragraphAlignment);
  paragraphAlignmentBlockStyle.alignment = align;
  startNewTextBlock(paragraphAlignmentBlockStyle);

  const XML_Parser parser = XML_ParserCreate(nullptr);
  int done;

  if (!parser) {
    LOG_ERR("EHP", "Couldn't allocate memory for parser");
    return false;
  }

  // Handle HTML entities (like &nbsp;) that aren't in XML spec or DTD
  // Using DefaultHandlerExpand preserves normal entity expansion from DOCTYPE
  XML_SetDefaultHandlerExpand(parser, defaultHandlerExpand);

  FsFile file;
  if (!Storage.openFileForRead("EHP", filepath, file)) {
    XML_ParserFree(parser);
    return false;
  }

  // Get file size to decide whether to show indexing popup.
  if (popupFn && file.size() >= MIN_SIZE_FOR_POPUP) {
    popupFn();
  }

  XML_SetUserData(parser, this);
  XML_SetElementHandler(parser, startElement, endElement);
  XML_SetCharacterDataHandler(parser, characterData);

  // Compute the time taken to parse and build pages
  const uint32_t chapterStartTime = millis();
  do {
    void* const buf = XML_GetBuffer(parser, PARSE_BUFFER_SIZE);
    if (!buf) {
      LOG_ERR("EHP", "Couldn't allocate memory for buffer");
      XML_StopParser(parser, XML_FALSE);                // Stop any pending processing
      XML_SetElementHandler(parser, nullptr, nullptr);  // Clear callbacks
      XML_SetCharacterDataHandler(parser, nullptr);
      XML_ParserFree(parser);
      file.close();
      return false;
    }

    SECTION_PERF_BEGIN(readStartUs);
    const size_t len = file.read(buf, PARSE_BUFFER_SIZE);
    SECTION_PERF_END(readStartUs, readUs);

    if (len == 0 && file.available() > 0) {
      LOG_ERR("EHP", "File read error");
      XML_StopParser(parser, XML_FALSE);                // Stop any pending processing
      XML_SetElementHandler(parser, nullptr, nullptr);  // Clear callbacks
      XML_SetCharacterDataHandler(parser, nullptr);
      XML_ParserFree(parser);
      file.close();
      return false;
    }

    done = file.available() == 0;

    SECTION_PERF_BEGIN(parseStartUs);
    const XML_Status parseStatus = XML_ParseBuffer(parser, static_cast<int>(len), done);
    SECTION_PERF_END(parseStartUs, parseUs);

    if (parseStatus == XML_STATUS_ERROR) {
      LOG_ERR("EHP", "Parse error at line %lu:\n%s", XML_GetCurrentLineNumber(parser),
              XML_ErrorString(XML_GetErrorCode(parser)));
      XML_StopParser(parser, XML_FALSE);                // Stop any pending processing
      XML_SetElementHandler(parser, nullptr, nullptr);  // Clear callbacks
      XML_SetCharacterDataHandler(parser, nullptr);
      XML_ParserFree(parser);
      file.close();
      return false;
    }

    // Periodic heap check during parsing to prevent abort() from failed allocations
    if (!done && ESP.getFreeHeap() < MIN_FREE_HEAP_FOR_PARSING) {
      LOG_ERR("EHP", "Low heap during parsing (%u bytes), stopping gracefully", ESP.getFreeHeap());
      XML_StopParser(parser, XML_FALSE);
      XML_SetElementHandler(parser, nullptr, nullptr);
      XML_SetCharacterDataHandler(parser, nullptr);
      XML_ParserFree(parser);
      file.close();
      return false;
    }
  } while (!done);
  LOG_DBG("EHP", "Time to parse and build pages: %lu ms", millis() - chapterStartTime);

  XML_StopParser(parser, XML_FALSE);                // Stop any pending processing
  XML_SetElementHandler(parser, nullptr, nullptr);  // Clear callbacks
  XML_SetCharacterDataHandler(parser, nullptr);
  XML_ParserFree(parser);
  file.close();

  // Process last page if there is still text
  if (currentTextBlock) {
    makePages();
    if (!pendingAnchorId.empty()) {
      anchorData.push_back({std::move(pendingAnchorId), static_cast<uint16_t>(completedPageCount)});
      pendingAnchorId.clear();
    }
    completePageFn(std::move(currentPage));
    completedPageCount++;
    currentPage.reset();
    currentTextBlock.reset();
  }

  return true;
}

void ChapterHtmlSlimParser::addLineToPage(std::shared_ptr<TextBlock> line) {
  const int effectiveFontId = (line->getBlockStyle().fontId != 0) ? line->getBlockStyle().fontId : fontId;
  const int lineHeight = renderer.getLineHeight(effectiveFontId) * lineCompression;

  // 枠線の縦線は行の送りと同じ長さでないと隣の行との間で途切れる。
  // 行送りを知っているのはここだけなので、この時点でブロックに入れておく。
  if (line->getBlockStyle().frameEdges != 0) {
    auto framedStyle = line->getBlockStyle();
    framedStyle.frameHeight = static_cast<uint16_t>(lineHeight);
    line->setBlockStyle(framedStyle);
  }

  if (verticalMode) {
    // Vertical mode: columns placed right-to-left
    const int columnWidth = lineHeight;
    const int columnSpacing = columnWidth / 4;

    if (!currentPage) {
      currentPage.reset(new Page());
      currentPageNextX = viewportWidth - columnWidth;  // start from right edge
    }

    if (currentPageNextX < 0) {
      // Page full — emit and start new page
      completePageFn(std::move(currentPage));
      completedPageCount++;
      currentPage.reset(new Page());
      currentPageNextX = viewportWidth - columnWidth;
    }

    // Track cumulative words for footnote assignment
    wordsExtractedInBlock += line->wordCount();
    auto footnoteIt = pendingFootnotes.begin();
    while (footnoteIt != pendingFootnotes.end() && footnoteIt->first <= wordsExtractedInBlock) {
      currentPage->addFootnote(footnoteIt->second.number, footnoteIt->second.href);
      ++footnoteIt;
    }
    pendingFootnotes.erase(pendingFootnotes.begin(), footnoteIt);

    // Column x-position, y=0 (column starts at top)
    currentPage->elements.push_back(std::make_shared<PageLine>(line, static_cast<int16_t>(currentPageNextX), 0));
    currentPageNextX -= (columnWidth + columnSpacing);
  } else {
    // Horizontal mode: lines placed top-to-bottom (existing logic)
    if (!currentPage) {
      currentPage.reset(new Page());
      currentPageNextY = 0;
    }

    if (currentPageNextY + lineHeight > viewportHeight) {
      completePageFn(std::move(currentPage));
      completedPageCount++;
      currentPage.reset(new Page());
      currentPageNextY = 0;
    }

    // Track cumulative words for footnote assignment
    wordsExtractedInBlock += line->wordCount();
    auto footnoteIt = pendingFootnotes.begin();
    while (footnoteIt != pendingFootnotes.end() && footnoteIt->first <= wordsExtractedInBlock) {
      currentPage->addFootnote(footnoteIt->second.number, footnoteIt->second.href);
      ++footnoteIt;
    }
    pendingFootnotes.erase(pendingFootnotes.begin(), footnoteIt);

    const int16_t xOffset = line->getBlockStyle().leftInset();
    currentPage->elements.push_back(std::make_shared<PageLine>(line, xOffset, currentPageNextY));
    currentPageNextY += lineHeight;
  }
}

// Character-level text wrapping for table cells.
// Unlike wrappedText() which only breaks on spaces, this breaks at any character boundary,
// making it suitable for CJK text without spaces.
static std::vector<std::string> wrapCellText(GfxRenderer& renderer, const int fontId, const char* text,
                                             const int maxWidth, const int maxLines, const EpdFontFamily::Style style) {
  std::vector<std::string> lines;
  if (!text || !*text || maxWidth <= 0 || maxLines <= 0) return lines;

  const char* p = text;
  std::string current;

  while (*p) {
    // Determine UTF-8 character length
    int charLen = 1;
    const auto c = static_cast<unsigned char>(*p);
    if (c >= 0xF0)
      charLen = 4;
    else if (c >= 0xE0)
      charLen = 3;
    else if (c >= 0xC0)
      charLen = 2;

    std::string test = current + std::string(p, charLen);
    if (renderer.getTextWidth(fontId, test.c_str(), style) > maxWidth) {
      if (current.empty()) {
        // Single char wider than maxWidth — just push it
        lines.push_back(std::string(p, charLen));
        p += charLen;
      } else {
        lines.push_back(current);
        current.clear();
        // Don't advance p — re-process this character on the next line
      }
      if (static_cast<int>(lines.size()) >= maxLines) {
        // Append remaining text as truncated last line
        if (*p) {
          lines.back() = renderer.truncatedText(fontId, (lines.back() + std::string(p)).c_str(), maxWidth, style);
        }
        return lines;
      }
      continue;
    }
    current = test;
    p += charLen;
  }

  if (!current.empty() && static_cast<int>(lines.size()) < maxLines) {
    lines.push_back(current);
  }

  return lines;
}

void ChapterHtmlSlimParser::flushTableAsGrid() {
  if (tableBuffer.empty()) return;

  int maxCols = 0;
  for (const auto& row : tableBuffer) {
    maxCols = std::max(maxCols, static_cast<int>(row.cells.size()));
  }
  if (maxCols == 0) return;

  const int tblFontId = (tableFontId != 0) ? tableFontId : fontId;
  // Use compact line height for tables: ascender + 1px instead of full advanceY.
  const int ascender = renderer.getFontAscenderSize(tblFontId);
  const int fullLineH = renderer.getLineHeight(tblFontId);
  const int lineH = ascender + 1;
  // Descender overshoot: glyphs extend below lineH by this amount
  const int descenderExtra = std::max(0, fullLineH - lineH);
  static constexpr int16_t CELL_PADDING = 2;
  static constexpr int MAX_CELL_LINES = 8;

  // Limit columns to what can fit (min ~50px per column)
  static constexpr int MIN_COL_WIDTH = 50;
  const int maxFittableCols = std::max(1, static_cast<int>(viewportWidth) / MIN_COL_WIDTH);
  if (maxCols > maxFittableCols) maxCols = maxFittableCols;

  // Measure max content width per column across all rows
  std::vector<int> maxContentWidth(maxCols, 0);
  for (const auto& row : tableBuffer) {
    for (int col = 0; col < maxCols && col < static_cast<int>(row.cells.size()); col++) {
      if (!row.cells[col].text.empty()) {
        const auto style = row.cells[col].isHeader ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
        const int tw = renderer.getTextWidth(tblFontId, row.cells[col].text.c_str(), style);
        maxContentWidth[col] = std::max(maxContentWidth[col], tw);
      }
    }
  }

  // Column widths: content-based with minimum width, capped to viewport
  auto layout = std::make_shared<TableColumnLayout>();
  layout->colWidths.resize(maxCols);
  layout->fontId = tblFontId;
  layout->lineHeight = static_cast<int16_t>(lineH);
  layout->cellPadding = CELL_PADDING;

  static constexpr int MIN_COL_CONTENT_WIDTH = 30;
  int totalNeeded = 0;
  for (int col = 0; col < maxCols; col++) {
    int needed = std::max(MIN_COL_CONTENT_WIDTH, maxContentWidth[col]) + CELL_PADDING * 2;
    layout->colWidths[col] = static_cast<uint16_t>(needed);
    totalNeeded += needed;
  }

  if (totalNeeded > viewportWidth) {
    // Scale down proportionally to fit viewport
    for (int col = 0; col < maxCols; col++) {
      layout->colWidths[col] =
          static_cast<uint16_t>(static_cast<int>(layout->colWidths[col]) * viewportWidth / totalNeeded);
    }
  }

  // Start table on a new page if current page has content
  if (currentPage && currentPageNextY > 0) {
    completePageFn(std::move(currentPage));
    completedPageCount++;
    currentPage.reset(new Page());
    currentPageNextY = 0;
  }

  // Process each row: wrap text, compute row height, add to pages
  const int numRows = static_cast<int>(tableBuffer.size());
  for (int rowIdx = 0; rowIdx < numRows; rowIdx++) {
    const auto& row = tableBuffer[rowIdx];
    std::vector<std::vector<std::string>> cellLines(maxCols);
    std::vector<bool> headers(maxCols, false);
    int maxLinesInRow = 1;

    for (int col = 0; col < maxCols && col < static_cast<int>(row.cells.size()); col++) {
      headers[col] = row.cells[col].isHeader;
      const auto style = headers[col] ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
      const int maxTextW = layout->colWidths[col] - CELL_PADDING * 2;
      if (maxTextW > 0 && !row.cells[col].text.empty()) {
        cellLines[col] =
            wrapCellText(renderer, tblFontId, row.cells[col].text.c_str(), maxTextW, MAX_CELL_LINES, style);
      }
      maxLinesInRow = std::max(maxLinesInRow, static_cast<int>(cellLines[col].size()));
    }

    // Add descenderExtra so bottom padding visually matches top padding
    const int16_t rowHeight = static_cast<int16_t>(maxLinesInRow * lineH + descenderExtra + CELL_PADDING * 2);

    auto block = std::make_shared<TableRowBlock>(std::move(cellLines), std::move(headers), layout, rowHeight,
                                                 rowIdx == 0, rowIdx == numRows - 1);

    if (!currentPage) {
      currentPage.reset(new Page());
      currentPageNextY = 0;
    }

    if (currentPageNextY + rowHeight > viewportHeight) {
      completePageFn(std::move(currentPage));
      completedPageCount++;
      currentPage.reset(new Page());
      currentPageNextY = 0;
    }

    currentPage->elements.push_back(std::make_shared<PageTableRow>(block, 0, currentPageNextY));
    currentPageNextY += rowHeight;
  }
}

void ChapterHtmlSlimParser::makePages() {
  if (!currentTextBlock) {
    LOG_ERR("EHP", "!! No text block to make pages for !!");
    return;
  }

  if (!currentPage) {
    currentPage.reset(new Page());
    currentPageNextY = 0;
    if (verticalMode) {
      const int lineHeight = renderer.getLineHeight(fontId) * lineCompression;
      currentPageNextX = viewportWidth - lineHeight;
    }
  }

  const int lineHeight = renderer.getLineHeight(fontId) * lineCompression;

  // Apply top spacing before the paragraph (stored in pixels)
  const BlockStyle& blockStyle = currentTextBlock->getBlockStyle();
  if (blockStyle.marginTop > 0) {
    currentPageNextY += blockStyle.marginTop;
  }
  if (blockStyle.paddingTop > 0) {
    currentPageNextY += blockStyle.paddingTop;
  }

  // Calculate effective width accounting for horizontal margins/padding
  const int horizontalInset = blockStyle.totalHorizontalInset();
  const uint16_t effectiveWidth =
      (horizontalInset < viewportWidth) ? static_cast<uint16_t>(viewportWidth - horizontalInset) : viewportWidth;

  const int layoutFontId = (blockStyle.fontId != 0) ? blockStyle.fontId : fontId;
  if (verticalMode) {
    currentTextBlock->layoutVerticalColumns(
        renderer, layoutFontId, viewportHeight,
        [this](const std::shared_ptr<TextBlock>& textBlock) { addLineToPage(textBlock); });
  } else {
    currentTextBlock->layoutAndExtractLines(
        renderer, layoutFontId, effectiveWidth,
        [this](const std::shared_ptr<TextBlock>& textBlock) { addLineToPage(textBlock); });
  }

  // Fallback: transfer any remaining pending footnotes to current page.
  // Normally addLineToPage handles this via word-index tracking, but this catches
  // edge cases where a footnote's word index equals the exact block size.
  if (!pendingFootnotes.empty() && currentPage) {
    for (const auto& [idx, fn] : pendingFootnotes) {
      currentPage->addFootnote(fn.number, fn.href);
    }
    pendingFootnotes.clear();
  }

  // Apply bottom spacing after the paragraph (stored in pixels)
  if (blockStyle.marginBottom > 0) {
    currentPageNextY += blockStyle.marginBottom;
  }
  if (blockStyle.paddingBottom > 0) {
    currentPageNextY += blockStyle.paddingBottom;
  }

  // Extra paragraph spacing if enabled (default behavior)
  // List items get reduced spacing to avoid excessive gaps in TOC pages etc.
  // 段落の追加アキ。次の 2 つには入れない。
  //  - 見出し: 自前の marginBottom で本文と分けている。行間を広げた設定（行送り 1.6 など）
  //    だと追加アキだけで本文 0.8 行ぶんになり、見出しと本文が離れすぎる。
  //  - <pre>: 1 行が 1 ブロックなので、入れると行間が広がって枠線も途切れる。
  if (extraParagraphSpacing && blockStyle.frameEdges == 0 && !blockStyle.isHeading) {
    currentPageNextY += blockStyle.isListItem ? (lineHeight / 6) : (lineHeight / 2);
  }
}
