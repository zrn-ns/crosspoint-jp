#pragma once

#include <I18n.h>

#include <string>
#include <vector>

#include "FontInstaller.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

#ifndef FONT_MANIFEST_URL
#define FONT_MANIFEST_URL "https://github.com/zrn-ns/crosspoint-jp/releases/download/sd-fonts/fonts.json"
#endif

class FontDownloadActivity : public Activity {
 public:
  explicit FontDownloadActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override {
    return state_ == LOADING_MANIFEST || state_ == DOWNLOADING ||
           // The download is synchronous and blocks the main loop until it
           // completes, so activityManager.preventAutoSleep() is never polled
           // during downloading.
           state_ == COMPLETE || state_ == ERROR;
  }
  bool skipLoopDelay() override { return true; }

 private:
  enum State {
    WIFI_SELECTION,
    LOADING_MANIFEST,
    FAMILY_LIST,
    CONFIRM_DOWNLOAD,
    CONFIRM_DELETE,
    DOWNLOADING,
    COMPLETE,
    ERROR,
  };

  // Fixed-size records rather than std::string members. The manifest holds 25
  // families and 108 files; as std::string those cost 200+ separate heap
  // allocations scattered across the arena, and the per-family files vector
  // grew without reserve() on top of that. The resulting fragmentation is what
  // starved the ~45-55KB contiguous block a TLS handshake needs — the font
  // download failed with ESP_ERR_HTTP_CONNECT while 70KB was still free but the
  // largest block was only 37KB. Capacities are the measured manifest maxima
  // (file name 30, family name 20, description 72) plus headroom; anything
  // longer is truncated, which only shortens a label.
  struct ManifestFile {
    char name[40] = {0};
    uint32_t size = 0;
  };

  struct ManifestFamily {
    char name[32] = {0};
    char description[80] = {0};
    uint16_t fileStart = 0;  // index of this family's first entry in allFiles_
    uint8_t fileCount = 0;
    uint32_t totalSize = 0;
    bool installed = false;
    bool hasUpdate = false;
  };

  State state_ = WIFI_SELECTION;
  FontInstaller fontInstaller_;
  ButtonNavigator buttonNavigator_;

  // Manifest data. Two heap blocks total: one for families_, one for the flat
  // file list every family indexes into.
  char baseUrl_[96] = {0};
  std::vector<ManifestFamily> families_;
  std::vector<ManifestFile> allFiles_;
  int selectedIndex_ = 0;

  // Download progress
  size_t currentFileIndex_ = 0;
  size_t currentFileTotal_ = 0;
  size_t fileProgress_ = 0;
  size_t fileTotal_ = 0;
  int downloadingFamilyIndex_ = 0;
  // Message shown on the COMPLETE screen: install and delete both land there.
  StrId completeMessage_ = StrId::STR_FONT_INSTALLED;
  std::string errorMessage_;
  // Second error line: err/http/heap diagnostics. Kept separate from
  // errorMessage_ so the failing file name stays readable on its own line.
  std::string errorDetail_;

  void onWifiSelectionComplete(bool success);
  bool fetchAndParseManifest();
  void downloadFamily(ManifestFamily& family);
  void downloadAll();
  void deleteFamily(ManifestFamily& family);
  // The selected list row, or nullptr for "Download all" / an empty manifest.
  ManifestFamily* selectedFamily();
  bool isDownloadAllSelected() const { return selectedIndex_ == 0 && !families_.empty(); }
  int familyIndexFromList(int listIndex) const { return listIndex - 1; }
  int listItemCount() const { return families_.empty() ? 0 : static_cast<int>(families_.size()) + 1; }
  size_t totalUninstalledSize() const;
  static std::string formatSize(size_t bytes);
};
