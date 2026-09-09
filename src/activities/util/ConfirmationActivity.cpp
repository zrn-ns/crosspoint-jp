#include "ConfirmationActivity.h"

#include <I18n.h>

#include "../../components/UITheme.h"
#include "../ActivityResult.h"
#include "HalDisplay.h"

ConfirmationActivity::ConfirmationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           const std::string& heading, const std::string& body,
                                           const std::string& neverLabel, const std::string& confirmLabel,
                                           const std::string& backLabel, const std::string& confirmMiddleLabel)
    : Activity("Confirmation", renderer, mappedInput),
      heading(heading),
      body(body),
      neverLabel(neverLabel),
      confirmLabel(confirmLabel),
      backLabel(backLabel),
      confirmMiddleLabel(confirmMiddleLabel) {}

void ConfirmationActivity::onEnter() {
  Activity::onEnter();

  lineHeight = renderer.getLineHeight(fontId);
  // ボタンヒントの領域を除いた矩形に収める（横向きではヒントが短辺側に来る）
  const Rect area = UITheme::getContentArea(renderer);
  const int maxWidth = area.width - (margin * 2);

  if (!heading.empty()) {
    safeHeading = renderer.truncatedText(fontId, heading.c_str(), maxWidth, EpdFontFamily::BOLD);
  }
  if (!body.empty()) {
    safeBody = renderer.truncatedText(fontId, body.c_str(), maxWidth, EpdFontFamily::REGULAR);
  }

  int totalHeight = 0;
  if (!safeHeading.empty()) totalHeight += lineHeight;
  if (!safeBody.empty()) totalHeight += lineHeight;
  if (!safeHeading.empty() && !safeBody.empty()) totalHeight += spacing;

  startY = area.y + (area.height - totalHeight) / 2;

  requestUpdate(true);
}

void ConfirmationActivity::render(RenderLock&& lock) {
  renderer.clearScreen();

  int currentY = startY;
  LOG_DBG("CONF", "currentY: %d", currentY);
  // ヒント領域を除いた幅で中央揃えする
  const Rect area = UITheme::getContentArea(renderer);
  auto drawCentered = [&](const int y, const char* text, const EpdFontFamily::Style style) {
    const int x = area.x + (area.width - renderer.getTextWidth(fontId, text, style)) / 2;
    renderer.drawText(fontId, x, y, text, true, style);
  };
  // Draw Heading
  if (!safeHeading.empty()) {
    drawCentered(currentY, safeHeading.c_str(), EpdFontFamily::BOLD);
    currentY += lineHeight + spacing;
  }

  // Draw Body
  if (!safeBody.empty()) {
    drawCentered(currentY, safeBody.c_str(), EpdFontFamily::REGULAR);
  }

  // Draw UI Elements
  const char* confirmText = confirmLabel.empty() ? I18N.get(StrId::STR_CONFIRM) : confirmLabel.c_str();
  const char* backText =
      backLabel.empty() ? (neverLabel.empty() ? "" : I18N.get(StrId::STR_CLOSE_BOOK)) : backLabel.c_str();
  const char* middleText = confirmMiddleLabel.empty() ? "" : confirmMiddleLabel.c_str();
  const auto labels = neverLabel.empty()
                          ? mappedInput.mapLabels(backText, middleText, I18N.get(StrId::STR_CANCEL), confirmText)
                          : mappedInput.mapLabels(backText, middleText, neverLabel.c_str(), confirmText);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(HalDisplay::RefreshMode::FAST_REFRESH);
}

void ConfirmationActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    ActivityResult res;
    res.isCancelled = false;
    setResult(std::move(res));
    finish();
    return;
  }

  if (!confirmMiddleLabel.empty() && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    ActivityResult res;
    res.isCancelled = true;
    res.data = MenuResult{RESULT_MIDDLE};
    setResult(std::move(res));
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    if (!neverLabel.empty()) {
      // "Never" option (don't ask again)
      ActivityResult res;
      res.isCancelled = true;
      res.data = MenuResult{RESULT_NEVER};
      setResult(std::move(res));
    } else {
      // Standard cancel
      ActivityResult res;
      res.isCancelled = true;
      setResult(std::move(res));
    }
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    // Close / cancel
    ActivityResult res;
    res.isCancelled = true;
    setResult(std::move(res));
    finish();
    return;
  }
}