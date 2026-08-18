#pragma once

#include <string>
#include <string_view>
#include <vector>

/**
 * Collects the set of tag names and class names actually used by a chapter's
 * (X)HTML document, so CSS cache loading can skip rules that can never match.
 *
 * CssParser::resolveStyle() only ever looks up "tag", ".class" and
 * "tag.class" keys, so a cached rule is worth loading into RAM iff its tag
 * and class parts appear somewhere in the document. Generic publisher
 * stylesheets (e.g. the EBPAJ template used by most Japanese EPUBs) register
 * hundreds of rules of which a typical chapter references a handful; skipping
 * the rest keeps tens of KB of heap free during section building (issue #105).
 */
class CssSelectorUsage {
 public:
  /**
   * Stream the (X)HTML file at path and record used tag and class names.
   * @return false if the file could not be opened; callers should then fall
   *         back to loading the full CSS cache.
   */
  bool scanHtmlFile(const std::string& path);

  /**
   * Whether a CSS cache key ("tag", ".class" or "tag.class") can match this
   * document. Fails open (returns true) when the collection overflowed.
   */
  [[nodiscard]] bool matches(const std::string& selectorKey) const;

  [[nodiscard]] size_t tagCount() const { return tags_.size(); }
  [[nodiscard]] size_t classCount() const { return classes_.size(); }

 private:
  // Caps keep pathological documents from growing the sets unbounded;
  // on overflow we fail open and matches() accepts every rule.
  static constexpr size_t MAX_TAGS = 64;
  static constexpr size_t MAX_CLASSES = 256;
  static constexpr size_t MAX_TOKEN_LENGTH = 128;

  void addTag(std::string_view name);
  void addClass(std::string_view name);
  [[nodiscard]] bool containsTag(std::string_view name) const { return contains(tags_, name); }
  [[nodiscard]] bool containsClass(std::string_view name) const { return contains(classes_, name); }
  static bool contains(const std::vector<std::string>& list, std::string_view name);

  // Small sets (tens of entries) held as flat vectors with linear lookup;
  // an unordered_set would cost one heap node per entry for no gain here.
  std::vector<std::string> tags_;
  std::vector<std::string> classes_;
  bool overflowed_ = false;
};
