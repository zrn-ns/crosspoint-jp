#include "FavoriteAuthorsManager.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>

bool FavoriteAuthorsManager::load() {
  entries_.clear();

  FsFile file;
  if (!Storage.openFileForRead("FAVAUTH", FAVORITES_PATH, file)) {
    LOG_DBG("FAVAUTH", "No favorites file, starting fresh");
    return true;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, file);
  file.close();

  if (err) {
    LOG_ERR("FAVAUTH", "Favorites parse error: %s", err.c_str());
    return true;
  }

  JsonArray arr = doc.as<JsonArray>();
  entries_.reserve(arr.size());

  for (JsonObject obj : arr) {
    FavoriteAuthor entry;
    entry.authorId = obj["author_id"] | 0;
    snprintf(entry.name, sizeof(entry.name), "%s", (const char*)(obj["name"] | ""));
    snprintf(entry.kana, sizeof(entry.kana), "%s", (const char*)(obj["kana"] | ""));
    entries_.push_back(entry);
  }

  sortEntries();
  LOG_DBG("FAVAUTH", "Loaded %zu favorites", entries_.size());
  return true;
}

bool FavoriteAuthorsManager::save() const {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();

  for (const auto& e : entries_) {
    JsonObject obj = arr.add<JsonObject>();
    obj["author_id"] = e.authorId;
    obj["name"] = e.name;
    obj["kana"] = e.kana;
  }

  FsFile file;
  if (!Storage.openFileForWrite("FAVAUTH", FAVORITES_PATH, file)) {
    LOG_ERR("FAVAUTH", "Failed to open favorites for write");
    return false;
  }

  serializeJson(doc, file);
  file.close();
  return true;
}

void FavoriteAuthorsManager::addAuthor(int id, const char* name, const char* kana) {
  if (isFavorited(id)) return;

  FavoriteAuthor entry;
  entry.authorId = id;
  snprintf(entry.name, sizeof(entry.name), "%s", name);
  snprintf(entry.kana, sizeof(entry.kana), "%s", kana);
  entries_.push_back(entry);
  sortEntries();
  save();
}

void FavoriteAuthorsManager::removeAuthor(int id) {
  for (auto it = entries_.begin(); it != entries_.end(); ++it) {
    if (it->authorId == id) {
      entries_.erase(it);
      save();
      return;
    }
  }
}

bool FavoriteAuthorsManager::isFavorited(int id) const {
  for (const auto& e : entries_) {
    if (e.authorId == id) return true;
  }
  return false;
}

void FavoriteAuthorsManager::sortEntries() {
  std::sort(entries_.begin(), entries_.end(),
            [](const FavoriteAuthor& a, const FavoriteAuthor& b) { return strcmp(a.kana, b.kana) < 0; });
}
