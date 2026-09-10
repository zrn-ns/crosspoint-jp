#pragma once

#include <cstdint>
#include <string>
#include <vector>

enum class ReadingStatus : uint8_t {
  Unread,   // progress.bin が存在しない
  Reading,  // progress.bin が存在し、読了フラグなし
  Finished  // progress.bin が存在し、読了フラグあり
};

// ファイルパスからSDカード上のキャッシュを確認し、読書状態を返す。
// filepath: 書籍ファイルの絶対パス（例: "/books/sample.epub"）
// cacheDir: キャッシュルート（通常 "/.crosspoint"）
//
// 数件だけ引く用途（ホーム画面の「最近の本」など）向け。
// 一覧全体の状態が要る場合は ReadingStatusIndex を使うこと。
ReadingStatus getReadingStatus(const std::string& filepath, const std::string& cacheDir);

// キャッシュディレクトリを1回だけ順次走査して、書籍ごとの読書状態をまとめて持つインデックス。
//
// getReadingStatus() は1件につき /.crosspoint/<prefix><hash>/progress.bin の
// パス解決を2回（exists + open）行う。FATのディレクトリ検索は線形走査なので、
// 蔵書 M 冊ぶんのキャッシュがある状態で N 個のファイルを一覧すると
// O(N×M) のセクタ読み出しになり、ファイルが増えるほど一覧表示が極端に遅くなる（Issue #136）。
//
// このクラスは /.crosspoint を openNextFile() で1回だけ順次走査し、
// 各キャッシュディレクトリの中も開いたハンドルから相対的にたどって progress.bin を読む。
// 全体が O(M) のシーケンシャル読み出しに収まり、パス解決は一切発生しない。
class ReadingStatusIndex {
 public:
  explicit ReadingStatusIndex(const std::string& cacheDir);

  // filepath の読書状態を返す。EPUB/XTC 以外は常に Unread。
  ReadingStatus lookup(const std::string& filepath) const;

 private:
  // key = FsHelpers::pathHash()。実機の size_t は4バイトなので1件8バイト。
  // 読書中／読了の本だけを保持し、未読（＝キャッシュなし）は入れない。
  // 一覧の生成中だけ持って破棄するため、蔵書1000冊でも一時的に8KB程度で済む。
  struct Entry {
    size_t key;
    ReadingStatus status;
  };

  std::vector<Entry> epubEntries;  // key 昇順
  std::vector<Entry> xtcEntries;   // key 昇順

  static ReadingStatus lookupIn(const std::vector<Entry>& entries, size_t key);
};

// 指定ファイルを既読（isFinished=1）にマークする。
// progress.bin が存在する場合は末尾バイトのみ更新して読書位置を保持。
// 存在しない場合は新規作成（spineIndex/page等はゼロ初期化）。
// EPUB/XTC 形式を拡張子から自動判定。
// 戻り値: 成功時 true、EPUB/XTC以外やファイルI/O失敗時 false。
bool markAsFinished(const std::string& filepath, const std::string& cacheDir);
