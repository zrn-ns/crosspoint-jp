#include "GenerateAllCacheActivity.h"

#include <Epub.h>
#include <Epub/Section.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

// Recursively scan a directory for EPUB files
void findEpubFiles(const char* dirPath, std::vector<std::string>& results) {
  auto dir = Storage.open(dirPath);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return;
  }

  char name[256];
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    file.getName(name, sizeof(name));
    if (name[0] == '.') {
      file.close();
      continue;
    }

    std::string fullPath = std::string(dirPath);
    if (fullPath.back() != '/') fullPath += '/';
    fullPath += name;

    if (file.isDirectory()) {
      file.close();
      findEpubFiles(fullPath.c_str(), results);
    } else {
      if (FsHelpers::hasEpubExtension(std::string_view(name))) {
        results.push_back(fullPath);
      }
      file.close();
    }
  }
  dir.close();
}

}  // namespace

void GenerateAllCacheActivity::onEnter() {
  Activity::onEnter();
  state = CONFIRMING;
  requestUpdate();
}

void GenerateAllCacheActivity::onExit() { Activity::onExit(); }

void GenerateAllCacheActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 tr(STR_GENERATE_ALL_CACHE));

  if (state == CONFIRMING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 10, tr(STR_GENERATE_CACHE), true);

    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_CONFIRM), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == GENERATING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_GENERATING_ALL_CACHE));
    renderer.displayBuffer();
    return;
  }

  if (state == SUCCESS) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, tr(STR_CACHE_GENERATED), true, EpdFontFamily::BOLD);
    std::string resultText = std::to_string(processedCount) + " " + std::string(tr(STR_BOOKS_PROCESSED));
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, resultText.c_str());

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }
}

