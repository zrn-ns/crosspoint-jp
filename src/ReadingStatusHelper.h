#pragma once

#include <cstdint>
#include <string>

enum class ReadingStatus : uint8_t {
  Unread,    // progress.bin が存在しない
  Reading,   // progress.bin が存在し、読了フラグなし
  Finished   // progress.bin が存在し、読了フラグあり
};

// ファイルパスからSDカード上のキャッシュを確認し、読書状態を返す。
// filepath: 書籍ファイルの絶対パス（例: "/books/sample.epub"）
// cacheDir: キャッシュルート（通常 "/.crosspoint"）
ReadingStatus getReadingStatus(const std::string& filepath, const std::string& cacheDir);

// 指定ファイルを既読（isFinished=1）にマークする。
// progress.bin が存在する場合は末尾バイトのみ更新して読書位置を保持。
// 存在しない場合は新規作成（spineIndex/page等はゼロ初期化）。
// EPUB/XTC 形式を拡張子から自動判定。
// 戻り値: 成功時 true、EPUB/XTC以外やファイルI/O失敗時 false。
bool markAsFinished(const std::string& filepath, const std::string& cacheDir);
