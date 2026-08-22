#include "BootActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "CrossPointSettings.h"
#include "OtaRollback.h"
#include "fontIds.h"
#include "images/Logo120.h"

void BootActivity::onEnter() {
  Activity::onEnter();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  renderer.drawImage(Logo120, (pageWidth - 120) / 2, (pageHeight - 120) / 2, 120, 120);
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 70, tr(STR_CROSSPOINT), true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 95, tr(STR_BOOTING));
  const int versionY = pageHeight - 30;
  renderer.drawCenteredText(SMALL_FONT_ID, versionY, CROSSPOINT_VERSION);
  // 「設定 → 本体 → デバッグ表示」がONのときだけ、起動時の ota_state を出す。
  // OTA/SD更新の直後の初回起動が PENDING_VERIFY になっていれば、ロールバックの
  // 確認が setup() 完了まで遅延できている（src/OtaRollback.cpp 参照）。
  //
  // バージョン表記の下ではなく上に置く。画面下端は物理ベゼルに隠れて見切れる。
  if (SETTINGS.debugDisplay) {
    char buf[48];
    snprintf(buf, sizeof(buf), "ota=%s", ota_rollback::bootStateName());
    renderer.drawCenteredText(SMALL_FONT_ID, versionY - renderer.getLineHeight(SMALL_FONT_ID), buf);
  }
  renderer.displayBuffer();
}
