#include "FileBrowserActivity.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Utf8.h>
#include <esp_task_wdt.h>

#include <algorithm>
#include <cstring>
#include <variant>

#include "../util/ConfirmationActivity.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "ReadingStatusHelper.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr unsigned long GO_HOME_MS = 1000;

// 指定ディレクトリ以下の空ディレクトリを再帰的に削除する。
// リーフ（末端）から順に削除するため、ネストした空ディレクトリも連鎖的に削除される。
// 戻り値: 引数のディレクトリ自身が空になり削除された場合 true
bool removeEmptyDirsRecursive(const std::string& dirPath) {
  auto dir = Storage.open(dirPath.c_str());
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return false;
  }

  char name[256];
  bool hasEntries = false;

  // まずサブディレクトリを再帰処理
  dir.rewindDirectory();
  for (auto entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    entry.getName(name, sizeof(name));
    if (entry.isDirectory()) {
      entry.close();
      std::string subPath = dirPath + "/" + name;
      // サブディレクトリを再帰処理。削除されなかったらエントリが残っている
      if (!removeEmptyDirsRecursive(subPath)) {
        hasEntries = true;
      }
    } else {
      // ファイルが存在する → 空ではない
      entry.close();
      hasEntries = true;
    }
    yield();
    esp_task_wdt_reset();
  }
  dir.close();

  if (!hasEntries) {
    if (Storage.rmdir(dirPath.c_str())) {
      LOG_DBG("CLN", "Removed empty directory: %s", dirPath.c_str());
      return true;
    }
  }
  return false;
}

// SDカードルート直下のユーザーディレクトリから空ディレクトリを削除する
void cleanupEmptyDirectories() {
  auto root = Storage.open("/");
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return;
  }

  char name[256];
  // ディレクトリ名を先に収集（イテレーション中の削除を避ける）
  std::vector<std::string> dirs;
  for (auto entry = root.openNextFile(); entry; entry = root.openNextFile()) {
    if (entry.isDirectory()) {
      entry.getName(name, sizeof(name));
      // 保護対象を除外: ドットで始まるディレクトリ、システムディレクトリ
      if (name[0] != '.' && strcmp(name, "System Volume Information") != 0 && strcmp(name, "XTCache") != 0) {
        dirs.emplace_back(std::string("/") + name);
      }
    }
    entry.close();
    yield();
    esp_task_wdt_reset();
  }
  root.close();

  for (const auto& dirPath : dirs) {
    removeEmptyDirsRecursive(dirPath);
  }
}

}  // namespace

void sortFileList(std::vector<std::string>& strs) {
  std::sort(begin(strs), end(strs), [](const std::string& str1, const std::string& str2) {
    // Directories first
    bool isDir1 = str1.back() == '/';
    bool isDir2 = str2.back() == '/';
    if (isDir1 != isDir2) return isDir1;

    // Start naive natural sort
    const char* s1 = str1.c_str();
    const char* s2 = str2.c_str();

    // Iterate while both strings have characters
    while (*s1 && *s2) {
      // Check if both are at the start of a number
      if (isdigit(*s1) && isdigit(*s2)) {
        // Skip leading zeros and track them
        const char* start1 = s1;
        const char* start2 = s2;
        while (*s1 == '0') s1++;
        while (*s2 == '0') s2++;

        // Count digits to compare lengths first
        int len1 = 0, len2 = 0;
        while (isdigit(s1[len1])) len1++;
        while (isdigit(s2[len2])) len2++;

        // Different length so return smaller integer value
        if (len1 != len2) return len1 < len2;

        // Same length so compare digit by digit
        for (int i = 0; i < len1; i++) {
          if (s1[i] != s2[i]) return s1[i] < s2[i];
        }

        // Numbers equal so advance pointers
        s1 += len1;
        s2 += len2;
      } else {
        // Regular case-insensitive character comparison
        char c1 = tolower(*s1);
        char c2 = tolower(*s2);
        if (c1 != c2) return c1 < c2;
        s1++;
        s2++;
      }
    }

    // One string is prefix of other
    return *s1 == '\0' && *s2 != '\0';
  });
}

