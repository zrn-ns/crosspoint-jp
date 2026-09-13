#pragma once

#include <functional>
#include <string>
#include <vector>

#include "../Activity.h"
#include "ReadingStatusHelper.h"
#include "RecentBooksStore.h"
#include "util/ButtonNavigator.h"

class FileBrowserActivity final : public Activity {
 private:
  ButtonNavigator buttonNavigator;

  size_t selectorIndex = 0;

  bool lockLongPressBack = false;

  // 初回描画のあとに空ディレクトリの掃除を走らせるためのフラグ（Issue #136）
  bool pendingCleanup = false;

  // Files state
  std::string basepath = "/";
  std::vector<std::string> files;
  std::vector<ReadingStatus> fileStatuses;

  // Data loading
  void loadFiles();
  size_t findEntry(const std::string& name) const;

 public:
  explicit FileBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string initialPath = "/")
      : Activity("FileBrowser", renderer, mappedInput), basepath(initialPath.empty() ? "/" : std::move(initialPath)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
