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
  const int maxWidth = renderer.getScreenWidth() - (margin * 2);

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

  startY = (renderer.getScreenHeight() - totalHeight) / 2;

  requestUpdate(true);
}

void ConfirmationActivity::render(RenderLock&& lock) {
  renderer.clearScreen();

  int currentY = startY;
  LOG_DBG("CONF", "currentY: %d", currentY);
  // Draw Heading
  if (!safeHeading.empty()) {
    renderer.drawCenteredText(fontId, currentY, safeHeading.c_str(), true, EpdFontFamily::BOLD);
    currentY += lineHeight + spacing;
  }

  // Draw Body
  if (!safeBody.empty()) {
    renderer.drawCenteredText(fontId, currentY, safeBody.c_str(), true, EpdFontFamily::REGULAR);
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