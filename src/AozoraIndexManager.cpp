#include "AozoraIndexManager.h"
#include <ArduinoJson.h>
#include <Logging.h>

bool AozoraIndexManager::loadAndPurge() {
  entries_.clear();

  FsFile file;
  if (!Storage.openFileForRead("AOZORA", INDEX_PATH, file)) {
    LOG_DBG("AOZORA", "No index file, starting fresh");
    return true;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, file);
  file.close();

  if (err) {
    LOG_ERR("AOZORA", "Index parse error: %s", err.c_str());
    return true;
  }

  JsonArray arr = doc.as<JsonArray>();
  entries_.reserve(arr.size());

  bool needsSave = false;
  for (JsonObject obj : arr) {
    AozoraBookEntry entry;
    entry.workId = obj["work_id"] | 0;
    snprintf(entry.title, sizeof(entry.title), "%s", (const char*)(obj["title"] | ""));
    snprintf(entry.author, sizeof(entry.author), "%s", (const char*)(obj["author"] | ""));
    snprintf(entry.filename, sizeof(entry.filename), "%s", (const char*)(obj["filename"] | ""));

    char fullPath[160];
    snprintf(fullPath, sizeof(fullPath), "%s/%s", AOZORA_DIR, entry.filename);

    if (Storage.exists(fullPath)) {
      entries_.push_back(entry);
    } else {
      LOG_DBG("AOZORA", "Purging missing: %s", entry.filename);
      needsSave = true;
    }
  }

  if (needsSave) {
    saveIndex();
  }

  return true;
}

bool AozoraIndexManager::isDownloaded(int workId) const {
  for (const auto& e : entries_) {
    if (e.workId == workId) return true;
  }
  return false;
}

bool AozoraIndexManager::addEntry(int workId, const char* title, const char* author, const char* filename) {
  if (isDownloaded(workId)) return true;

  AozoraBookEntry entry;
  entry.workId = workId;
  snprintf(entry.title, sizeof(entry.title), "%s", title);
  snprintf(entry.author, sizeof(entry.author), "%s", author);
  snprintf(entry.filename, sizeof(entry.filename), "%s", filename);
  entries_.push_back(entry);

  return saveIndex();
}

bool AozoraIndexManager::removeEntry(int workId) {
  for (auto it = entries_.begin(); it != entries_.end(); ++it) {
    if (it->workId == workId) {
      char fullPath[160];
      snprintf(fullPath, sizeof(fullPath), "%s/%s", AOZORA_DIR, it->filename);
      Storage.remove(fullPath);
      entries_.erase(it);
      return saveIndex();
    }
  }
  return false;
}

bool AozoraIndexManager::saveIndex() const {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();

  for (const auto& e : entries_) {
    JsonObject obj = arr.add<JsonObject>();
    obj["work_id"] = e.workId;
    obj["title"] = e.title;
    obj["author"] = e.author;
    obj["filename"] = e.filename;
  }

  FsFile file;
  if (!Storage.openFileForWrite("AOZORA", INDEX_PATH, file)) {
    LOG_ERR("AOZORA", "Failed to open index for write");
    return false;
  }

  serializeJson(doc, file);
  file.close();
  return true;
}

std::string AozoraIndexManager::makeFilename(int workId, const char* title) {
  char safeName[52];
  int pos = 0;
  for (int i = 0; title[i] && pos < 50; i++) {
    unsigned char c = static_cast<unsigned char>(title[i]);
    if (c == '<' || c == '>' || c == ':' || c == '"' ||
        c == '/' || c == '\\' || c == '|' || c == '?' || c == '*') {
      safeName[pos++] = '_';
    } else {
      safeName[pos++] = static_cast<char>(c);
    }
  }
  safeName[pos] = '\0';

  char result[80];
  snprintf(result, sizeof(result), "%d_%s.epub", workId, safeName);
  return std::string(result);
}

bool AozoraIndexManager::ensureDirectory() {
  if (Storage.exists(AOZORA_DIR)) return true;
  return Storage.mkdir(AOZORA_DIR);
}
