#pragma once

// 本文中に「1 文字ぶんの大きさ」で置かれる画像（外字の代用など）を、
// TextBlock の語のひとつとして扱うためのヘルパ。
//
// 背景: レイアウトエンジンは <img> を常にブロック要素として扱い、直前の本文を確定させる。
// 縦書きでは 1 文字ぶんの外字画像でも段落がそこで分断され、画像だけが本文と無関係な
// 位置に置かれていた（Issue #107）。画像を語の列に混ぜれば、列送り・改ページが
// 本文と同じ経路を通る。
//
// 語文字列の書式: '\x01' 幅 ',' 高さ ',' 画像パス
// 先頭の 0x01 は UTF-8 の本文には現れない制御文字なので、通常の語と衝突しない。
// 文字列で表すことで、セクションキャッシュ（section.bin）の形式を変えずに済む。

#include <cstdlib>
#include <string>

namespace InlineImage {

constexpr char MARKER = '\x01';

inline std::string encode(const std::string& path, const int width, const int height) {
  return std::string(1, MARKER) + std::to_string(width) + "," + std::to_string(height) + "," + path;
}

inline bool isInlineImage(const std::string& word) { return !word.empty() && word[0] == MARKER; }

// 画像語を分解する。成功したら path / width / height を埋めて true を返す。
inline bool decode(const std::string& word, std::string& path, int& width, int& height) {
  if (!isInlineImage(word)) return false;
  const size_t firstComma = word.find(',', 1);
  if (firstComma == std::string::npos) return false;
  const size_t secondComma = word.find(',', firstComma + 1);
  if (secondComma == std::string::npos) return false;

  width = atoi(word.substr(1, firstComma - 1).c_str());
  height = atoi(word.substr(firstComma + 1, secondComma - firstComma - 1).c_str());
  path = word.substr(secondComma + 1);
  return width > 0 && height > 0 && !path.empty();
}

}  // namespace InlineImage
