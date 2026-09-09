#include "QrDisplayActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/QrUtils.h"

void QrDisplayActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void QrDisplayActivity::onExit() { Activity::onExit(); }

void QrDisplayActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    finish();
    return;
  }
}

void QrDisplayActivity::render(RenderLock&&) {
  renderer.clearScreen();
  auto metrics = UITheme::getInstance().getMetrics();
  // ボタンヒントの領域を除いた矩形を基準にする（横向きではヒントが短辺側に来る）
  const Rect area = UITheme::getContentArea(renderer);

  GUI.drawHeader(renderer, Rect{area.x, area.y + metrics.topPadding, area.width, metrics.headerHeight},
                 tr(STR_DISPLAY_QR), nullptr);

  const int availableWidth = area.width - 40;
  const int startY = area.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  // 従来は下端のヒント高さ (40) を引いていた。area.height はヒント領域を含まないので
  // 上端からの距離と余白ぶんだけ引く
  const int availableHeight = area.y + area.height - startY - metrics.verticalSpacing;

  const Rect qrBounds(area.x + 20, startY, availableWidth, availableHeight);
  QrUtils::drawQrCode(renderer, qrBounds, textPayload);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
