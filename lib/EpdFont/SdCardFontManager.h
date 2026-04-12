#pragma once

#include <cstdint>
#include <string>
#include <vector>

class GfxRenderer;
class SdCardFont;
struct SdCardFontFamilyInfo;

class SdCardFontManager {
 public:
  SdCardFontManager() = default;
  ~SdCardFontManager();
  SdCardFontManager(const SdCardFontManager&) = delete;
  SdCardFontManager& operator=(const SdCardFontManager&) = delete;

  // Load .cpfont file(s) for the family.
  // Primary base: closest to preferredBasePt (body text size).
  // Secondary base: closest to headingBasePt (heading size), loaded only when
  // headingBasePt != 0 && headingBasePt != preferredBasePt && a different .cpfont exists.
  // Virtual font IDs for each target size use the closer base, eliminating upscaling.
  // Returns true if at least one font was loaded.
  bool loadFamily(const SdCardFontFamilyInfo& family, GfxRenderer& renderer, uint8_t preferredBasePt = 14,
                  uint8_t headingBasePt = 0);

  // Returns the point size of the currently loaded primary base .cpfont file (0 if none loaded).
  uint8_t loadedBasePt() const { return loadedBasePt_; }

  // Returns the point size of the currently loaded heading base .cpfont file (0 if none loaded).
  uint8_t loadedHeadingBasePt() const { return loadedHeadingBasePt_; }

  // Unload everything, unregister from renderer.
  void unloadAll(GfxRenderer& renderer);

  // Look up the font ID for a loaded family + size. Returns 0 if not found.
  int getFontId(const std::string& familyName, uint8_t size, uint8_t style = 0) const;

  // Get name of currently loaded family (empty if none).
  const std::string& currentFamilyName() const { return loadedFamilyName_; };

 private:
  struct LoadedFont {
    SdCardFont* font;  // heap-allocated, owned
    int fontId;
    uint8_t size;
  };
  static int computeFontId(uint32_t contentHash, const char* familyName, uint8_t pointSize);

  std::string loadedFamilyName_;
  std::vector<LoadedFont> loaded_;
  std::vector<int> virtualFontIds_;      // 全仮想fontId（unload時に全削除用）
  uint8_t loadedBasePt_ = 0;            // 現在ロード中のprimaryベース.cpfontのptサイズ
  uint8_t loadedHeadingBasePt_ = 0;     // 現在ロード中のheadingベース.cpfontのptサイズ（0=未使用）
};
