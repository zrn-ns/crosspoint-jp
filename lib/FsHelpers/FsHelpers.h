#pragma once
#include <WString.h>

#include <string>
#include <string_view>

namespace FsHelpers {

std::string normalisePath(const std::string& path);

/**
 * Check if the given filename ends with the specified extension (case-insensitive).
 */
bool checkFileExtension(std::string_view fileName, const char* extension);
inline bool checkFileExtension(const String& fileName, const char* extension) {
  return checkFileExtension(std::string_view{fileName.c_str(), fileName.length()}, extension);
}

// Check for either .jpg or .jpeg extension (case-insensitive)
bool hasJpgExtension(std::string_view fileName);
inline bool hasJpgExtension(const String& fileName) {
  return hasJpgExtension(std::string_view{fileName.c_str(), fileName.length()});
}

// Check for .png extension (case-insensitive)
bool hasPngExtension(std::string_view fileName);
inline bool hasPngExtension(const String& fileName) {
  return hasPngExtension(std::string_view{fileName.c_str(), fileName.length()});
}

// Check for .bmp extension (case-insensitive)
bool hasBmpExtension(std::string_view fileName);

// Check for .gif extension (case-insensitive)
bool hasGifExtension(std::string_view fileName);
inline bool hasGifExtension(const String& fileName) {
  return hasGifExtension(std::string_view{fileName.c_str(), fileName.length()});
}

// Check for .epub extension (case-insensitive)
bool hasEpubExtension(std::string_view fileName);
inline bool hasEpubExtension(const String& fileName) {
  return hasEpubExtension(std::string_view{fileName.c_str(), fileName.length()});
}

// Check for either .xtc or .xtch extension (case-insensitive)
bool hasXtcExtension(std::string_view fileName);

// Check for .txt extension (case-insensitive)
bool hasTxtExtension(std::string_view fileName);
inline bool hasTxtExtension(const String& fileName) {
  return hasTxtExtension(std::string_view{fileName.c_str(), fileName.length()});
}

// Check for .md extension (case-insensitive)
bool hasMarkdownExtension(std::string_view fileName);

std::string extractFolderPath(const std::string& filePath);

// キャッシュディレクトリ名（epub_<n> など）に使うパスのハッシュ。
// 実機では std::hash<std::string> そのもの。ESP32 の libstdc++ (32bit) では MurmurHash2
// (seed 0xc70f6907) で、ホストシミュレータ（64bit libc++）ではまったく別の値になるため、
// CROSSPOINT_SIM ではその 32bit 実装を再現して実機と同じディレクトリ名を得る。
// 実機のバイナリには一切影響しない（std::hash の呼び出しがそのまま残る）。
#ifdef CROSSPOINT_SIM
inline size_t pathHash(const std::string& s) {
  // libstdc++ hash_bytes.cc の size_t==4 バイト版 std::_Hash_bytes を uint32_t で再現
  constexpr uint32_t m = 0x5bd1e995u;
  const size_t len = s.size();
  uint32_t hash = 0xc70f6907u ^ static_cast<uint32_t>(len);
  const unsigned char* buf = reinterpret_cast<const unsigned char*>(s.data());
  size_t remaining = len;
  while (remaining >= 4) {
    uint32_t k = static_cast<uint32_t>(buf[0]) | (static_cast<uint32_t>(buf[1]) << 8) |
                 (static_cast<uint32_t>(buf[2]) << 16) | (static_cast<uint32_t>(buf[3]) << 24);
    k *= m;
    k ^= k >> 24;
    k *= m;
    hash *= m;
    hash ^= k;
    buf += 4;
    remaining -= 4;
  }
  switch (remaining) {
    case 3:
      hash ^= static_cast<uint32_t>(buf[2]) << 16;
      [[fallthrough]];
    case 2:
      hash ^= static_cast<uint32_t>(buf[1]) << 8;
      [[fallthrough]];
    case 1:
      hash ^= static_cast<uint32_t>(buf[0]);
      hash *= m;
  }
  hash ^= hash >> 13;
  hash *= m;
  hash ^= hash >> 15;
  return hash;
}
#else
inline size_t pathHash(const std::string& s) { return std::hash<std::string>{}(s); }
#endif

}  // namespace FsHelpers
