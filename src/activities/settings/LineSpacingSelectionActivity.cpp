#include "LineSpacingSelectionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstdio>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int kSmallStep = 1;
constexpr int kLargeStep = 10;
}  // namespace

void LineSpacingSelectionActivity::onEnter() {
  ActivityWithSubactivity::onEnter();
  if (value < CrossPointSettings::LINE_SPACING_MIN) {
    value = CrossPointSettings::LINE_SPACING_MIN;
  } else if (value > CrossPointSettings::LINE_SPACING_MAX) {
    value = CrossPointSettings::LINE_SPACING_MAX;
  }
  requestUpdate();
}

void LineSpacingSelectionActivity::onExit() { ActivityWithSubactivity::onExit(); }

void LineSpacingSelectionActivity::adjustValue(const int delta) {
  value += delta;
  if (value < CrossPointSettings::LINE_SPACING_MIN) {
    value = CrossPointSettings::LINE_SPACING_MIN;
  } else if (value > CrossPointSettings::LINE_SPACING_MAX) {
    value = CrossPointSettings::LINE_SPACING_MAX;
  }
  requestUpdate();
}

void LineSpacingSelectionActivity::loop() {
  // This sub-page is opened from Settings on Confirm *press*.
  // Using release events here would consume the same key-up and immediately exit.
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    onCancel();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    onSelect(value);
    return;
  }

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left}, [this] { adjustValue(-kSmallStep); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right}, [this] { adjustValue(kSmallStep); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::ValueIncrease},
                                       [this] { adjustValue(kLargeStep); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::ValueDecrease},
                                       [this] { adjustValue(-kLargeStep); });
}

void LineSpacingSelectionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  // ボタンヒント領域を除いたコンテンツ矩形。反転では上端、横向きでは短辺側が除かれる
  const Rect area = UITheme::getContentArea(renderer);
  // ヒント領域を除いた幅で中央揃えする
  auto drawCentered = [&](const int fontId, const int y, const char* text,
                          const EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
    renderer.drawText(fontId, area.x + (area.width - renderer.getTextWidth(fontId, text, style)) / 2, y, text, true,
                      style);
  };

  drawCentered(UI_12_FONT_ID, area.y + 15, tr(STR_LINE_SPACING), EpdFontFamily::BOLD);

  char valueBuf[16];
  snprintf(valueBuf, sizeof(valueBuf), "%.2fx", static_cast<float>(value) / 100.0f);
  const std::string valueText = valueBuf;
  drawCentered(UI_12_FONT_ID, area.y + 90, valueText.c_str(), EpdFontFamily::BOLD);

  constexpr int barWidth = 360;
  constexpr int barHeight = 16;
  const int barX = area.x + (area.width - barWidth) / 2;
  const int barY = area.y + 140;

  renderer.drawRect(barX, barY, barWidth, barHeight);

  const int range = CrossPointSettings::LINE_SPACING_MAX - CrossPointSettings::LINE_SPACING_MIN;
  const int normalized = value - CrossPointSettings::LINE_SPACING_MIN;
  const int fillWidth = (barWidth - 4) * normalized / range;
  if (fillWidth > 0) {
    renderer.fillRect(barX + 2, barY + 2, fillWidth, barHeight - 4);
  }

  const int knobX = barX + 2 + fillWidth - 2;
  renderer.fillRect(knobX, barY - 4, 4, barHeight + 8, true);

  drawCentered(SMALL_FONT_ID, barY + 30, tr(STR_PERCENT_STEP_HINT));

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "-", "+");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
