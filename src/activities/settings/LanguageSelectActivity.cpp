#include "LanguageSelectActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <iterator>

#include "I18nKeys.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

void LanguageSelectActivity::onEnter() {
  Activity::onEnter();

  // Set current selection based on current language
  const auto currentLang = static_cast<uint8_t>(I18N.getLanguage());
  const auto* begin = std::begin(SORTED_LANGUAGE_INDICES);
  const auto* end = std::end(SORTED_LANGUAGE_INDICES);
  const auto* it = std::find(begin, end, currentLang);
  selectedIndex = (it != end) ? std::distance(begin, it) : 0;

  requestUpdate();
}

void LanguageSelectActivity::onExit() { Activity::onExit(); }

void LanguageSelectActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    onBack();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }

  // Handle navigation
  buttonNavigator.onNextRelease([this] {
    selectedIndex = ButtonNavigator::nextIndex(static_cast<int>(selectedIndex), totalItems);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this] {
    selectedIndex = ButtonNavigator::previousIndex(static_cast<int>(selectedIndex), totalItems);
    requestUpdate();
  });
}

void LanguageSelectActivity::handleSelection() {
  {
    RenderLock lock(*this);
    I18N.setLanguage(static_cast<Language>(SORTED_LANGUAGE_INDICES[selectedIndex]));
  }

  // Return to previous page
  onBack();
}

void LanguageSelectActivity::render(RenderLock&&) {
  renderer.clearScreen();

  auto metrics = UITheme::getInstance().getMetrics();
  // ボタンヒント領域を除いたコンテンツ矩形（横向きではヒントが短辺側に来る）
  const Rect area = UITheme::getContentArea(renderer);

  GUI.drawHeader(renderer, Rect{area.x, area.y + metrics.topPadding, area.width, metrics.headerHeight},
                 tr(STR_LANGUAGE));

  // Current language marker
  const int contentTop = area.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = area.y + area.height - contentTop;
  const auto currentLang = static_cast<uint8_t>(I18N.getLanguage());
  GUI.drawList(
      renderer, Rect{area.x, contentTop, area.width, contentHeight}, totalItems, selectedIndex,
      [this](int index) { return I18N.getLanguageName(static_cast<Language>(SORTED_LANGUAGE_INDICES[index])); },
      nullptr, nullptr,
      [this, currentLang](int index) { return SORTED_LANGUAGE_INDICES[index] == currentLang ? tr(STR_SELECTED) : ""; },
      true);

  // Button hints
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
