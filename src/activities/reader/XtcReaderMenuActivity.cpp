#include "XtcReaderMenuActivity.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

XtcReaderMenuActivity::XtcReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                             const std::string& title, const uint32_t currentPage,
                                             const uint32_t totalPages, const bool hasChapters)
    : Activity("XtcReaderMenu", renderer, mappedInput),
      menuItems(buildMenuItems(hasChapters)),
      title(title),
      currentPage(currentPage),
      totalPages(totalPages) {}

std::vector<XtcReaderMenuActivity::MenuItem> XtcReaderMenuActivity::buildMenuItems(bool hasChapters) {
  std::vector<MenuItem> items;
  items.reserve(6);
  if (hasChapters) {
    items.push_back({MenuAction::SELECT_CHAPTER, StrId::STR_SELECT_CHAPTER});
  }
  items.push_back({MenuAction::GO_TO_PERCENT, StrId::STR_GO_TO_PERCENT});
  items.push_back({MenuAction::SCREENSHOT, StrId::STR_SCREENSHOT_BUTTON});
  items.push_back({MenuAction::GO_HOME, StrId::STR_GO_HOME_BUTTON});
  if (gpio.deviceIsX3()) {
    items.push_back({MenuAction::TILT_PAGE_TURN, StrId::STR_TILT_PAGE_TURN});
  }
  return items;
}

void XtcReaderMenuActivity::onEnter() {
  Activity::onEnter();
  skipNextButtonCheck = true;
  requestUpdate();
}

void XtcReaderMenuActivity::onExit() { Activity::onExit(); }

void XtcReaderMenuActivity::loop() {
  if (skipNextButtonCheck) {
    const bool confirmCleared = !mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
                                !mappedInput.wasReleased(MappedInputManager::Button::Confirm);
    const bool backCleared = !mappedInput.isPressed(MappedInputManager::Button::Back) &&
                             !mappedInput.wasReleased(MappedInputManager::Button::Back);
    if (confirmCleared && backCleared) {
      skipNextButtonCheck = false;
    }
    return;
  }

  buttonNavigator.onNext([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, static_cast<int>(menuItems.size()));
    requestUpdate();
  });

  buttonNavigator.onPrevious([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, static_cast<int>(menuItems.size()));
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const auto selectedAction = menuItems[selectedIndex].action;

    if (selectedAction == MenuAction::TILT_PAGE_TURN) {
      SETTINGS.tiltPageTurn = !SETTINGS.tiltPageTurn;
      SETTINGS.saveToFile();
      requestUpdate();
      return;
    }

    setResult(MenuResult{static_cast<int>(selectedAction)});
    finish();
    return;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult cancelResult;
    cancelResult.isCancelled = true;
    setResult(std::move(cancelResult));
    finish();
    return;
  }
}

void XtcReaderMenuActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto pageWidth = renderer.getScreenWidth();
  // ボタンヒントが占める辺（縦持ちは下端、反転は上端、横向きは短辺側）を避ける
  const auto insets = GUI.getButtonHintInsets(renderer);
  const int contentX = insets.left;
  const int contentWidth = pageWidth - insets.left - insets.right;
  const int contentY = insets.top;

  // タイトル
  const std::string truncTitle =
      renderer.truncatedText(UI_12_FONT_ID, title.c_str(), contentWidth - 40, EpdFontFamily::BOLD);
  const int titleX =
      contentX + (contentWidth - renderer.getTextWidth(UI_12_FONT_ID, truncTitle.c_str(), EpdFontFamily::BOLD)) / 2;
  renderer.drawText(UI_12_FONT_ID, titleX, 15 + contentY, truncTitle.c_str(), true, EpdFontFamily::BOLD);

  // 進捗表示
  char progressBuf[64];
  if (totalPages > 0) {
    const int percent = static_cast<int>(static_cast<float>(currentPage) / totalPages * 100.0f + 0.5f);
    snprintf(progressBuf, sizeof(progressBuf), "%lu / %lu (%d%%)", static_cast<unsigned long>(currentPage + 1),
             static_cast<unsigned long>(totalPages), percent);
  } else {
    snprintf(progressBuf, sizeof(progressBuf), "%lu", static_cast<unsigned long>(currentPage + 1));
  }
  renderer.drawCenteredText(UI_10_FONT_ID, 45 + contentY, progressBuf);

  // メニュー項目
  const int startY = 75 + contentY;
  constexpr int lineHeight = 30;
  // 画面高さに収まる行数だけ描き、選択項目を含むページを表示する（EpubReaderMenuActivity と同じ）
  const int availableHeight = renderer.getScreenHeight() - startY - insets.bottom;
  const int pageItems = std::max(1, availableHeight / lineHeight);
  const int pageStart = selectedIndex / pageItems * pageItems;
  const int pageEnd = std::min(static_cast<int>(menuItems.size()), pageStart + pageItems);

  for (int i = pageStart; i < pageEnd; ++i) {
    const int displayY = startY + ((i - pageStart) * lineHeight);
    const bool isSelected = (i == selectedIndex);

    if (isSelected) {
      renderer.fillRect(contentX, displayY, contentWidth - 1, lineHeight, true);
    }

    renderer.drawText(UI_10_FONT_ID, contentX + 20, displayY, I18N.get(menuItems[i].labelId), !isSelected);

    const std::string value = getMenuItemValue(menuItems[i].action);
    if (!value.empty()) {
      const auto width = renderer.getTextWidth(UI_10_FONT_ID, value.c_str());
      renderer.drawText(UI_10_FONT_ID, contentX + contentWidth - 20 - width, displayY, value.c_str(), !isSelected);
    }
  }

  // ボタンヒント
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

std::string XtcReaderMenuActivity::getMenuItemValue(const MenuAction action) const {
  switch (action) {
    case MenuAction::TILT_PAGE_TURN:
      return SETTINGS.tiltPageTurn ? std::string(tr(STR_STATE_ON)) : std::string(tr(STR_STATE_OFF));
    default:
      return "";
  }
}
