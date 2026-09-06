#include "BookFileHelper.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>

namespace {

// EPUB のみキャッシュを持つ。パスのハッシュで引くので、移動・削除の前に消す。
void clearEpubCache(const std::string& fullPath) {
  if (FsHelpers::hasEpubExtension(fullPath)) {
    Epub(fullPath, "/.crosspoint").clearCache();
    LOG_DBG("BookFile", "Cleared metadata cache for: %s", fullPath.c_str());
  }
}

bool removeEntry(const std::string& path, bool isDirectory) {
  return isDirectory ? Storage.removeDir(path.c_str()) : Storage.remove(path.c_str());
}

}  // namespace

namespace BookFileHelper {

bool archive(const std::string& fullPath, bool isDirectory) {
  const size_t slash = fullPath.find_last_of('/');
  const std::string filename = slash == std::string::npos ? fullPath : fullPath.substr(slash + 1);
  if (filename.empty()) {
    LOG_ERR("BookFile", "Cannot archive path without a name: %s", fullPath.c_str());
    return false;
  }
  const std::string destPath = std::string(ARCHIVE_DIR) + "/" + filename;
  Storage.mkdir(ARCHIVE_DIR);
  // 同名が存在する場合は先に削除する（rename は上書きしない）
  if (Storage.exists(destPath.c_str())) {
    removeEntry(destPath, isDirectory);
  }
  if (!isDirectory) clearEpubCache(fullPath);
  if (!Storage.rename(fullPath.c_str(), destPath.c_str())) {
    LOG_ERR("BookFile", "Failed to archive: %s", fullPath.c_str());
    return false;
  }
  LOG_DBG("BookFile", "Archived to: %s", destPath.c_str());
  return true;
}

bool remove(const std::string& fullPath, bool isDirectory) {
  LOG_DBG("BookFile", "Attempting to delete: %s", fullPath.c_str());
  if (!isDirectory) clearEpubCache(fullPath);
  if (!removeEntry(fullPath, isDirectory)) {
    LOG_ERR("BookFile", "Failed to delete file: %s", fullPath.c_str());
    return false;
  }
  LOG_DBG("BookFile", "Deleted successfully");
  return true;
}

}  // namespace BookFileHelper
