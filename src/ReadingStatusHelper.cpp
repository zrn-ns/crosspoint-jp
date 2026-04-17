#include "ReadingStatusHelper.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstring>
#include <functional>
#include <string>

ReadingStatus getReadingStatus(const std::string& filepath, const std::string& cacheDir) {
  // EPUB/XTC以外は常にUnread（アイコン対象外のため到達しないが安全策）
  const char* prefix;
  if (FsHelpers::hasEpubExtension(filepath)) {
    prefix = "epub_";
  } else if (FsHelpers::hasXtcExtension(filepath)) {
    prefix = "xtc_";
  } else {
    return ReadingStatus::Unread;
  }

  // progress.bin パスを構築
  std::string progressPath = cacheDir + "/" + prefix +
                             std::to_string(std::hash<std::string>{}(filepath)) + "/progress.bin";

  FsFile f;
  if (!Storage.openFileForRead("RSH", progressPath, f)) {
    return ReadingStatus::Unread;
  }

  // ファイル全体を読み取り（最大7バイト: EPUB新フォーマット）
  uint8_t data[7];
  int bytesRead = f.read(data, sizeof(data));
  f.close();

  if (bytesRead <= 0) {
    return ReadingStatus::Unread;
  }

  // 読了フラグの位置: EPUB=byte6, XTC=byte4
  int flagOffset = FsHelpers::hasEpubExtension(filepath) ? 6 : 4;

  if (bytesRead > flagOffset && data[flagOffset] == 1) {
    return ReadingStatus::Finished;
  }

  return ReadingStatus::Reading;
}

bool markAsFinished(const std::string& filepath, const std::string& cacheDir) {
  const char* prefix;
  bool isEpub;
  if (FsHelpers::hasEpubExtension(filepath)) {
    prefix = "epub_";
    isEpub = true;
  } else if (FsHelpers::hasXtcExtension(filepath)) {
    prefix = "xtc_";
    isEpub = false;
  } else {
    return false;
  }

  const std::string hash = std::to_string(std::hash<std::string>{}(filepath));
  const std::string bookDir = cacheDir + "/" + prefix + hash;
  const std::string progressPath = bookDir + "/progress.bin";

  // EPUB=7, XTC=5
  const size_t recordSize = isEpub ? 7 : 5;
  const size_t flagOffset = isEpub ? 6 : 4;

  // 既存progress.binを読み込んで読書位置を保持する（なければゼロ初期化）
  uint8_t data[7] = {0};
  FsFile rf;
  if (Storage.openFileForRead("RSH", progressPath, rf)) {
    rf.read(data, recordSize);
    rf.close();
  }
  data[flagOffset] = 1;

  // ディレクトリを確保してから書き込む
  Storage.mkdir(cacheDir.c_str());
  Storage.mkdir(bookDir.c_str());

  FsFile wf;
  if (!Storage.openFileForWrite("RSH", progressPath, wf)) {
    LOG_ERR("RSH", "markAsFinished: Could not open %s for write", progressPath.c_str());
    return false;
  }
  wf.write(data, recordSize);
  wf.close();
  LOG_DBG("RSH", "Marked as finished: %s", filepath.c_str());
  return true;
}