void FileBrowserActivity::loadFiles() {
  files.clear();
  fileStatuses.clear();

  auto root = Storage.open(basepath.c_str());
  if (!root || !root.isDirectory()) {
    return;
  }

  root.rewindDirectory();

  char name[500];
  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(name, sizeof(name));
    if ((!SETTINGS.showHiddenFiles && name[0] == '.') || strcmp(name, "System Volume Information") == 0) {
      continue;
    }

    if (file.isDirectory()) {
      // Store original (NFD) directory name for path construction.
      // NFC normalization is done at display time only.
      files.emplace_back(std::string(name) + "/");
    } else {
      std::string_view filename{name};
      if (FsHelpers::hasEpubExtension(filename) || FsHelpers::hasXtcExtension(filename) ||
          FsHelpers::hasBmpExtension(filename)) {
        // Store original (NFD) filename for path construction.
        // NFC normalization is done at display time only.
        files.emplace_back(filename);
      }
    }
  }
  sortFileList(files);

  // 各書籍ファイルの読書状態を取得
  std::string fullBase = basepath;
  if (fullBase.back() != '/') fullBase += '/';
  fileStatuses.reserve(files.size());
  for (const auto& file : files) {
    if (FsHelpers::hasEpubExtension(file) || FsHelpers::hasXtcExtension(file)) {
      fileStatuses.push_back(getReadingStatus(fullBase + file, "/.crosspoint"));
    } else {
      fileStatuses.push_back(ReadingStatus::Unread);
    }
  }
}

void FileBrowserActivity::onEnter() {
  Activity::onEnter();

  // ルートディレクトリ表示時のみ、空ディレクトリを削除する
  if (basepath == "/") {
    cleanupEmptyDirectories();
  }

  selectorIndex = 0;

  auto root = Storage.open(basepath.c_str());
  if (!root) {
    basepath = "/";
    loadFiles();
  } else if (!root.isDirectory()) {
    lockLongPressBack = mappedInput.isPressed(MappedInputManager::Button::Back);

    const std::string oldPath = basepath;
    basepath = FsHelpers::extractFolderPath(basepath);
    loadFiles();

    const auto pos = oldPath.find_last_of('/');
    const std::string fileName = oldPath.substr(pos + 1);
    selectorIndex = findEntry(fileName);
  } else {
    loadFiles();
  }

  requestUpdate();
}

void FileBrowserActivity::onExit() {
  Activity::onExit();
  files.clear();
}

void FileBrowserActivity::clearFileMetadata(const std::string& fullPath) {
  // Only clear cache for .epub files
  if (FsHelpers::hasEpubExtension(fullPath)) {
    Epub(fullPath, "/.crosspoint").clearCache();
    LOG_DBG("FileBrowser", "Cleared metadata cache for: %s", fullPath.c_str());
  }
}

