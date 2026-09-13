#include "ReadingStatusHelper.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <esp_task_wdt.h>

#include <algorithm>
#include <cstdlib>
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
  std::string progressPath = cacheDir + "/" + prefix + std::to_string(FsHelpers::pathHash(filepath)) + "/progress.bin";

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

namespace {

// 既に開いているキャッシュディレクトリのハンドルから progress.bin を探して読書状態を返す。
// パス指定で開き直すと FAT のディレクトリ検索がもう一度走るため、
// openNextFile() で相対的にたどる。
ReadingStatus readStatusFromCacheDir(FsFile& bookDir, bool isEpub) {
  // 読了フラグの位置: EPUB=byte6, XTC=byte4
  const int flagOffset = isEpub ? 6 : 4;

  char name[64];
  bookDir.rewindDirectory();
  for (auto child = bookDir.openNextFile(); child; child = bookDir.openNextFile()) {
    child.getName(name, sizeof(name));
    if (strcmp(name, "progress.bin") != 0) {
      child.close();
      // 通常このループは数件で終わるが、エントリ数の多いディレクトリを
      // 掴んだ場合に外側のループまでウォッチドッグを待たせないようにする
      yield();
      esp_task_wdt_reset();
      continue;
    }

    // ファイル全体を読み取り（最大7バイト: EPUB新フォーマット）
    uint8_t data[7] = {0};
    const int bytesRead = child.read(data, sizeof(data));
    child.close();
    if (bytesRead <= 0) {
      return ReadingStatus::Unread;
    }
    return (bytesRead > flagOffset && data[flagOffset] == 1) ? ReadingStatus::Finished : ReadingStatus::Reading;
  }
  return ReadingStatus::Unread;
}

}  // namespace

ReadingStatusIndex::ReadingStatusIndex(const std::string& cacheDir) {
  auto root = Storage.open(cacheDir.c_str());
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return;
  }

  // 蔵書数は事前に分からないため控えめに確保しておき、再確保によるDRAM断片化を抑える
  epubEntries.reserve(32);
  xtcEntries.reserve(8);

  char name[64];
  root.rewindDirectory();
  for (auto entry = root.openNextFile(); entry; entry = root.openNextFile()) {
    if (!entry.isDirectory()) {
      entry.close();
      continue;
    }
    entry.getName(name, sizeof(name));

    bool isEpub;
    const char* digits;
    if (strncmp(name, "epub_", 5) == 0) {
      isEpub = true;
      digits = name + 5;
    } else if (strncmp(name, "xtc_", 4) == 0) {
      isEpub = false;
      digits = name + 4;
    } else {
      entry.close();
      continue;
    }

    char* end = nullptr;
    const unsigned long long parsed = strtoull(digits, &end, 10);
    if (end == digits || *end != '\0') {
      entry.close();
      continue;
    }

    const ReadingStatus status = readStatusFromCacheDir(entry, isEpub);
    entry.close();

    // 未読はキャッシュが無い場合と同じ扱いなので保持しない（メモリ節約）
    if (status != ReadingStatus::Unread) {
      (isEpub ? epubEntries : xtcEntries).push_back(Entry{static_cast<size_t>(parsed), status});
    }

    yield();
    esp_task_wdt_reset();
  }
  root.close();

  const auto byKey = [](const Entry& a, const Entry& b) { return a.key < b.key; };
  std::sort(epubEntries.begin(), epubEntries.end(), byKey);
  std::sort(xtcEntries.begin(), xtcEntries.end(), byKey);

  LOG_DBG("RSH", "Reading status index: %u epub, %u xtc", static_cast<unsigned>(epubEntries.size()),
          static_cast<unsigned>(xtcEntries.size()));
}

ReadingStatus ReadingStatusIndex::lookup(const std::string& filepath) const {
  const bool isEpub = FsHelpers::hasEpubExtension(filepath);
  if (!isEpub && !FsHelpers::hasXtcExtension(filepath)) {
    return ReadingStatus::Unread;
  }
  return lookupIn(isEpub ? epubEntries : xtcEntries, FsHelpers::pathHash(filepath));
}

ReadingStatus ReadingStatusIndex::lookupIn(const std::vector<Entry>& entries, size_t key) {
  const auto it =
      std::lower_bound(entries.begin(), entries.end(), key, [](const Entry& e, size_t k) { return e.key < k; });
  if (it == entries.end() || it->key != key) {
    return ReadingStatus::Unread;
  }
  return it->status;
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

  const std::string hash = std::to_string(FsHelpers::pathHash(filepath));
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
