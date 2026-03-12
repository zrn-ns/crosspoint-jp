#include "SdCardFontManager.h"

#include <EpdFontFamily.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <SdCardFont.h>
#include <SdCardFontRegistry.h>

SdCardFontManager::~SdCardFontManager() {
  for (auto& lf : loaded_) {
    delete lf.font;
  }
}

int SdCardFontManager::nextFontId_ = SD_FONT_ID_BASE;

int SdCardFontManager::generateFontId() {
  // Monotonic counter. Built-in font IDs are large hash values (see fontIds.h),
  // so small sequential IDs starting at SD_FONT_ID_BASE can never collide.
  return nextFontId_++;
}

bool SdCardFontManager::loadFamily(const SdCardFontFamilyInfo& family, GfxRenderer& renderer) {
  // Unload any previously loaded family first
  if (!loadedFamilyName_.empty()) {
    unloadAll(renderer);
  }

  // Load all font files from SD card
  loaded_.reserve(family.files.size());
  for (const auto& fileInfo : family.files) {
    auto* font = new (std::nothrow) SdCardFont();
    if (!font) {
      LOG_ERR("SDMGR", "Failed to allocate SdCardFont for %s", fileInfo.path.c_str());
      continue;
    }

    if (!font->load(fileInfo.path.c_str())) {
      LOG_ERR("SDMGR", "Failed to load %s", fileInfo.path.c_str());
      delete font;
      continue;
    }

    // Check for duplicate point size (e.g. two files for the same family at the same size)
    bool duplicate = false;
    for (const auto& lf : loaded_) {
      if (lf.size == fileInfo.pointSize) {
        LOG_ERR("SDMGR", "Duplicate size %u for family %s, skipping %s", fileInfo.pointSize, family.name.c_str(),
                fileInfo.path.c_str());
        duplicate = true;
        break;
      }
    }
    if (duplicate) {
      delete font;
      continue;
    }

    int fontId = generateFontId();
    // Guard against collision with built-in font IDs (shouldn't happen with
    // sequential IDs, but future-proofs against ID scheme changes)
    if (renderer.getFontMap().count(fontId) != 0) {
      LOG_ERR("SDMGR", "Font ID %d collides with existing font, skipping %s", fontId, fileInfo.path.c_str());
      delete font;
      continue;
    }
    renderer.registerSdCardFont(fontId, font);
    loaded_.push_back({font, fontId, fileInfo.pointSize});

    LOG_DBG("SDMGR", "Loaded %s size=%u id=%d styles=%u", fileInfo.path.c_str(), fileInfo.pointSize, fontId,
            font->styleCount());
  }

  if (loaded_.empty()) return false;

  // Build EpdFontFamily objects: each v4 SdCardFont has all styles
  for (const auto& lf : loaded_) {
    EpdFontFamily fontFamily(lf.font->getEpdFont(0), lf.font->getEpdFont(1), lf.font->getEpdFont(2),
                             lf.font->getEpdFont(3));
    renderer.insertFont(lf.fontId, fontFamily);
  }

  loadedFamilyName_ = family.name;
  return true;
}

void SdCardFontManager::unloadAll(GfxRenderer& renderer) {
  renderer.clearSdCardFonts();
  for (auto& lf : loaded_) {
    renderer.removeFont(lf.fontId);
    delete lf.font;
  }
  loaded_.clear();
  loadedFamilyName_.clear();
}

int SdCardFontManager::getFontId(const std::string& familyName, uint8_t size, uint8_t /*style*/) const {
  if (familyName != loadedFamilyName_) return 0;
  for (const auto& lf : loaded_) {
    if (lf.size == size) return lf.fontId;
  }
  return 0;
}
