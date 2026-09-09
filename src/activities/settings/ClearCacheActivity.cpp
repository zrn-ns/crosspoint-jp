#include "ClearCacheActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

void ClearCacheActivity::onEnter() {
  Activity::onEnter();

  state = WARNING;
  requestUpdate();
}

void ClearCacheActivity::onExit() { Activity::onExit(); }

void ClearCacheActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // ボタンヒントの領域を除いた矩形を基準にする（横向きでは短辺側の帯を避ける）
  const Rect area = UITheme::getContentArea(renderer);
  const int centerY = area.y + area.height / 2;
  auto drawCentered = [&](const int y, const char* text, const EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
    const int x = area.x + (area.width - renderer.getTextWidth(UI_10_FONT_ID, text, style)) / 2;
    renderer.drawText(UI_10_FONT_ID, x, y, text, true, style);
  };

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{area.x, area.y + metrics.topPadding, area.width, metrics.headerHeight},
                 tr(STR_CLEAR_READING_CACHE));

  if (state == WARNING) {
    drawCentered(centerY - 60, tr(STR_CLEAR_CACHE_WARNING_1));
    drawCentered(centerY - 30, tr(STR_CLEAR_CACHE_WARNING_2), EpdFontFamily::BOLD);
    drawCentered(centerY + 10, tr(STR_CLEAR_CACHE_WARNING_3));
    drawCentered(centerY + 30, tr(STR_CLEAR_CACHE_WARNING_4));

    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_CLEAR_BUTTON), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == CLEARING) {
    drawCentered(centerY, tr(STR_CLEARING_CACHE));
    renderer.displayBuffer();
    return;
  }

  if (state == SUCCESS) {
    drawCentered(centerY - 20, tr(STR_CACHE_CLEARED), EpdFontFamily::BOLD);
    std::string resultText = std::to_string(clearedCount) + " " + std::string(tr(STR_ITEMS_REMOVED));
    if (failedCount > 0) {
      resultText += ", " + std::to_string(failedCount) + " " + std::string(tr(STR_FAILED_LOWER));
    }
    drawCentered(centerY + 10, resultText.c_str());

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == FAILED) {
    drawCentered(centerY - 20, tr(STR_CLEAR_CACHE_FAILED), EpdFontFamily::BOLD);
    drawCentered(centerY + 10, tr(STR_CHECK_SERIAL_OUTPUT));

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }
}

void ClearCacheActivity::clearCache() {
  LOG_DBG("CLEAR_CACHE", "Clearing cache...");

  // Open .crosspoint directory
  auto root = Storage.open("/.crosspoint");
  if (!root || !root.isDirectory()) {
    LOG_DBG("CLEAR_CACHE", "Failed to open cache directory");
    if (root) root.close();
    state = FAILED;
    requestUpdate();
    return;
  }

  clearedCount = 0;
  failedCount = 0;
  char name[128];

  // Iterate through all entries in the directory
  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(name, sizeof(name));
    String itemName(name);

    // Only delete directories starting with epub_ or xtc_
    if (file.isDirectory() && (itemName.startsWith("epub_") || itemName.startsWith("xtc_"))) {
      String fullPath = "/.crosspoint/" + itemName;
      LOG_DBG("CLEAR_CACHE", "Removing cache: %s", fullPath.c_str());

      file.close();  // Close before attempting to delete

      if (Storage.removeDir(fullPath.c_str())) {
        clearedCount++;
      } else {
        LOG_ERR("CLEAR_CACHE", "Failed to remove: %s", fullPath.c_str());
        failedCount++;
      }
    } else {
      file.close();
    }
  }
  root.close();

  LOG_DBG("CLEAR_CACHE", "Cache cleared: %d removed, %d failed", clearedCount, failedCount);

  state = SUCCESS;
  requestUpdate();
}

void ClearCacheActivity::loop() {
  if (state == WARNING) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      LOG_DBG("CLEAR_CACHE", "User confirmed, starting cache clear");
      {
        RenderLock lock(*this);
        state = CLEARING;
      }
      requestUpdateAndWait();

      clearCache();
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      LOG_DBG("CLEAR_CACHE", "User cancelled");
      goBack();
    }
    return;
  }

  if (state == SUCCESS || state == FAILED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      goBack();
    }
    return;
  }
}