void GenerateAllCacheActivity::generateAllCaches() {
  LOG_DBG("GENALL", "Scanning for EPUB files...");

  std::vector<std::string> epubFiles;
  findEpubFiles("/", epubFiles);

  totalCount = epubFiles.size();
  processedCount = 0;

  LOG_DBG("GENALL", "Found %d EPUB files", totalCount);

  if (totalCount == 0) {
    state = SUCCESS;
    requestUpdate();
    return;
  }

  // Show progress popup
  Rect popupRect = GUI.drawPopup(renderer, tr(STR_GENERATING_ALL_CACHE));
  GUI.fillPopupProgress(renderer, popupRect, 0);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);

  // Calculate viewport dimensions
  int orientedMarginTop = 0, orientedMarginRight = 0, orientedMarginBottom = 0, orientedMarginLeft = 0;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom, &orientedMarginLeft);
  orientedMarginTop += SETTINGS.screenMargin;
  orientedMarginRight += SETTINGS.screenMargin;
  orientedMarginLeft += SETTINGS.screenMargin;
  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  orientedMarginBottom += std::max(SETTINGS.screenMargin, statusBarHeight);
  const uint16_t viewportWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
  const uint16_t viewportHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;

  for (int bookIdx = 0; bookIdx < totalCount; bookIdx++) {
    const auto& epubPath = epubFiles[bookIdx];
    LOG_DBG("GENALL", "Processing %d/%d: %s", bookIdx + 1, totalCount, epubPath.c_str());

    // Update progress
    const int progress = (bookIdx * 100) / totalCount;
    GUI.fillPopupProgress(renderer, popupRect, progress);
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);

    // Check for cancel (button held)
    const int adc1 = analogRead(1);
    const int adc2 = analogRead(2);
    if (adc1 < 3800 || adc2 < 3800) {
      LOG_DBG("GENALL", "Cancelled by user at book %d/%d", bookIdx + 1, totalCount);
      break;
    }

    // Load EPUB
    auto epub = std::make_shared<Epub>(epubPath, "/.crosspoint");
    if (!epub->load(true, SETTINGS.embeddedStyle == 0)) {
      LOG_ERR("GENALL", "Failed to load: %s", epubPath.c_str());
      continue;
    }

    const int spineCount = epub->getSpineItemsCount();
    if (spineCount <= 0) continue;

    // Check if already fully cached
    const std::string firstSectionPath = epub->getCachePath() + "/sections/0.bin";
    const std::string lastSectionPath =
        epub->getCachePath() + "/sections/" + std::to_string(spineCount - 1) + ".bin";
    if (Storage.exists(firstSectionPath.c_str()) && Storage.exists(lastSectionPath.c_str())) {
      processedCount++;
      continue;  // Already cached
    }

    // Resolve writing mode
    bool isVertical = false;
    if (SETTINGS.writingMode == CrossPointSettings::WM_VERTICAL) {
      isVertical = true;
    } else if (SETTINGS.writingMode == CrossPointSettings::WM_HORIZONTAL) {
      isVertical = false;
    } else {
      isVertical = epub->isPageProgressionRtl() &&
                   (epub->getLanguage() == "ja" || epub->getLanguage() == "jpn" ||
                    epub->getLanguage() == "zh" || epub->getLanguage() == "zho");
    }

    const float lineCompression = SETTINGS.getReaderLineCompression(isVertical);
    renderer.setVerticalCharSpacing(SETTINGS.getVerticalCharSpacingPercent());

    auto* fcm = renderer.getFontCacheManager();
    if (fcm) {
      fcm->clearCache();
      fcm->freeKernLigatureData();
    }

    const int headingFontIds[6] = {SETTINGS.getHeadingFontId(1), SETTINGS.getHeadingFontId(2), 0, 0, 0, 0};

    for (int i = 0; i < spineCount; i++) {
      Section sec(epub, i, renderer);
      if (sec.loadSectionFile(SETTINGS.getReaderFontId(), lineCompression, SETTINGS.extraParagraphSpacing,
                              SETTINGS.paragraphAlignment, viewportWidth, viewportHeight,
                              SETTINGS.hyphenationEnabled, SETTINGS.firstLineIndent, SETTINGS.embeddedStyle,
                              SETTINGS.imageRendering, isVertical)) {
        continue;
      }

      if (!sec.createSectionFile(SETTINGS.getReaderFontId(), lineCompression, SETTINGS.extraParagraphSpacing,
                                 SETTINGS.paragraphAlignment, viewportWidth, viewportHeight,
                                 SETTINGS.hyphenationEnabled, SETTINGS.firstLineIndent, SETTINGS.embeddedStyle,
                                 SETTINGS.imageRendering, isVertical, nullptr, headingFontIds,
                                 SETTINGS.getTableFontId())) {
        LOG_ERR("GENALL", "Failed section %d of %s", i, epubPath.c_str());
        continue;
      }

      // Generate image BMP caches
      const std::string imgPrefix = epub->getCachePath() + "/img_" + std::to_string(i) + "_";
      for (int j = 0; ; j++) {
        std::string jpgPath = imgPrefix + std::to_string(j) + ".jpg";
        if (!Storage.exists(jpgPath.c_str())) {
          jpgPath = imgPrefix + std::to_string(j) + ".jpeg";
          if (!Storage.exists(jpgPath.c_str())) break;
        }

        const size_t dotPos = jpgPath.rfind('.');
        const std::string bmpCachePath = jpgPath.substr(0, dotPos) + ".pxc.bmp";
        if (Storage.exists(bmpCachePath.c_str())) continue;

        FsFile jpegFile, bmpFile;
        if (Storage.openFileForRead("GEN", jpgPath, jpegFile) &&
            Storage.openFileForWrite("GEN", bmpCachePath, bmpFile)) {
          JpegToBmpConverter::jpegFileToBmpStreamWithSize(jpegFile, bmpFile, viewportWidth, viewportHeight);
          jpegFile.close();
          bmpFile.close();
        }
      }
    }

    processedCount++;
  }

  GUI.fillPopupProgress(renderer, popupRect, 100);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  delay(500);

  state = SUCCESS;
  requestUpdate();
}

void GenerateAllCacheActivity::loop() {
  if (state == CONFIRMING) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      {
        RenderLock lock(*this);
        state = GENERATING;
      }
      requestUpdateAndWait();
      generateAllCaches();
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      goBack();
    }
    return;
  }

  if (state == SUCCESS) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      goBack();
    }
    return;
  }
}
