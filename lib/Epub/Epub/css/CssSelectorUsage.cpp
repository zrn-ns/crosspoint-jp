#include "CssSelectorUsage.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cctype>

namespace {

// Buffer size for streaming the HTML file (matches CssParser::loadFromStream)
constexpr size_t READ_BUFFER_SIZE = 512;

bool isNameChar(const char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == ':';
}

bool isHtmlWhitespace(const char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

}  // namespace

bool CssSelectorUsage::contains(const std::vector<std::string>& list, const std::string_view name) {
  for (const auto& entry : list) {
    if (entry == name) {
      return true;
    }
  }
  return false;
}

void CssSelectorUsage::addTag(const std::string_view name) {
  if (name.empty() || overflowed_ || contains(tags_, name)) {
    return;
  }
  if (tags_.size() >= MAX_TAGS) {
    overflowed_ = true;
    return;
  }
  tags_.emplace_back(name);
}

void CssSelectorUsage::addClass(const std::string_view name) {
  if (name.empty() || overflowed_ || contains(classes_, name)) {
    return;
  }
  if (classes_.size() >= MAX_CLASSES) {
    overflowed_ = true;
    return;
  }
  classes_.emplace_back(name);
}

bool CssSelectorUsage::matches(const std::string& selectorKey) const {
  if (overflowed_) {
    return true;
  }
  const std::string_view key(selectorKey);
  const size_t dot = key.find('.');
  if (dot == std::string_view::npos) {
    return containsTag(key);
  }
  if (dot == 0) {
    return containsClass(key.substr(1));
  }
  // "tag.class" — both parts must be present in the document.
  // Keys with multiple dots (e.g. "p.a.b") are unreachable by resolveStyle()
  // and correctly fail the class lookup here.
  return containsTag(key.substr(0, dot)) && containsClass(key.substr(dot + 1));
}

bool CssSelectorUsage::scanHtmlFile(const std::string& path) {
  FsFile file;
  if (!Storage.openFileForRead("CSU", path, file)) {
    return false;
  }

  // Minimal streaming markup scanner. EPUB chapters are well-formed XHTML,
  // so attribute values are always quoted; everything else (comments,
  // processing instructions, closing tags) is skipped. Over-collection from
  // odd markup is harmless — it only means loading a few extra CSS rules.
  enum class State : uint8_t {
    Text,           // outside any markup
    TagOpen,        // just consumed '<'
    SkipMarkup,     // "</", "<!", "<?" — ignore until '>'
    TagName,        // reading the element name
    InTag,          // inside a tag, between attributes
    AttrName,       // reading an attribute name
    AfterAttrName,  // after attribute name, before '=' or the next attribute
    BeforeValue,    // after '=', before the opening quote
    AttrValue,      // inside a quoted attribute value
  };

  State state = State::Text;
  char token[MAX_TOKEN_LENGTH];
  size_t tokenLen = 0;
  bool tokenOverflow = false;
  bool inClassAttr = false;
  char quoteChar = '"';

  const auto tokenView = [&]() { return std::string_view(token, tokenLen); };
  const auto resetToken = [&]() {
    tokenLen = 0;
    tokenOverflow = false;
  };
  const auto appendToken = [&](const char c) {
    if (tokenLen < sizeof(token)) {
      token[tokenLen++] = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    } else {
      // Token longer than any realistic tag/class name; drop it rather than
      // record a truncated name that could spuriously match a shorter rule.
      tokenOverflow = true;
    }
  };

  char buffer[READ_BUFFER_SIZE];
  while (file.available()) {
    const int bytesRead = file.read(buffer, sizeof(buffer));
    if (bytesRead <= 0) {
      break;
    }
    for (int i = 0; i < bytesRead; ++i) {
      const char c = buffer[i];
      switch (state) {
        case State::Text:
          if (c == '<') {
            state = State::TagOpen;
          }
          break;
        case State::TagOpen:
          if (c == '/' || c == '!' || c == '?') {
            state = State::SkipMarkup;
          } else if (std::isalpha(static_cast<unsigned char>(c))) {
            resetToken();
            appendToken(c);
            state = State::TagName;
          } else {
            state = State::Text;
          }
          break;
        case State::SkipMarkup:
          if (c == '>') {
            state = State::Text;
          }
          break;
        case State::TagName:
          if (isNameChar(c)) {
            appendToken(c);
          } else {
            if (!tokenOverflow) {
              addTag(tokenView());
            }
            state = (c == '>') ? State::Text : State::InTag;
          }
          break;
        case State::InTag:
          if (c == '>') {
            state = State::Text;
          } else if (std::isalpha(static_cast<unsigned char>(c))) {
            resetToken();
            appendToken(c);
            state = State::AttrName;
          }
          break;
        case State::AttrName:
          if (isNameChar(c)) {
            appendToken(c);
          } else {
            inClassAttr = !tokenOverflow && tokenView() == "class";
            if (c == '=') {
              state = State::BeforeValue;
            } else if (c == '>') {
              state = State::Text;
            } else {
              state = State::AfterAttrName;
            }
          }
          break;
        case State::AfterAttrName:
          if (c == '=') {
            state = State::BeforeValue;
          } else if (c == '>') {
            state = State::Text;
          } else if (std::isalpha(static_cast<unsigned char>(c))) {
            resetToken();
            appendToken(c);
            state = State::AttrName;
          }
          break;
        case State::BeforeValue:
          if (c == '"' || c == '\'') {
            quoteChar = c;
            resetToken();
            state = State::AttrValue;
          } else if (c == '>') {
            state = State::Text;
          } else if (!isHtmlWhitespace(c)) {
            // Unquoted value — invalid in XHTML; skip it as generic tag content
            state = State::InTag;
          }
          break;
        case State::AttrValue:
          if (c == quoteChar) {
            if (inClassAttr && !tokenOverflow) {
              addClass(tokenView());
            }
            state = State::InTag;
          } else if (inClassAttr) {
            if (isHtmlWhitespace(c)) {
              if (!tokenOverflow) {
                addClass(tokenView());
              }
              resetToken();
            } else {
              appendToken(c);
            }
          }
          break;
      }
    }
  }
  file.close();

  // ChapterHtmlSlimParser resolves "img" styles for image elements regardless
  // of the source tag name (e.g. SVG <image>), so always keep img rules.
  addTag("img");

  LOG_DBG("CSU", "Scanned %s: %zu tags, %zu classes%s", path.c_str(), tags_.size(), classes_.size(),
          overflowed_ ? " (overflow, fail-open)" : "");
  return true;
}