void FileBrowserActivity::loop() {
  // Long press BACK (1s+) goes to root folder
  // but Long press BACK (1s+) from ReaderActivity sends us here with the MappedInput already set.
  // So ignore it the first time.
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= GO_HOME_MS &&
      basepath != "/" && !lockLongPressBack) {
    basepath = "/";
    loadFiles();
    selectorIndex = 0;
    requestUpdate();
    return;
  }

  if (lockLongPressBack && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    lockLongPressBack = false;
    return;
  }

  const int pathReserved = renderer.getLineHeight(SMALL_FONT_ID) + UITheme::getInstance().getMetrics().verticalSpacing;
  const int pageItems = UITheme::getNumberOfItemsPerPage(renderer, true, false, true, false, pathReserved);

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (files.empty()) return;

    const std::string& entry = files[selectorIndex];
    bool isDirectory = (entry.back() == '/');

    if (mappedInput.getHeldTime() >= GO_HOME_MS) {
      // --- LONG PRESS ACTION: DELETE FILE/FOLDER ---
      std::string cleanBasePath = basepath;
      if (cleanBasePath.back() != '/') cleanBasePath += "/";
      const std::string fullPath = cleanBasePath + (isDirectory ? entry.substr(0, entry.length() - 1) : entry);

      auto handler = [this, fullPath, isDirectory, entry](const ActivityResult& res) {
        if (!res.isCancelled) {
          // Right ボタン → 削除
          LOG_DBG("FileBrowser", "Attempting to delete: %s", fullPath.c_str());
          if (!isDirectory) clearFileMetadata(fullPath);
          const bool ok = isDirectory ? Storage.removeDir(fullPath.c_str()) : Storage.remove(fullPath.c_str());
          if (ok) {
            LOG_DBG("FileBrowser", "Deleted successfully");
          } else {
            LOG_ERR("FileBrowser", "Failed to delete file: %s", fullPath.c_str());
            return;
          }
        } else if (std::holds_alternative<MenuResult>(res.data)) {
          const int code = std::get<MenuResult>(res.data).action;
          if (code == ConfirmationActivity::RESULT_NEVER) {
            // Left ボタン → アーカイブ（/Archived/ に移動）
            std::string filename = isDirectory ? entry.substr(0, entry.length() - 1) : entry;
            std::string destPath = "/Archived/" + filename;
            Storage.mkdir("/Archived");
            // 同名ファイルが存在する場合は先に削除
            if (Storage.exists(destPath.c_str())) {
              isDirectory ? Storage.removeDir(destPath.c_str()) : Storage.remove(destPath.c_str());
            }
            if (!isDirectory) clearFileMetadata(fullPath);
            if (Storage.rename(fullPath.c_str(), destPath.c_str())) {
              LOG_DBG("FileBrowser", "Archived to: %s", destPath.c_str());
            } else {
              LOG_ERR("FileBrowser", "Failed to archive: %s", fullPath.c_str());
              return;
            }
          } else if (code == ConfirmationActivity::RESULT_MIDDLE) {
            // Confirm ボタン → 既読にする
            if (isDirectory) return;
            if (!markAsFinished(fullPath, "/.crosspoint")) {
              LOG_ERR("FileBrowser", "Failed to mark as finished: %s", fullPath.c_str());
              return;
            }
          } else {
            // Back ボタン → キャンセル
            LOG_DBG("FileBrowser", "Action cancelled by user");
            return;
          }
        } else {
          // Back ボタン → キャンセル
          LOG_DBG("FileBrowser", "Action cancelled by user");
          return;
        }
        // 操作成功後、ファイル一覧を更新（アイコン状態反映のため）
        loadFiles();
        if (files.empty()) {
          selectorIndex = 0;
        } else if (selectorIndex >= files.size()) {
          selectorIndex = files.size() - 1;
        }
        requestUpdate(true);
      };

      std::string heading = entry;

      // ディレクトリには既読操作を提供しない（btn2を空にする）
      const char* markAsReadLabel = isDirectory ? "" : tr(STR_MARK_AS_READ);
      startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, heading, "", tr(STR_ARCHIVE),
                                                                    tr(STR_DELETE), tr(STR_CANCEL), markAsReadLabel),
                             handler);
      return;
    } else {
      // --- SHORT PRESS ACTION: OPEN/NAVIGATE ---
      if (basepath.back() != '/') basepath += "/";

      if (isDirectory) {
        basepath += entry.substr(0, entry.length() - 1);
        loadFiles();
        selectorIndex = 0;
        requestUpdate();
      } else {
        onSelectBook(basepath + entry);
      }
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    // Short press: go up one directory, or go home if at root
    if (mappedInput.getHeldTime() < GO_HOME_MS) {
      if (basepath != "/") {
        const std::string oldPath = basepath;

        basepath.replace(basepath.find_last_of('/'), std::string::npos, "");
        if (basepath.empty()) basepath = "/";
        loadFiles();

        const auto pos = oldPath.find_last_of('/');
        const std::string dirName = oldPath.substr(pos + 1) + "/";
        selectorIndex = findEntry(dirName);

        requestUpdate();
      } else {
        onGoHome();
      }
    }
  }

  int listSize = static_cast<int>(files.size());
  buttonNavigator.onNextRelease([this, listSize] {
    selectorIndex = ButtonNavigator::nextIndex(static_cast<int>(selectorIndex), listSize);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this, listSize] {
    selectorIndex = ButtonNavigator::previousIndex(static_cast<int>(selectorIndex), listSize);
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this, listSize, pageItems] {
    selectorIndex = ButtonNavigator::nextPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this, listSize, pageItems] {
    selectorIndex = ButtonNavigator::previousPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
    requestUpdate();
  });
}

