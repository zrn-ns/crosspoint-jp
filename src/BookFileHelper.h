#pragma once

#include <string>

// 書籍ファイルのアーカイブ・削除。ファイル一覧の長押しメニューと、読了時の確認
// ダイアログ（#47）の両方から呼ばれるので、SD 上の操作をここに集約する。
namespace BookFileHelper {

// アーカイブ先のディレクトリ（SD ルート直下）。
constexpr const char* ARCHIVE_DIR = "/Archived";

// fullPath を /Archived/ 配下へ移動する。同名のエントリが既にあれば置き換える。
// EPUB は移動前にキャッシュを削除する（キャッシュ名はパスのハッシュなので、
// 移動後は参照できなくなり孤児になるだけ）。
// isDirectory: fullPath がフォルダなら true（末尾の '/' は含めない）。
bool archive(const std::string& fullPath, bool isDirectory);

// fullPath を削除する。EPUB はキャッシュも合わせて削除する。
bool remove(const std::string& fullPath, bool isDirectory);

}  // namespace BookFileHelper
