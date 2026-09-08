#include "EpubReaderPercentSelectionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// Fine/coarse slider step sizes for percent adjustments.
constexpr int kSmallStep = 1;
constexpr int kLargeStep = 10;
}  // namespace

void EpubReaderPercentSelectionActivity::onEnter() {
  Activity::onEnter();
  // Set up rendering task and mark first frame dirty.
  requestUpdate();
}

void EpubReaderPercentSelectionActivity::onExit() { Activity::onExit(); }

void EpubReaderPercentSelectionActivity::adjustPercent(const int delta) {
  // Apply delta and clamp within 0-100.
  percent += delta;
  if (percent < 0) {
    percent = 0;
  } else if (percent > 100) {
    percent = 100;
  }
  requestUpdate();
}

void EpubReaderPercentSelectionActivity::loop() {
  // Back cancels, confirm selects, arrows adjust the percent.
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    setResult(PercentResult{percent});
    finish();
    return;
  }

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left}, [this] { adjustPercent(-kSmallStep); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right}, [this] { adjustPercent(kSmallStep); });

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::ValueIncrease},
                                       [this] { adjustPercent(kLargeStep); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::ValueDecrease},
                                       [this] { adjustPercent(-kLargeStep); });
}

void EpubReaderPercentSelectionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  // ボタンヒントが占める辺（縦持ちは下端、反転は上端、横向きは短辺側）を避ける
  const auto insets = GUI.getButtonHintInsets(renderer);
  const int contentX = insets.left;
  const int contentWidth = renderer.getScreenWidth() - insets.left - insets.right;
  const int hintGutterHeight = insets.top;

  // 中央揃えはヒント領域を除いたコンテンツ幅に対して行う
  auto drawCentered = [&](const int fontId, const int y, const char* text, const EpdFontFamily::Style style) {
    const int x = contentX + (contentWidth - renderer.getTextWidth(fontId, text, style)) / 2;
    renderer.drawText(fontId, x, y, text, true, style);
  };

  // Title and numeric percent value.
  drawCentered(UI_12_FONT_ID, 15 + hintGutterHeight, tr(STR_GO_TO_PERCENT), EpdFontFamily::BOLD);

  const std::string percentText = std::to_string(percent) + "%";
  drawCentered(UI_12_FONT_ID, 90 + hintGutterHeight, percentText.c_str(), EpdFontFamily::BOLD);

  // Draw slider track.
  constexpr int barWidth = 360;
  constexpr int barHeight = 16;
  const int barX = contentX + (contentWidth - barWidth) / 2;
  const int barY = 140 + hintGutterHeight;

  renderer.drawRect(barX, barY, barWidth, barHeight);

  // Fill slider based on percent.
  const int fillWidth = (barWidth - 4) * percent / 100;
  if (fillWidth > 0) {
    renderer.fillRect(barX + 2, barY + 2, fillWidth, barHeight - 4);
  }

  // Draw a simple knob centered at the current percent.
  const int knobX = barX + 2 + fillWidth - 2;
  renderer.fillRect(knobX, barY - 4, 4, barHeight + 8, true);

  // Hint text for step sizes.
  drawCentered(SMALL_FONT_ID, barY + 30, tr(STR_PERCENT_STEP_HINT), EpdFontFamily::REGULAR);

  // Button hints follow the current front button layout.
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "-", "+");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