std::string getFileName(std::string filename) {
  // NFC normalize for display (original NFD path is preserved in files[] for SD card access)
  utf8NfcNormalizeKana(filename);
  if (filename.back() == '/') {
    filename.pop_back();
    if (!UITheme::getInstance().getTheme().showsFileIcons()) {
      return "[" + filename + "]";
    }
    return filename;
  }
  const auto pos = filename.rfind('.');
  return filename.substr(0, pos);
}

std::string getFileExtension(std::string filename) {
  if (filename.back() == '/') {
    return "";
  }
  const auto pos = filename.rfind('.');
  return filename.substr(pos);
}

void FileBrowserActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  std::string folderName = (basepath == "/") ? tr(STR_SD_CARD) : basepath.substr(basepath.rfind('/') + 1);
  utf8NfcNormalizeKana(folderName);
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, folderName.c_str());

  const int pathLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int pathReserved = pathLineHeight + metrics.verticalSpacing;
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight =
      pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing - pathReserved;
  if (files.empty()) {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, tr(STR_NO_FILES_FOUND));
  } else {
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, files.size(), selectorIndex,
        [this](int index) { return getFileName(files[index]); }, nullptr,
        [this](int index) { return UITheme::getFileIcon(files[index], fileStatuses[index]); },
        [this](int index) { return getFileExtension(files[index]); }, false);
  }

  // Full path display
  {
    const int pathY = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing - pathLineHeight;
    const int separatorY = pathY - metrics.verticalSpacing / 2;
    renderer.drawLine(0, separatorY, pageWidth - 1, separatorY, 3, true);
    const int pathMaxWidth = pageWidth - metrics.contentSidePadding * 2;
    // Left-truncate so the deepest directory is always visible
    const char* pathStr = basepath.c_str();
    const char* pathDisplay = pathStr;
    char leftTruncBuf[256];
    if (renderer.getTextWidth(SMALL_FONT_ID, pathStr) > pathMaxWidth) {
      const char ellipsis[] = "\xe2\x80\xa6";  // UTF-8 ellipsis (…)
      const int ellipsisWidth = renderer.getTextWidth(SMALL_FONT_ID, ellipsis);
      const int available = pathMaxWidth - ellipsisWidth;
      // Walk forward from the start until the suffix fits, skipping UTF-8 continuation bytes
      const char* p = pathStr;
      while (*p) {
        if (renderer.getTextWidth(SMALL_FONT_ID, p) <= available) break;
        ++p;
        while (*p && (static_cast<unsigned char>(*p) & 0xC0) == 0x80) ++p;
      }
      snprintf(leftTruncBuf, sizeof(leftTruncBuf), "%s%s", ellipsis, p);
      pathDisplay = leftTruncBuf;
    }
    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, pathY, pathDisplay);
  }

  // Help text
  const auto labels =
      mappedInput.mapLabels(basepath == "/" ? tr(STR_HOME) : tr(STR_BACK), files.empty() ? "" : tr(STR_OPEN),
                            files.empty() ? "" : tr(STR_DIR_UP), files.empty() ? "" : tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

size_t FileBrowserActivity::findEntry(const std::string& name) const {
  for (size_t i = 0; i < files.size(); i++)
    if (files[i] == name) return i;
  return 0;
}
