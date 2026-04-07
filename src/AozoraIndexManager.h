#pragma once

#include <string>
#include <vector>
#include <HalStorage.h>

struct AozoraBookEntry {
  int workId;
  char title[64];
  char author[32];
  char filename[80];
};

class AozoraIndexManager {
 public:
  static constexpr const char* AOZORA_DIR = "/Aozora";
  static constexpr const char* INDEX_PATH = "/Aozora/.aozora_index.json";

  bool loadAndPurge();
  bool isDownloaded(int workId) const;
  bool addEntry(int workId, const char* title, const char* author, const char* filename);
  bool removeEntry(int workId);
  const std::vector<AozoraBookEntry>& entries() const { return entries_; }
  static std::string makeFilename(int workId, const char* title);
  static bool ensureDirectory();

 private:
  std::vector<AozoraBookEntry> entries_;
  bool saveIndex() const;
};
